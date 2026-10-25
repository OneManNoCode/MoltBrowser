// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_fonts.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_net_fonts_table.h"
#include "content/public/browser/browser_thread.h"

namespace molt_ai::fingerprinting {

namespace {

enum class LoadState { kNotStarted, kLoading, kReady, kFailed };

struct LoadedFonts {
  LoadState state = LoadState::kNotStarted;
  std::vector<base::File> files;
};

LoadedFonts& Loaded() {
  static base::NoDestructor<LoadedFonts> loaded;
  return *loaded;
}

void OnLoaded(std::optional<std::vector<base::File>> files) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LoadedFonts& loaded = Loaded();
  if (files) {
    loaded.files = std::move(*files);
    loaded.state = LoadState::kReady;
  } else {
    loaded.state = LoadState::kFailed;
  }
}

}  // namespace

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
      LOG(WARNING) << "MoltNet fonts: " << path
                   << " is missing or not the pinned file; MoltNet windows "
                      "use the OS's font allow-list (set "
                   << kMoltNetFontSetVersion << ")";
      return std::nullopt;
    }
    files.push_back(std::move(file));
  }
  return files;
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
      base::BindOnce(&OpenMoltNetFonts, GetMoltNetFontsDirectory()),
      base::BindOnce(&OnLoaded));
}

bool AreMoltNetFontsReady() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return Loaded().state == LoadState::kReady;
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

void SetMoltNetFontsForTesting(std::optional<std::vector<base::File>> files) {
  LoadedFonts& loaded = Loaded();
  loaded.files.clear();
  loaded.state = LoadState::kNotStarted;
  if (files) {
    loaded.files = std::move(*files);
    loaded.state = LoadState::kReady;
  }
}

}  // namespace molt_ai::fingerprinting
