// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/models/model_download_job.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/models/download_policy.h"
#include "crypto/hash.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace molt_ai {
namespace models {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("molt_ai_model_download", R"(
        semantics {
          sender: "MoltBrowser AI"
          description: "Downloads an on-device AI model file (GGUF) from "
            "Hugging Face, pinned to a specific repository revision."
          trigger: "User picks a model to download in the AI chat."
          data: "HTTP GET (optionally with a Range header to resume)."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: NO
          setting: "User-initiated model download in MoltBrowser AI."
        }
      )");

constexpr size_t kRehashChunk = 1 << 20;

}  // namespace

// Owns the .partial file and the running SHA-256. Lives on the job's
// blocking file sequence only.
class ModelDownloadJob::Writer {
 public:
  explicit Writer(base::FilePath partial)
      : partial_(std::move(partial)), hasher_(crypto::hash::kSha256) {}

  // Opens (creating) the .partial file and re-hashes the bytes already in
  // it, so a resumed download's final hash covers the whole file. Returns
  // the bytes on disk, or -1 on IO failure.
  int64_t Open(int64_t expected_size) {
    if (!base::CreateDirectory(partial_.DirName())) return -1;
    file_ = base::File(partial_, base::File::FLAG_OPEN_ALWAYS |
                                     base::File::FLAG_READ |
                                     base::File::FLAG_WRITE);
    if (!file_.IsValid()) return -1;
    const int64_t len = file_.GetLength();
    if (len < 0) return -1;
    if (expected_size > 0 && len >= expected_size) {
      // A complete-but-never-renamed (or oversized) leftover: start clean
      // rather than ask the server for an empty range.
      return Truncate() ? 0 : -1;
    }
    std::vector<uint8_t> buf(kRehashChunk);
    int64_t off = 0;
    while (off < len) {
      const size_t want =
          static_cast<size_t>(std::min<int64_t>(buf.size(), len - off));
      std::optional<size_t> n =
          file_.Read(off, base::span<uint8_t>(buf).first(want));
      if (!n || *n == 0) {
        // Unreadable leftover: start over.
        return Truncate() ? 0 : -1;
      }
      hasher_.Update(base::span<const uint8_t>(buf).first(*n));
      off += static_cast<int64_t>(*n);
    }
    size_ = len;
    return len;
  }

  // Closes and deletes the .partial file (user cancelled), then
  // |also_delete|.
  void Discard(const std::vector<base::FilePath>& also_delete) {
    file_.Close();
    base::DeleteFile(partial_);
    for (const base::FilePath& path : also_delete) {
      base::DeleteFile(path);
    }
  }

  bool Truncate() {
    if (!file_.IsValid() || !file_.SetLength(0)) return false;
    hasher_ = crypto::hash::Hasher(crypto::hash::kSha256);
    size_ = 0;
    return true;
  }

  bool Append(const std::string& data) {
    if (!file_.IsValid()) return false;
    std::optional<size_t> n = file_.Write(size_, base::as_byte_span(data));
    if (!n || *n != data.size()) return false;
    hasher_.Update(base::as_byte_span(data));
    size_ += static_cast<int64_t>(*n);
    return true;
  }

