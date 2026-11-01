// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_regular_file.h"

#include <stdint.h>

#include <utility>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#else
#include "base/containers/span.h"
#include "base/files/file.h"
#endif

namespace molt_ai {

namespace {

// Read this much at a time.
constexpr size_t kChunkBytes = 16 * 1024;

// What a read or a rewrite of a path uses: the path itself, or a link's
// target.
struct Resolved {
  MoltFileKind kind = MoltFileKind::kMissing;
  base::FilePath file;
};

#if BUILDFLAG(IS_POSIX)

MoltFileKind KindOfError(int error) {
  return error == ENOENT || error == ENOTDIR ? MoltFileKind::kMissing
                                             : MoltFileKind::kUnreadable;
}

// A link at |path|, followed to a regular file inside the home directory.
Resolved FollowLinkWithinHome(const base::FilePath& path) {
  Resolved refused{MoltFileKind::kNotRegular, path};
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return refused;
  }
  // Every link resolved, the home directory's own included (it may be a
  // link itself, e.g. /home -> /usr/home).
  const base::FilePath real_home = base::MakeAbsoluteFilePath(home);
  const base::FilePath target = base::MakeAbsoluteFilePath(path);
  if (real_home.empty() || target.empty() || !real_home.IsParent(target)) {
    return refused;
  }
  struct stat st;
  if (lstat(target.value().c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return refused;
  }
  return {MoltFileKind::kRegular, target};
}

Resolved Resolve(const base::FilePath& path, MoltFileLinks links) {
  struct stat st;
  if (lstat(path.value().c_str(), &st) != 0) {
    return {KindOfError(errno), path};
  }
  if (S_ISREG(st.st_mode)) {
    return {MoltFileKind::kRegular, path};
  }
  if (S_ISLNK(st.st_mode) && links == MoltFileLinks::kFollowedWithinHome) {
    return FollowLinkWithinHome(path);
  }
  return {MoltFileKind::kNotRegular, path};
}

// |file| opened for reading only if it is a regular file: never waits on a
// FIFO, never follows a link swapped in since it was looked at.
base::ScopedFD OpenRegularForRead(const base::FilePath& file,
                                  MoltFileKind* kind,
                                  int64_t* size) {
  base::ScopedFD fd(HANDLE_EINTR(
      open(file.value().c_str(),
           O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY)));
  if (!fd.is_valid()) {
    *kind = errno == ELOOP ? MoltFileKind::kNotRegular : KindOfError(errno);
    return base::ScopedFD();
  }
  struct stat st;
  if (fstat(fd.get(), &st) != 0) {
    *kind = MoltFileKind::kUnreadable;
    return base::ScopedFD();
  }
  if (!S_ISREG(st.st_mode)) {
    *kind = MoltFileKind::kNotRegular;
    return base::ScopedFD();
  }
  *kind = MoltFileKind::kRegular;
  *size = static_cast<int64_t>(st.st_size);
  return fd;
}

// Reads from |fd| at |offset| until its end, at most |max_bytes| (false,
// and |out| incomplete, past them or on an error).
bool ReadFrom(int fd, int64_t offset, size_t max_bytes, std::string* out) {
  out->clear();
  std::string chunk(kChunkBytes, '\0');
  for (;;) {
    const ssize_t n = HANDLE_EINTR(
        pread(fd, chunk.data(), chunk.size(), static_cast<off_t>(offset)));
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      return true;
    }
    if (static_cast<size_t>(n) > max_bytes - out->size()) {
      return false;
    }
    out->append(chunk, 0, static_cast<size_t>(n));
    offset += n;
  }
}

#else  // BUILDFLAG(IS_POSIX)

Resolved Resolve(const base::FilePath& path, MoltFileLinks links) {
  base::File::Info info;
  if (!base::GetFileInfo(path, &info)) {
    return {base::PathExists(path) ? MoltFileKind::kUnreadable
                                   : MoltFileKind::kMissing,
            path};
  }
  if (info.is_directory) {
    return {MoltFileKind::kNotRegular, path};
  }
  return {MoltFileKind::kRegular, path};
}

// |file| opened for reading (Windows: no FIFO at an ordinary path).
base::File OpenForRead(const base::FilePath& file,
                       MoltFileKind* kind,
                       int64_t* size) {
  base::File opened(file, base::File::FLAG_OPEN | base::File::FLAG_READ |
                              base::File::FLAG_WIN_SHARE_DELETE);
  base::File::Info info;
  if (!opened.IsValid() || !opened.GetInfo(&info)) {
    *kind = base::PathExists(file) ? MoltFileKind::kUnreadable
                                   : MoltFileKind::kMissing;
    return base::File();
  }
  if (info.is_directory) {
    *kind = MoltFileKind::kNotRegular;
    return base::File();
  }
  *kind = MoltFileKind::kRegular;
  *size = info.size;
  return opened;
}

bool ReadFrom(base::File& file,
              int64_t offset,
              size_t max_bytes,
              std::string* out) {
  out->clear();
  std::string chunk(kChunkBytes, '\0');
  for (;;) {
    const std::optional<size_t> n =
        file.Read(offset, base::as_writable_byte_span(chunk));
    if (!n) {
      return false;
    }
    if (*n == 0) {
      return true;
    }
    if (*n > max_bytes - out->size()) {
      return false;
    }
    out->append(chunk, 0, *n);
    offset += static_cast<int64_t>(*n);
  }
}

#endif  // BUILDFLAG(IS_POSIX)

}  // namespace

