// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MeasureScratch() on Windows. Each entry is opened for its attributes
// only (which no sharing mode refuses, and which opens a reparse point
// itself rather than its target) and measured through that handle: the
// sizes a directory listing gives can be out of date while a file is being
// written. Every stream counts, a directory's too.

#include <windows.h>

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/numerics/byte_conversions.h"
#include "base/win/scoped_handle.h"
#include "chrome/browser/molt_ai/common/helper_limits.h"

namespace molt_ai {
namespace helper_limits {

namespace {

using Gap = ScratchUsage::Gap;

// The most a FILE_STREAM_INFO list may take: a few thousand streams. A
// file with more can't be measured.
constexpr size_t kMaxStreamInfoBytes = 1u << 20;

// The unnamed data stream's name in a FILE_STREAM_INFO list.
constexpr std::wstring_view kDataStream = L"::$DATA";

struct Walk {
  uint64_t max_entries = 0;
  ScratchUsage usage;
};

void AddBytes(uint64_t bytes, ScratchUsage* usage) {
  usage->bytes = usage->bytes > std::numeric_limits<uint64_t>::max() - bytes
                     ? std::numeric_limits<uint64_t>::max()
                     : usage->bytes + bytes;
}

uint64_t Larger(int64_t a, int64_t b) {
  const int64_t larger = std::max(a, b);
  return larger > 0 ? static_cast<uint64_t>(larger) : 0;
}

uint64_t Larger(LARGE_INTEGER a, LARGE_INTEGER b) {
  return Larger(static_cast<int64_t>(a.QuadPart),
                static_cast<int64_t>(b.QuadPart));
}

// "\\?\" before a drive-letter path, so that an entry's path may be longer
// than MAX_PATH.
std::wstring ExtendedPath(const std::wstring& path) {
  if (path.starts_with(L"\\\\?\\")) {
    return path;
  }
  if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') {
    return L"\\\\?\\" + path;
  }
  return path;
}

enum class Opened { kOpen, kGone, kFailed };

Opened OpenForAttributes(const std::wstring& path,
                         base::win::ScopedHandle* handle) {
  handle->Set(::CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (handle->is_valid()) {
    return Opened::kOpen;
  }
  const DWORD error = ::GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
             ? Opened::kGone
             : Opened::kFailed;
}

// What the file or directory open as |handle| takes: its data (a file's)
// and every named stream, each the larger of its size and its allocation.
// False if it can't be measured.
bool BytesOf(HANDLE handle, bool is_directory, uint64_t* bytes) {
  uint64_t data = 0;
  if (!is_directory) {
    FILE_STANDARD_INFO standard = {};
    if (!::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard,
                                        sizeof(standard))) {
      return false;
    }
    data = Larger(standard.EndOfFile, standard.AllocationSize);
  }
  std::vector<uint8_t> list(16 * 1024);
  while (!::GetFileInformationByHandleEx(handle, FileStreamInfo, list.data(),
                                         static_cast<DWORD>(list.size()))) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_HANDLE_EOF) {
      // No stream at all (a directory without named streams).
      *bytes = data;
      return true;
    }
    if ((error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) ||
        list.size() >= kMaxStreamInfoBytes) {
      return false;
    }
    list.resize(list.size() * 4);
  }
  // FILE_STREAM_INFO: NextEntryOffset and StreamNameLength (DWORDs),
  // StreamSize and StreamAllocationSize (LARGE_INTEGERs), then the name.
  static_assert(offsetof(FILE_STREAM_INFO, NextEntryOffset) == 0);
  static_assert(offsetof(FILE_STREAM_INFO, StreamNameLength) == 4);
  static_assert(offsetof(FILE_STREAM_INFO, StreamSize) == 8);
  static_assert(offsetof(FILE_STREAM_INFO, StreamAllocationSize) == 16);
  constexpr size_t kFixedPart = offsetof(FILE_STREAM_INFO, StreamName);
  static_assert(kFixedPart == 24);
  const base::span<const uint8_t> data_stream_name =
      base::as_byte_span(kDataStream);
  uint64_t named = 0;
  base::span<const uint8_t> rest(list);
  for (;;) {
    if (rest.size() < kFixedPart) {
      return false;
    }
    const uint32_t next = base::U32FromNativeEndian(rest.subspan<0, 4>());
    const uint32_t name_length =
        base::U32FromNativeEndian(rest.subspan<4, 4>());
    const int64_t size = base::I64FromNativeEndian(rest.subspan<8, 8>());
    const int64_t allocated =
        base::I64FromNativeEndian(rest.subspan<16, 8>());
    if (name_length > rest.size() - kFixedPart) {
      return false;
    }
    const base::span<const uint8_t> name =
        rest.subspan(kFixedPart, name_length);
    const uint64_t stream = Larger(size, allocated);
    if (std::ranges::equal(name, data_stream_name)) {
      data = std::max(data, stream);
    } else {
      named = named > std::numeric_limits<uint64_t>::max() - stream
                  ? std::numeric_limits<uint64_t>::max()
                  : named + stream;
    }
    if (next == 0) {
      break;
    }
    if (next > rest.size()) {
      return false;
    }
    rest = rest.subspan(next);
  }
  *bytes = data > std::numeric_limits<uint64_t>::max() - named
               ? std::numeric_limits<uint64_t>::max()
               : data + named;
  return true;
}

