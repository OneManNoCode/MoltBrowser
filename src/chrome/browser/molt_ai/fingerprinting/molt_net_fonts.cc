// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_fonts.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_net_fonts_table.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

namespace molt_ai::fingerprinting {

namespace {

enum class LoadState { kNotStarted, kLoading, kReady, kAbsent, kBroken };

struct LoadedFonts {
  LoadState state = LoadState::kNotStarted;
  std::vector<base::File> files;
  // Renderers launched while the set was being opened.
  std::vector<MoltNetFontsCallback> waiting;
};

LoadedFonts& Loaded() {
  static base::NoDestructor<LoadedFonts> loaded;
  return *loaded;
}

void OnLoaded(MoltNetFontsLoad load) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LoadedFonts& loaded = Loaded();
  std::vector<MoltNetFontsCallback> waiting = std::move(loaded.waiting);
  loaded.waiting.clear();
  switch (load.status) {
    case MoltNetFontsStatus::kReady:
      loaded.files = std::move(load.files);
      loaded.state = LoadState::kReady;
      for (MoltNetFontsCallback& deliver : waiting) {
        std::move(deliver).Run(DuplicateMoltNetFonts());
      }
      return;
    case MoltNetFontsStatus::kAbsent:
      loaded.state = LoadState::kAbsent;
      break;
    case MoltNetFontsStatus::kBroken:
      loaded.state = LoadState::kBroken;
      break;
  }
  if (!waiting.empty()) {
    // They expect the set and keep drawing text without fonts.
    LOG(ERROR) << "MoltNet fonts: no set for " << waiting.size()
               << " MoltNet renderer(s) waiting for it";
  }
}

}  // namespace

MoltNetFontsLoad::MoltNetFontsLoad() = default;
MoltNetFontsLoad::MoltNetFontsLoad(MoltNetFontsStatus status,
                                   std::vector<base::File> files)
    : status(status), files(std::move(files)) {}
MoltNetFontsLoad::MoltNetFontsLoad(MoltNetFontsLoad&&) = default;
MoltNetFontsLoad& MoltNetFontsLoad::operator=(MoltNetFontsLoad&&) = default;
MoltNetFontsLoad::~MoltNetFontsLoad() = default;

base::FilePath GetMoltNetFontsDirectory() {
#if BUILDFLAG(IS_MAC)
  // MoltBrowser.app/Contents/MacOS/MoltBrowser ->
  // MoltBrowser.app/Contents/Resources/molt_fonts/
  base::FilePath exe;
  if (!base::PathService::Get(base::FILE_EXE, &exe)) {
    return base::FilePath();
  }
  return exe.DirName()
      .DirName()
      .AppendASCII("Resources")
      .Append(kMoltNetFontsFolder);
#else
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return base::FilePath();
  }
  return exe_dir.Append(kMoltNetFontsFolder);
#endif
}

bool IsExpectedMoltNetFontFile(size_t index,
                               int64_t length,
                               base::span<const uint8_t> head) {
  if (index >= std::size(kMoltNetFontFiles) || length < 0 ||
      static_cast<uint64_t>(length) != kMoltNetFontFiles[index].size ||
      head.size() < 4) {
    return false;
  }
  const uint32_t tag = (uint32_t{head[0]} << 24) | (uint32_t{head[1]} << 16) |
                       (uint32_t{head[2]} << 8) | uint32_t{head[3]};
  return tag == 0x00010000u || tag == 0x4f54544fu /* OTTO */ ||
         tag == 0x74727565u /* true */ || tag == 0x74746366u /* ttcf */;
}

std::optional<std::vector<base::File>> OpenMoltNetFonts(
    const base::FilePath& directory) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  if (directory.empty()) {
    return std::nullopt;
  }
  std::vector<base::File> files;
  files.reserve(std::size(kMoltNetFontFiles));
  for (size_t i = 0; i < std::size(kMoltNetFontFiles); ++i) {
    const base::FilePath path =
        directory.AppendASCII(kMoltNetFontFiles[i].name);
    base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
    std::array<uint8_t, 4> head{};
    const std::optional<size_t> read =
        file.IsValid() ? file.Read(0, head) : std::nullopt;
    if (!file.IsValid() || !read || *read != head.size() ||
        !IsExpectedMoltNetFontFile(i, file.GetLength(), head)) {
      LOG(ERROR) << "MoltNet fonts: " << path
                 << " is missing or not the pinned file (set "
                 << kMoltNetFontSetVersion
                 << "); MoltNet windows have no fonts until it is restored";
      return std::nullopt;
    }
    files.push_back(std::move(file));
  }
  return files;
}

