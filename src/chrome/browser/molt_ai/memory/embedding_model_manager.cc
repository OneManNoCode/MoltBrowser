// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/embedding_model_manager.h"

#include <algorithm>
#include <utility>

#include "base/cpu.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/models/download_policy.h"
#include "chrome/browser/molt_ai/models/model_download_job.h"
#include "chrome/browser/molt_ai/runtime/embedding_process_host.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;
// As for chat models: a model may never need more than this share of RAM.
constexpr double kMaxShareOfTotalRam = 0.9;

base::FilePath DefaultModelDir() {
  return base::GetHomeDir().AppendASCII(".moltbrowser").AppendASCII("models");
}

}  // namespace

// static
EmbeddingModelManager* EmbeddingModelManager::GetInstance() {
  static base::NoDestructor<EmbeddingModelManager> instance;
  return instance.get();
}

EmbeddingModelManager::EmbeddingModelManager()
    : spec_(DefaultEmbeddingModel()) {}

EmbeddingModelManager::~EmbeddingModelManager() = default;

void EmbeddingModelManager::SetSpecForTesting(const EmbeddingModelSpec& spec) {
  spec_ = spec;
}

void EmbeddingModelManager::SetModelDirForTesting(const base::FilePath& dir) {
  model_dir_ = dir;
}

base::FilePath EmbeddingModelManager::ModelPath() const {
  return model_dir_.AppendASCII(spec_.filename);
}

std::string EmbeddingModelManager::NotOfferedReason() const {
  if (!IsPinned(spec_)) {
    return "The on-device search model isn't available in this version of "
           "MoltBrowser yet.";
  }
#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_IOS)
  return "On-device AI isn't available in this version of MoltBrowser.";
#else
#if defined(ARCH_CPU_X86_64)
  // The inference service's kernels need x86-64-v2 at least (see
  // BrowserAIRuntime).
  const base::CPU& cpu = base::CPU::GetInstanceNoAllocation();
  if (!cpu.has_sse3() || !cpu.has_ssse3() || !cpu.has_sse41() ||
      !cpu.has_sse42() || !cpu.has_popcnt()) {
    return "This CPU doesn't support SSE4.2, which on-device AI in "
           "MoltBrowser needs.";
  }
#endif
  const uint64_t total = static_cast<uint64_t>(std::max<int64_t>(
      0, base::SysInfo::AmountOfTotalPhysicalMemory().InBytes()));
  if (static_cast<double>(spec_.ram_required_bytes) >
      static_cast<double>(total) * kMaxShareOfTotalRam) {
    return std::string("The on-device search model needs about ") +
           base::NumberToString((spec_.ram_required_bytes + kGiB - 1) / kGiB) +
           " GB of memory, more than this device has.";
  }
  return std::string();
#endif
}

void EmbeddingModelManager::Refresh() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint64_t refresh = ++refresh_;
  const std::string reason = NotOfferedReason();
  if (!reason.empty()) {
    status_.reason = reason;
    SetState(State::kNotOffered);
    return;
  }
  if (status_.state == State::kDownloading ||
      status_.state == State::kRemoving) {
    return;  // they end with a Refresh() of their own
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath dir, std::string filename, uint64_t size) {
            DiskState disk;
            disk.dir = dir.value().empty() ? DefaultModelDir() : dir;
            // Exactly the pinned size; anything else is never loaded (a
            // finished download was hash-checked before its rename).
            const std::optional<int64_t> on_disk =
                base::GetFileSize(disk.dir.AppendASCII(filename));
            disk.installed = on_disk.has_value() && *on_disk >= 0 &&
                             static_cast<uint64_t>(*on_disk) == size;
            return disk;
          },
          model_dir_, std::string(spec_.filename), spec_.file_size_bytes),
      base::BindOnce(&EmbeddingModelManager::OnRefreshed,
                     weak_factory_.GetWeakPtr(), refresh));
}

void EmbeddingModelManager::OnRefreshed(uint64_t refresh, DiskState disk) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (refresh != refresh_ || status_.state == State::kDownloading ||
      status_.state == State::kRemoving) {
    return;
  }
  model_dir_ = disk.dir;
  status_.reason.clear();
  SetState(disk.installed ? State::kInstalled : State::kNotInstalled);
}