  // Closes the file, checks size + hash, and renames .partial -> final on
  // success. A mismatching file is deleted (it can't be resumed into a
  // correct one).
  Result Finish(const std::string& expected_sha256,
                int64_t expected_size,
                const base::FilePath& final_path) {
    Result r;
    if (!file_.IsValid() || !file_.Flush()) {
      r.error = "Couldn't finish writing the model file.";
      return r;
    }
    file_.Close();
    std::array<uint8_t, crypto::hash::kSha256Size> digest;
    hasher_.Finish(digest);
    r.sha256 = base::HexEncodeLower(digest);

    switch (VerifyDownload(r.sha256, size_, expected_sha256, expected_size)) {
      case VerifyResult::kSizeMismatch:
        base::DeleteFile(partial_);
        r.error = "The downloaded file has the wrong size (" +
                  base::NumberToString(size_) + " bytes, expected " +
                  base::NumberToString(expected_size) +
                  "). It was discarded — please try again.";
        return r;
      case VerifyResult::kHashMismatch:
        base::DeleteFile(partial_);
        r.error =
            "The downloaded file doesn't match the published checksum, so it "
            "was discarded. Please try again; if this keeps happening the "
            "file on the server may have changed.";
        return r;
      case VerifyResult::kVerified:
        r.verified = true;
        break;
      case VerifyResult::kUnverified:
        LOG(WARNING) << "[MoltAI] model download is UNVERIFIED (no pinned "
                        "SHA-256 in the registry): "
                     << final_path.BaseName().AsUTF8Unsafe()
                     << " sha256=" << r.sha256;
        break;
    }
    if (!base::ReplaceFile(partial_, final_path, nullptr)) {
      r.error = "Couldn't move the downloaded model into place.";
      return r;
    }
    r.success = true;
    return r;
  }

 private:
  const base::FilePath partial_;
  base::File file_;
  crypto::hash::Hasher hasher_;
  int64_t size_ = 0;
};

// static
base::FilePath ModelDownloadJob::PartialPathFor(
    const base::FilePath& final_path) {
  return final_path.AddExtension(FILE_PATH_LITERAL("partial"));
}

// static
std::unique_ptr<ModelDownloadJob> ModelDownloadJob::Start(
    Params params,
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    ProgressCallback progress,
    DoneCallback done) {
  return base::WrapUnique(new ModelDownloadJob(std::move(params),
                                               std::move(factory),
                                               std::move(progress),
                                               std::move(done)));
}

ModelDownloadJob::ModelDownloadJob(
    Params params,
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    ProgressCallback progress,
    DoneCallback done)
    : params_(std::move(params)),
      factory_(std::move(factory)),
      progress_(std::move(progress)),
      done_(std::move(done)),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
      writer_(std::make_unique<Writer>(PartialPathFor(params_.final_path))) {
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Writer::Open, base::Unretained(writer_.get()),
                     params_.expected_size),
      base::BindOnce(&ModelDownloadJob::OnPartialOpened,
                     weak_factory_.GetWeakPtr()));
}

ModelDownloadJob::~ModelDownloadJob() {
  loader_.reset();
  // Queued behind every pending write, so the raw Writer pointers bound
  // into those tasks stay valid until they have run.
  if (writer_) file_runner_->DeleteSoon(FROM_HERE, std::move(writer_));
}

void ModelDownloadJob::CancelAndDiscard(
    std::vector<base::FilePath> also_delete) {
  loader_.reset();
  done_.Reset();
  // Ordered after any pending write (or Finish's rename) on the file
  // sequence.
  file_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&Writer::Discard, base::Unretained(writer_.get()),
                     std::move(also_delete)));
}

void ModelDownloadJob::OnPartialOpened(int64_t existing_bytes) {
  if (existing_bytes < 0) {
    Fail("Couldn't write the model file. Check free disk space.", 0);
    return;
  }
  requested_offset_ = existing_bytes;
  written_ = existing_bytes;
  if (progress_) progress_.Run(written_, params_.expected_size);

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = params_.url;
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  if (existing_bytes > 0) {
    request->headers.SetHeader(
        "Range", "bytes=" + base::NumberToString(existing_bytes) + "-");
  }
  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             kTrafficAnnotation);
  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &ModelDownloadJob::OnResponseStarted, weak_factory_.GetWeakPtr()));
  loader_->DownloadAsStream(factory_.get(), this);
}