MoltFileKind MoltFileKindAt(const base::FilePath& path, MoltFileLinks links) {
  return Resolve(path, links).kind;
}

MoltFileRead ReadMoltFile(const base::FilePath& path,
                          size_t max_bytes,
                          MoltFileLinks links) {
  MoltFileRead read;
  const Resolved resolved = Resolve(path, links);
  if (resolved.kind != MoltFileKind::kRegular) {
    read.kind = resolved.kind;
    return read;
  }
  int64_t size = 0;
#if BUILDFLAG(IS_POSIX)
  base::ScopedFD file = OpenRegularForRead(resolved.file, &read.kind, &size);
  if (!file.is_valid()) {
    return read;
  }
#else
  base::File file = OpenForRead(resolved.file, &read.kind, &size);
  if (!file.IsValid()) {
    return read;
  }
#endif
  if (size < 0 || static_cast<uint64_t>(size) > max_bytes) {
    read.kind = MoltFileKind::kTooBig;
    return read;
  }
  read.contents.reserve(static_cast<size_t>(size));
#if BUILDFLAG(IS_POSIX)
  const bool complete = ReadFrom(file.get(), 0, max_bytes, &read.contents);
#else
  const bool complete = ReadFrom(file, 0, max_bytes, &read.contents);
#endif
  if (!complete) {
    // Grown past the cap while read, or an I/O error.
    read.kind = read.contents.size() >= max_bytes ? MoltFileKind::kTooBig
                                                  : MoltFileKind::kUnreadable;
    read.contents.clear();
  }
  return read;
}

std::optional<base::FilePath> MoltFileWritePath(const base::FilePath& path,
                                                MoltFileLinks links) {
  const Resolved resolved = Resolve(path, links);
  switch (resolved.kind) {
    case MoltFileKind::kMissing:
      return path;
    case MoltFileKind::kRegular:
      return resolved.file;
    case MoltFileKind::kNotRegular:
    case MoltFileKind::kTooBig:
    case MoltFileKind::kUnreadable:
      return std::nullopt;
  }
  return std::nullopt;
}

bool AppendToMoltFile(const base::FilePath& path, std::string_view data) {
  const MoltFileKind kind = MoltFileKindAt(path, MoltFileLinks::kNotFollowed);
  if (kind != MoltFileKind::kMissing && kind != MoltFileKind::kRegular) {
    return false;
  }
#if BUILDFLAG(IS_POSIX)
  base::ScopedFD fd(
      HANDLE_EINTR(open(path.value().c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_NONBLOCK |
                            O_NOFOLLOW | O_CLOEXEC | O_NOCTTY,
                        0600)));
  struct stat st;
  if (!fd.is_valid() || fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  return base::WriteFileDescriptor(fd.get(), data);
#else
  base::File file(path, base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
  base::File::Info info;
  if (!file.IsValid() || !file.GetInfo(&info) || info.is_directory) {
    return false;
  }
  return file.WriteAtCurrentPosAndCheck(base::as_byte_span(data));
#endif
}

std::optional<std::string> ReadMoltFileTail(const base::FilePath& path,
                                            size_t max_bytes) {
  const Resolved resolved = Resolve(path, MoltFileLinks::kNotFollowed);
  if (resolved.kind == MoltFileKind::kMissing) {
    return std::string();
  }
  if (resolved.kind != MoltFileKind::kRegular) {
    return std::nullopt;
  }
  MoltFileKind kind = MoltFileKind::kMissing;
  int64_t size = 0;
#if BUILDFLAG(IS_POSIX)
  base::ScopedFD file = OpenRegularForRead(resolved.file, &kind, &size);
  if (!file.is_valid()) {
    return kind == MoltFileKind::kMissing ? std::optional<std::string>("")
                                          : std::nullopt;
  }
#else
  base::File file = OpenForRead(resolved.file, &kind, &size);
  if (!file.IsValid()) {
    return kind == MoltFileKind::kMissing ? std::optional<std::string>("")
                                          : std::nullopt;
  }
#endif
  const bool cut = size >= 0 && static_cast<uint64_t>(size) > max_bytes;
  // Cut: from the byte before the last |max_bytes|, so that a line starting
  // just at them is kept whole.
  const int64_t from = cut ? size - static_cast<int64_t>(max_bytes) - 1 : 0;
  std::string tail;
  // What is appended while it is read is left for the next read.
#if BUILDFLAG(IS_POSIX)
  ReadFrom(file.get(), from, max_bytes + 1, &tail);
#else
  ReadFrom(file, from, max_bytes + 1, &tail);
#endif
  if (cut) {
    const size_t newline = tail.find('\n');
    tail.erase(0, newline == std::string::npos ? tail.size() : newline + 1);
  }
  return tail;
}

}  // namespace molt_ai