void EmbeddingModelManager::StartDownload(
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    base::OnceCallback<void(std::string error)> done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (status_.state != State::kNotInstalled || model_dir_.value().empty()) {
    std::move(done).Run(status_.state == State::kInstalled
                            ? std::string()
                            : "The search model can't be downloaded now.");
    return;
  }
  if (!factory) {
    std::move(done).Run("No network connection is available.");
    return;
  }
  status_.error.clear();
  status_.received_bytes = 0;
  status_.total_bytes = static_cast<int64_t>(spec_.file_size_bytes);
  SetState(State::kDownloading);
  // Disk checks on a worker (never on the UI thread).
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath final_path, uint64_t size) -> std::string {
            const base::FilePath dir = final_path.DirName();
            if (!base::DirectoryExists(dir) && !base::CreateDirectory(dir)) {
              return "Couldn't create the models folder.";
            }
            const int64_t partial =
                base::GetFileSize(
                    models::ModelDownloadJob::PartialPathFor(final_path))
                    .value_or(0);
            const int64_t needed =
                std::max<int64_t>(0, static_cast<int64_t>(size) - partial);
            const int64_t free_space =
                base::SysInfo::AmountOfFreeDiskSpace(dir).value_or(-1);
            if (free_space >= 0 && free_space < needed) {
              return "Not enough disk space: the search model needs " +
                     base::NumberToString(needed / (1024 * 1024)) + " MB.";
            }
            return std::string();
          },
          ModelPath(), spec_.file_size_bytes),
      base::BindOnce(&EmbeddingModelManager::OnDownloadPrecheck,
                     weak_factory_.GetWeakPtr(), std::move(factory),
                     std::move(done)));
}

void EmbeddingModelManager::OnDownloadPrecheck(
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    base::OnceCallback<void(std::string)> done,
    std::string error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (status_.state != State::kDownloading) {
    std::move(done).Run("The download was cancelled.");
    return;
  }
  if (!error.empty()) {
    status_.error = error;
    SetState(State::kNotInstalled);
    std::move(done).Run(error);
    return;
  }
  // Fail closed: only the pinned revision, rejected unless its size and
  // SHA-256 match (the model is never offered unpinned).
  models::ModelDownloadJob::Params params;
  params.url = GURL(models::ModelDownloadUrl(spec_.huggingface_id,
                                             spec_.revision, spec_.filename));
  params.final_path = ModelPath();
  params.expected_sha256 = spec_.sha256;
  params.expected_size = static_cast<int64_t>(spec_.file_size_bytes);
  LOG(INFO) << "[MoltMemory] downloading the search model from "
            << params.url.spec();
  download_ = models::ModelDownloadJob::Start(
      std::move(params), std::move(factory),
      base::BindRepeating(&EmbeddingModelManager::OnDownloadProgress,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(
          [](base::WeakPtr<EmbeddingModelManager> self,
             base::OnceCallback<void(std::string)> done,
             models::ModelDownloadJob::Result result) {
            if (!self) return;
            // Not inside the job's own callback.
            base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
                FROM_HERE, std::move(self->download_));
            if (!result.success || !result.verified) {
              self->status_.error = result.error.empty()
                                        ? "The download failed."
                                        : result.error;
              self->SetState(State::kNotInstalled);
              std::move(done).Run(self->status_.error);
              return;
            }
            LOG(INFO) << "[MoltMemory] search model downloaded and verified";
            self->status_.state = State::kNotInstalled;  // Refresh() decides
            self->Refresh();
            std::move(done).Run(std::string());
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void EmbeddingModelManager::OnDownloadProgress(int64_t received,
                                               int64_t total) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  status_.received_bytes = received;
  if (total > 0) status_.total_bytes = total;
}

void EmbeddingModelManager::CancelDownload() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (status_.state != State::kDownloading) return;
  if (download_) {
    download_->CancelAndDiscard();
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(download_));
  }
  SetState(State::kNotInstalled);
}

void EmbeddingModelManager::Remove(base::OnceCallback<void(bool ok)> done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (status_.state != State::kInstalled) {
    std::move(done).Run(status_.state != State::kDownloading &&
                        status_.state != State::kRemoving);
    return;
  }
  // Memory switches back to keyword matching first (observers), then the
  // process ends, then the file goes.
  SetState(State::kRemoving);
  EmbeddingProcessHost::GetInstance()->Unload(base::BindOnce(
      [](base::WeakPtr<EmbeddingModelManager> self,
         base::OnceCallback<void(bool)> done) {
        if (!self) return;
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE,
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
             base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
            base::BindOnce(&base::DeleteFile, self->ModelPath()),
            base::BindOnce(
                [](base::WeakPtr<EmbeddingModelManager> self,
                   base::OnceCallback<void(bool)> done, bool ok) {
                  if (!self) return;
                  if (!ok) self->status_.error = "Couldn't delete the file.";
                  self->status_.state = State::kNotInstalled;
                  self->Refresh();
                  std::move(done).Run(ok);
                },
                self, std::move(done)));
      },
      weak_factory_.GetWeakPtr(), std::move(done)));
}

void EmbeddingModelManager::SetState(State state) {
  if (status_.state == state) return;
  status_.state = state;
  for (Observer& observer : observers_) observer.OnEmbeddingModelChanged();
}

void EmbeddingModelManager::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
}

void EmbeddingModelManager::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

}  // namespace memory
}  // namespace molt_ai