void ModelDownloadJob::OnResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  const int status = head.headers ? head.headers->response_code() : 0;
  int64_t range_first = -1;
  if (status == 206 && head.headers) {
    int64_t first = -1;
    int64_t last = -1;
    int64_t length = -1;
    if (head.headers->GetContentRangeFor206(&first, &last, &length)) {
      range_first = first;
    }
  }
  switch (DecideResumeAction(requested_offset_, status, range_first)) {
    case ResumeAction::kAppend:
      response_ok_ = true;
      return;
    case ResumeAction::kRestart:
      // The server sent the whole file: rewrite from byte 0. Posted before
      // any chunk of this body, so it runs first on the file sequence.
      response_ok_ = true;
      written_ = 0;
      file_runner_->PostTask(FROM_HERE,
                             base::BindOnce(base::IgnoreResult(&Writer::Truncate),
                                            base::Unretained(writer_.get())));
      return;
    case ResumeAction::kFail:
      // SimpleURLLoader runs this callback before its own HTTP-error check,
      // so every non-2xx status ends here (OnComplete never sees one).
      loader_.reset();
      if (status == 416 || status == 206) {
        // "Range not satisfiable", or a range that doesn't start where we
        // asked: the leftover doesn't match the file on the server. Start
        // from scratch on the next attempt.
        file_runner_->PostTask(
            FROM_HERE, base::BindOnce(base::IgnoreResult(&Writer::Truncate),
                                      base::Unretained(writer_.get())));
      }
      Fail("Download failed (HTTP " + base::NumberToString(status) + ").", 0);
      return;
  }
}

void ModelDownloadJob::OnDataReceived(std::string_view data,
                                      base::OnceClosure resume) {
  if (!response_ok_) return;  // OnResponseStarted already failed the job
  const int64_t size = static_cast<int64_t>(data.size());
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Writer::Append, base::Unretained(writer_.get()),
                     std::string(data)),
      base::BindOnce(
          [](base::WeakPtr<ModelDownloadJob> self, base::OnceClosure resume,
             int64_t size, bool ok) {
            if (!self) return;
            if (ok) self->written_ += size;
            self->OnChunkWritten(std::move(resume), ok);
          },
          weak_factory_.GetWeakPtr(), std::move(resume), size));
}

void ModelDownloadJob::OnChunkWritten(base::OnceClosure resume, bool ok) {
  if (!ok) {
    loader_.reset();
    Fail("Couldn't write the model file. Check free disk space.", 0);
    return;
  }
  if (progress_) progress_.Run(written_, params_.expected_size);
  std::move(resume).Run();
}

void ModelDownloadJob::OnComplete(bool success) {
  if (!success) {
    // HTTP errors were already handled in OnResponseStarted(); this is a
    // network failure (before or during the body). The .partial is kept.
    const int net_error = loader_ ? loader_->NetError() : 0;
    loader_.reset();
    Fail("Download failed (network error " + base::NumberToString(net_error) +
             "). It will resume where it stopped.",
         net_error);
    return;
  }
  loader_.reset();
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&Writer::Finish, base::Unretained(writer_.get()),
                     params_.expected_sha256, params_.expected_size,
                     params_.final_path),
      base::BindOnce(&ModelDownloadJob::OnFinished,
                     weak_factory_.GetWeakPtr()));
}

void ModelDownloadJob::OnRetry(base::OnceClosure start_retry) {
  // Retries aren't enabled; if they ever are, the retried response is a
  // full body: rewrite from byte 0 first (ordered on the file sequence).
  written_ = 0;
  requested_offset_ = 0;
  file_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(base::IgnoreResult(&Writer::Truncate),
                     base::Unretained(writer_.get())),
      std::move(start_retry));
}

void ModelDownloadJob::OnFinished(Result result) {
  if (done_) std::move(done_).Run(std::move(result));
}

void ModelDownloadJob::Fail(std::string error, int net_error) {
  Result r;
  r.error = std::move(error);
  r.net_error = net_error;
  if (done_) std::move(done_).Run(std::move(r));
}

}  // namespace models
}  // namespace molt_ai