// Walks the directory |dir| (an extended path), at |depth| below the
// scratch directory.
void WalkDirectory(const std::wstring& dir, int depth, Walk* walk) {
  WIN32_FIND_DATAW found = {};
  HANDLE find = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic,
                                   &found, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) {
    if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
      walk->usage.gap = Gap::kUnreadable;
    }
    return;
  }
  do {
    const std::wstring name(found.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    // Every name the directory listed counts, also one that is gone by the
    // time it is measured.
    if (walk->usage.entries >= walk->max_entries) {
      walk->usage.gap = Gap::kTooManyEntries;
      break;
    }
    ++walk->usage.entries;
    const std::wstring path = dir + L"\\" + name;
    base::win::ScopedHandle handle;
    const Opened opened = OpenForAttributes(path, &handle);
    if (opened == Opened::kGone) {
      continue;
    }
    FILE_BASIC_INFO basic = {};
    if (opened == Opened::kFailed ||
        !::GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic,
                                        sizeof(basic))) {
      walk->usage.gap = Gap::kUnreadable;
      break;
    }
    const bool is_directory =
        (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    uint64_t bytes = 0;
    if (!BytesOf(handle.get(), is_directory, &bytes)) {
      walk->usage.gap = Gap::kUnreadable;
      break;
    }
    AddBytes(bytes, &walk->usage);
    // A reparse point (a junction or link) is an entry, never followed.
    if (!is_directory ||
        (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      continue;
    }
    if (depth + 1 > kMaxScratchDepth) {
      walk->usage.gap = Gap::kTooDeep;
      break;
    }
    handle.Close();
    WalkDirectory(path, depth + 1, walk);
    if (walk->usage.gap != Gap::kNone) {
      break;
    }
  } while (::FindNextFileW(find, &found));
  if (walk->usage.gap == Gap::kNone && ::GetLastError() != ERROR_NO_MORE_FILES) {
    walk->usage.gap = Gap::kUnreadable;
  }
  ::FindClose(find);
}

}  // namespace

ScratchUsage MeasureScratch(const PathString& dir, uint64_t max_entries) {
  Walk walk;
  walk.max_entries = max_entries;
  const std::wstring root = ExtendedPath(dir);
  // The directory's own named streams count too.
  base::win::ScopedHandle handle;
  uint64_t bytes = 0;
  if (OpenForAttributes(root, &handle) != Opened::kOpen ||
      !BytesOf(handle.get(), /*is_directory=*/true, &bytes)) {
    walk.usage.gap = Gap::kUnreadable;
    return walk.usage;
  }
  handle.Close();
  AddBytes(bytes, &walk.usage);
  WalkDirectory(root, 0, &walk);
  return walk.usage;
}

}  // namespace helper_limits
}  // namespace molt_ai