MoltNetFontsLoad LoadMoltNetFonts(const base::FilePath& directory) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  if (directory.empty() || !base::DirectoryExists(directory)) {
    LOG(WARNING) << "MoltNet fonts: no " << directory
                 << "; MoltNet windows use the OS's font allow-list";
    return MoltNetFontsLoad(MoltNetFontsStatus::kAbsent, {});
  }
  std::optional<std::vector<base::File>> files = OpenMoltNetFonts(directory);
  if (!files) {
    return MoltNetFontsLoad(MoltNetFontsStatus::kBroken, {});
  }
  return MoltNetFontsLoad(MoltNetFontsStatus::kReady, std::move(*files));
}

void StartLoadingMoltNetFontsAfterStartup() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // Best-effort UI tasks wait until startup is complete.
  content::GetUIThreadTaskRunner({base::TaskPriority::BEST_EFFORT})
      ->PostTask(FROM_HERE, base::BindOnce(&StartLoadingMoltNetFonts));
}

void StartLoadingMoltNetFonts() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LoadedFonts& loaded = Loaded();
  if (loaded.state != LoadState::kNotStarted) {
    return;
  }
  loaded.state = LoadState::kLoading;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&LoadMoltNetFonts, GetMoltNetFontsDirectory()),
      base::BindOnce(&OnLoaded));
}

bool AreMoltNetFontsReady() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return Loaded().state == LoadState::kReady;
}

bool MoltNetRenderersExpectFonts() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return Loaded().state != LoadState::kAbsent;
}

std::optional<MoltNetFontsStatus> GetMoltNetFontsStatus() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  switch (Loaded().state) {
    case LoadState::kReady:
      return MoltNetFontsStatus::kReady;
    case LoadState::kAbsent:
      return MoltNetFontsStatus::kAbsent;
    case LoadState::kBroken:
      return MoltNetFontsStatus::kBroken;
    case LoadState::kNotStarted:
    case LoadState::kLoading:
      return std::nullopt;
  }
  return std::nullopt;
}

std::vector<base::File> DuplicateMoltNetFonts() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const LoadedFonts& loaded = Loaded();
  std::vector<base::File> duplicates;
  if (loaded.state != LoadState::kReady) {
    return duplicates;
  }
  duplicates.reserve(loaded.files.size());
  for (const base::File& file : loaded.files) {
    base::File duplicate = file.Duplicate();
    if (!duplicate.IsValid()) {
      return {};
    }
    duplicates.push_back(std::move(duplicate));
  }
  return duplicates;
}

void WhenMoltNetFontsReady(MoltNetFontsCallback deliver) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LoadedFonts& loaded = Loaded();
  switch (loaded.state) {
    case LoadState::kReady:
      std::move(deliver).Run(DuplicateMoltNetFonts());
      return;
    case LoadState::kNotStarted:
    case LoadState::kLoading:
      loaded.waiting.push_back(std::move(deliver));
      StartLoadingMoltNetFonts();
      return;
    case LoadState::kAbsent:
    case LoadState::kBroken:
      return;
  }
}

void SetMoltNetFontsForTesting(std::optional<std::vector<base::File>> files) {
  LoadedFonts& loaded = Loaded();
  loaded.files.clear();
  loaded.waiting.clear();
  loaded.state = LoadState::kNotStarted;
  if (files) {
    loaded.files = std::move(*files);
    loaded.state = LoadState::kReady;
  }
}

void SetMoltNetFontsLoadingForTesting() {
  SetMoltNetFontsForTesting(std::nullopt);
  Loaded().state = LoadState::kLoading;
}

void FinishMoltNetFontsLoadForTesting(MoltNetFontsLoad load) {
  OnLoaded(std::move(load));
}

}  // namespace molt_ai::fingerprinting
