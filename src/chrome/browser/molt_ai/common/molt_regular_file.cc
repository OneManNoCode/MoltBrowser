// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_regular_file.h"

#include <stdint.h>

#include <utility>

#include "base/base_paths.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/threading/platform_thread.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#else
#include <windows.h>

#include <string_view>

#include "base/containers/span.h"
#include "base/win/scoped_handle.h"
#endif

namespace molt_ai {

namespace {

// Read this much at a time.
constexpr size_t kChunkBytes = 16 * 1024;

// MoltFileLock tries again this often.
constexpr base::TimeDelta kLockRetry = base::Milliseconds(20);

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

// |path| opened to be locked: created when missing, never through a link,
// never waiting on a FIFO, and only a regular file.
base::File OpenLockFile(const base::FilePath& path) {
  base::ScopedFD fd(HANDLE_EINTR(
      open(path.value().c_str(),
           O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY,
           0600)));
  struct stat st;
  if (!fd.is_valid() || fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return base::File();
  }
  return base::File(fd.release());
}

#else  // BUILDFLAG(IS_POSIX)

constexpr DWORD kShareAll =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

MoltFileKind KindOfOpenError(DWORD error) {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
             ? MoltFileKind::kMissing
             : MoltFileKind::kUnreadable;
}

// What an open handle is: a link (a name-surrogate reparse point: a
// symbolic link, a junction, a mount point), a directory, a pipe or a
// device ("not a regular file"), or a file (a reparse point of another
// kind, such as a cloud file's placeholder, holds its own data).
struct HandleKind {
  MoltFileKind kind = MoltFileKind::kUnreadable;
  bool link = false;
};

HandleKind KindOfHandle(HANDLE handle) {
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag,
                                      sizeof(tag))) {
    return {MoltFileKind::kUnreadable, false};
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      IsReparseTagNameSurrogate(tag.ReparseTag)) {
    return {MoltFileKind::kNotRegular, true};
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      ::GetFileType(handle) != FILE_TYPE_DISK) {
    return {MoltFileKind::kNotRegular, false};
  }
  return {MoltFileKind::kRegular, false};
}

