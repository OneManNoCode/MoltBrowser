// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MeasureScratch() on Linux and macOS: a walk relative to open directory
// descriptors (openat, fstatat), so no path is resolved twice and no path
// length limits it, that never follows a symbolic link.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/common/helper_limits.h"

#if defined(__APPLE__)
#include <sys/attr.h>
#endif

namespace molt_ai {
namespace helper_limits {

namespace {

using Gap = ScratchUsage::Gap;

struct Walk {
  uint64_t max_entries = 0;
  ScratchUsage usage;
};

void AddBytes(uint64_t bytes, ScratchUsage* usage) {
  usage->bytes = usage->bytes > std::numeric_limits<uint64_t>::max() - bytes
                     ? std::numeric_limits<uint64_t>::max()
                     : usage->bytes + bytes;
}

// Its size or the space it takes (st_blocks is in 512-byte units on Linux
// and macOS), whichever is larger.
uint64_t BytesOf(const struct stat& info) {
  const uint64_t size =
      info.st_size > 0 ? static_cast<uint64_t>(info.st_size) : 0;
  const uint64_t blocks =
      info.st_blocks > 0 ? static_cast<uint64_t>(info.st_blocks) : 0;
  const uint64_t allocated =
      blocks > std::numeric_limits<uint64_t>::max() / 512
          ? std::numeric_limits<uint64_t>::max()
          : blocks * 512;
  return std::max(size, allocated);
}

#if defined(__APPLE__)
// getattrlist(2)'s reply for ATTR_FILE_TOTALSIZE | ATTR_FILE_ALLOCSIZE, in
// bit order: the length, then the logical and the allocated size of all of
// the file's forks.
struct __attribute__((aligned(4), packed)) ForkSizes {
  uint32_t length;
  off_t total_size;
  off_t allocated_size;
};

// The regular file |name| in |dir_fd| with all its forks (a resource fork
// isn't in st_size): the larger of their logical and allocated sizes. False
// if it can't be measured; then |*gone| if it is gone.
bool AllForksBytes(int dir_fd, const char* name, uint64_t* bytes, bool* gone) {
  struct attrlist request = {};
  request.bitmapcount = ATTR_BIT_MAP_COUNT;
  request.fileattr = ATTR_FILE_TOTALSIZE | ATTR_FILE_ALLOCSIZE;
  ForkSizes sizes = {};
  if (getattrlistat(dir_fd, name, &request, &sizes, sizeof(sizes),
                    FSOPT_NOFOLLOW) != 0) {
    *gone = errno == ENOENT;
    return false;
  }
  if (sizes.length < sizeof(sizes)) {
    return false;
  }
  const off_t largest = std::max(sizes.total_size, sizes.allocated_size);
  *bytes = largest > 0 ? static_cast<uint64_t>(largest) : 0;
  return true;
}
#endif  // defined(__APPLE__)

// Walks the directory open as |dir_fd| (which it takes), at |depth| below
// the scratch directory.
void WalkDirectory(int dir_fd, int depth, Walk* walk) {
  DIR* dir = fdopendir(dir_fd);
  if (!dir) {
    close(dir_fd);
    walk->usage.gap = Gap::kUnreadable;
    return;
  }
  const int fd = dirfd(dir);
  for (;;) {
    errno = 0;
    const struct dirent* entry = readdir(dir);
    if (!entry) {
      if (errno != 0) {
        walk->usage.gap = Gap::kUnreadable;
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    // Every name the directory listed counts, also one that is gone by
    // the time it is measured: a helper that keeps making and removing
    // files can't keep the walk going.
    if (walk->usage.entries >= walk->max_entries) {
      walk->usage.gap = Gap::kTooManyEntries;
      break;
    }
    ++walk->usage.entries;
    struct stat info = {};
    if (fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        continue;
      }
      walk->usage.gap = Gap::kUnreadable;
      break;
    }
    uint64_t bytes = BytesOf(info);
#if defined(__APPLE__)
    if (S_ISREG(info.st_mode)) {
      uint64_t forks = 0;
      bool gone = false;
      if (AllForksBytes(fd, entry->d_name, &forks, &gone)) {
        bytes = std::max(bytes, forks);
      } else if (!gone) {
        walk->usage.gap = Gap::kUnreadable;
        break;
      }
    }
#endif
    AddBytes(bytes, &walk->usage);
    if (!S_ISDIR(info.st_mode)) {
      continue;
    }
    if (depth + 1 > kMaxScratchDepth) {
      walk->usage.gap = Gap::kTooDeep;
      break;
    }
    // O_NOFOLLOW: if it was replaced by a symbolic link since fstatat(),
    // this fails rather than follow it.
    const int child = openat(fd, entry->d_name,
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) {
      if (errno == ENOENT) {
        continue;
      }
      walk->usage.gap = Gap::kUnreadable;
      break;
    }
    WalkDirectory(child, depth + 1, walk);
    if (walk->usage.gap != Gap::kNone) {
      break;
    }
  }
  closedir(dir);
}

}  // namespace

ScratchUsage MeasureScratch(const PathString& dir, uint64_t max_entries) {
  Walk walk;
  walk.max_entries = max_entries;
  const int fd =
      open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    walk.usage.gap = Gap::kUnreadable;
    return walk.usage;
  }
  WalkDirectory(fd, 0, &walk);
  return walk.usage;
}

}  // namespace helper_limits
}  // namespace molt_ai