// |path| opened without following a link at it (FILE_FLAG_OPEN_REPARSE_POINT;
// FILE_FLAG_BACKUP_SEMANTICS opens a directory too, to be told apart).
base::win::ScopedHandle OpenNotFollowed(const base::FilePath& path,
                                        DWORD access,
                                        DWORD disposition) {
  return base::win::ScopedHandle(::CreateFileW(
      path.value().c_str(), access, kShareAll, nullptr, disposition,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

// What is at |path| itself, a link not followed.
HandleKind LookAt(const base::FilePath& path) {
  base::win::ScopedHandle handle =
      OpenNotFollowed(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING);
  if (!handle.is_valid()) {
    return {KindOfOpenError(::GetLastError()), false};
  }
  return KindOfHandle(handle.get());
}

// |path| with every link on the way followed, as the system names it
// (GetFinalPathNameByHandleW(), without its "\\?\" prefix); empty when it
// leads nowhere.
base::FilePath FinalPathOf(const base::FilePath& path) {
  base::win::ScopedHandle handle(::CreateFileW(
      path.value().c_str(), /*dwDesiredAccess=*/0, kShareAll, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle.is_valid()) {
    return base::FilePath();
  }
  std::wstring name(MAX_PATH, L'\0');
  constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
  DWORD length = ::GetFinalPathNameByHandleW(
      handle.get(), name.data(), static_cast<DWORD>(name.size()), kFlags);
  if (length >= name.size()) {
    // Too long for the buffer: |length| is the size it needs.
    name.assign(length, L'\0');
    length = ::GetFinalPathNameByHandleW(
        handle.get(), name.data(), static_cast<DWORD>(name.size()), kFlags);
  }
  if (length == 0 || length >= name.size()) {
    return base::FilePath();
  }
  name.resize(length);
  constexpr std::wstring_view kUnc = L"\\\\?\\UNC\\";
  constexpr std::wstring_view kLocal = L"\\\\?\\";
  if (std::wstring_view(name).substr(0, kUnc.size()) == kUnc) {
    name = L"\\\\" + name.substr(kUnc.size());
  } else if (std::wstring_view(name).substr(0, kLocal.size()) == kLocal) {
    name.erase(0, kLocal.size());
  }
  return base::FilePath(name);
}

// A link at |path|, followed to a regular file inside the home directory.
Resolved FollowLinkWithinHome(const base::FilePath& path) {
  Resolved refused{MoltFileKind::kNotRegular, path};
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return refused;
  }
  // Every link resolved, the home directory's own included.
  const base::FilePath real_home = FinalPathOf(home);
  const base::FilePath target = FinalPathOf(path);
  if (real_home.empty() || target.empty() || !real_home.IsParent(target)) {
    return refused;
  }
  if (LookAt(target).kind != MoltFileKind::kRegular) {
    return refused;
  }
  return {MoltFileKind::kRegular, target};
}

Resolved Resolve(const base::FilePath& path, MoltFileLinks links) {
  const HandleKind at = LookAt(path);
  if (at.link && links == MoltFileLinks::kFollowedWithinHome) {
    return FollowLinkWithinHome(path);
  }
  return {at.kind, path};
}

// |file| opened for reading if it is a regular file: never through a link
// swapped in since it was looked at, never a pipe or a device.
base::File OpenForRead(const base::FilePath& file,
                       MoltFileKind* kind,
                       int64_t* size) {
  base::win::ScopedHandle handle =
      OpenNotFollowed(file, GENERIC_READ, OPEN_EXISTING);
  if (!handle.is_valid()) {
    *kind = KindOfOpenError(::GetLastError());
    return base::File();
  }
  const HandleKind opened = KindOfHandle(handle.get());
  if (opened.kind != MoltFileKind::kRegular) {
    *kind = opened.kind;
    return base::File();
  }
  base::File read(handle.Take());
  base::File::Info info;
  if (!read.GetInfo(&info)) {
    *kind = MoltFileKind::kUnreadable;
    return base::File();
  }
  *kind = MoltFileKind::kRegular;
  *size = info.size;
  return read;
}

// |path| opened to be appended to (|append|) or locked: created when
// missing, never through a link, and only a regular file.
base::File OpenToChange(const base::FilePath& path, bool append) {
  base::win::ScopedHandle handle = OpenNotFollowed(
      path, append ? FILE_APPEND_DATA : GENERIC_READ | GENERIC_WRITE,
      OPEN_ALWAYS);
  if (!handle.is_valid() ||
      KindOfHandle(handle.get()).kind != MoltFileKind::kRegular) {
    return base::File();
  }
  return base::File(handle.Take());
}

base::File OpenLockFile(const base::FilePath& path) {
  return OpenToChange(path, /*append=*/false);
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
  // FILE_APPEND_DATA only: every write goes at the end.
  base::File file = OpenToChange(path, /*append=*/true);
  if (!file.IsValid()) {
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

size_t DeleteStaleWriteTemporaries(
    const base::FilePath& dir,
    base::FunctionRef<bool(std::string_view name)> is_ours,
    base::Time now) {
  size_t deleted = 0;
  // The writer's temporary files (base::CreateAndOpenTemporaryFileInDir()
  // with the file's name as the prefix), whatever file they were for.
  base::FileEnumerator temporaries(
      dir, /*recursive=*/false, base::FileEnumerator::FILES,
      base::FormatTemporaryFileName(FILE_PATH_LITERAL("*"), /*hidden=*/true)
          .value());
  for (base::FilePath path = temporaries.Next(); !path.empty();
       path = temporaries.Next()) {
    const std::optional<base::FilePath::StringType> prefix =
        base::GetNamePrefixForTemporaryFile(path);
    if (!prefix || !is_ours(base::FilePath(*prefix).AsUTF8Unsafe()) ||
        MoltFileKindAt(path, MoltFileLinks::kNotFollowed) !=
            MoltFileKind::kRegular) {
      continue;
    }
    base::File::Info info;
    if (!base::GetFileInfo(path, &info)) {
      continue;
    }
    // A write in progress (here, or in another browser on the same home
    // directory) is left to finish.
    const base::TimeDelta age = now - info.last_modified;
    if (!age.is_negative() && age < kStaleWriteAge) {
      continue;
    }
    if (base::DeleteFile(path)) {
      ++deleted;
    }
  }
  return deleted;
}

MoltFileLock::MoltFileLock(const base::FilePath& path, base::TimeDelta wait) {
  file_ = OpenLockFile(path);
  if (!file_.IsValid()) {
    return;
  }
  for (base::TimeDelta waited;; waited += kLockRetry) {
    const base::File::Error error =
        file_.Lock(base::File::LockMode::kExclusive);
    if (error == base::File::FILE_OK) {
      held_ = true;
      return;
    }
    // Held by another browser (POSIX EAGAIN or EACCES, Windows
    // ERROR_LOCK_VIOLATION): tried again a little later. Anything else (a
    // file system without locks) won't change.
    const bool busy = error == base::File::FILE_ERROR_ACCESS_DENIED ||
                      error == base::File::FILE_ERROR_IN_USE;
    if (!busy || waited >= wait) {
      file_.Close();
      return;
    }
    base::PlatformThread::Sleep(kLockRetry);
  }
}

MoltFileLock::~MoltFileLock() {
  if (held_) {
    file_.Unlock();
  }
}

}  // namespace molt_ai
