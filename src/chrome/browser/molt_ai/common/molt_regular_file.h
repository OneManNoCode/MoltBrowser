// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser's own files on disk (settings.json, a profile's voice
// learning file, workflows and voice routines, their audit logs) are read
// and written only when they are regular files:
//
//   - Nothing else at their path is ever opened: not a FIFO (whose open()
//     would wait for a writer, and with it a sequence that must finish
//     before shutdown), a socket, a device or a directory. Such a path is
//     "not a regular file": not read, not written over, left as it is.
//     (POSIX: lstat() first, then open() with O_NONBLOCK and O_NOFOLLOW,
//     and fstat() on what was opened, so a file swapped in between is
//     refused too. Windows: a directory is refused; a FIFO can't be at an
//     ordinary path there.)
//   - Read with a cap: a bigger file isn't read (kTooBig).
//   - Links (POSIX), by file:
//       * kFollowedWithinHome, for the files rewritten whole
//         (settings.json, the learning file): a link (or chain of links)
//         to a regular file inside the user's home directory is followed,
//         read and written through, so the link stays (dotfile managers
//         keep settings.json as a link). A link anywhere else, to anything
//         but a regular file or to nothing is "not a regular file".
//       * kNotFollowed, for the files that are renamed, moved and deleted
//         by name (workflows and routines, the audit log): a link is "not
//         a regular file", because renaming or deleting one acts on the
//         link, not on what it points to (a deleted routine's steps would
//         stay in the link's target).
//     Windows: a link is a reparse point that names another file (a
//     symbolic link, a junction or a mount point:
//     IsReparseTagNameSurrogate()), looked at with
//     FILE_FLAG_OPEN_REPARSE_POINT so it isn't followed, and followed (for
//     kFollowedWithinHome) to its final path (GetFinalPathNameByHandleW()).
//     Any other reparse point (a cloud file's placeholder, a deduplicated
//     file) holds the file's own data: a file, not a link. A pipe or a
//     device a link leads to is "not a regular file" (GetFileType()).
//
// A browser that ends in the middle of a whole-file rewrite leaves the
// writer's temporary file beside it (base::ImportantFileWriter):
// DeleteStaleWriteTemporaries() deletes those (below).
//
// Advisory locks: MoltFileLock, which another browser on the same home
// directory honours (below).
//
// A hung network file system can still block a read: stat() itself waits
// there (molt_file_sequences.h keeps such a read from holding shutdown).
// Blocking (file I/O), on a sequence that may block. base only, so
// molt_ai_unittests tests it on real files.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_REGULAR_FILE_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_REGULAR_FILE_H_

#include <stddef.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/function_ref.h"
#include "base/time/time.h"

namespace molt_ai {

// What is at a path.
enum class MoltFileKind {
  kMissing,     // nothing (or no directory) there
  kRegular,     // a regular file (or a link followed to one)
  kNotRegular,  // anything else, or a link not followed: never opened
  kTooBig,      // a regular file over the cap: not read
  kUnreadable,  // couldn't be looked at, opened or read
};

// Whether a link at the path itself is followed (see above).
enum class MoltFileLinks {
  kNotFollowed,
  kFollowedWithinHome,
};

struct MoltFileRead {
  MoltFileKind kind = MoltFileKind::kMissing;
  // The file's contents (kRegular only).
  std::string contents;
};

// What is at |path|, without opening it.
MoltFileKind MoltFileKindAt(const base::FilePath& path, MoltFileLinks links);

// |path|'s contents when it is a regular file of at most |max_bytes|.
MoltFileRead ReadMoltFile(const base::FilePath& path,
                          size_t max_bytes,
                          MoltFileLinks links);

// Where a whole-file rewrite of |path| goes (a temporary file beside it,
// then a rename): |path| when nothing or a regular file is there, a link's
// target when |links| follows it; nullopt for anything else, which is left
// alone.
std::optional<base::FilePath> MoltFileWritePath(const base::FilePath& path,
                                                MoltFileLinks links);

// Appends |data| to |path|, which is created (readable by its owner only)
// when missing; false, and nothing written, when anything but a regular
// file is there (never through a link).
bool AppendToMoltFile(const base::FilePath& path, std::string_view data);

// The end of |path| (a regular file, never through a link): all of it up
// to |max_bytes|, else its last |max_bytes| from the first whole line in
// them. "" when it is missing; nullopt when it isn't a regular file or
// can't be read.
std::optional<std::string> ReadMoltFileTail(const base::FilePath& path,
                                            size_t max_bytes);

// A temporary file the writer left this long ago is one a browser that
// ended mid-write left: no write takes this long.
inline constexpr base::TimeDelta kStaleWriteAge = base::Minutes(1);

// The temporary files base::ImportantFileWriter left in |dir| when the
// browser ended in the middle of a write: each regular file there (never a
// link) named as the writer names a temporary file beside a file whose
// name |is_ours| accepts (Chromium 156: POSIX ".<product>.<name>.XXXXXX",
// Windows "<name><GUID>.tmp"; base::GetNamePrefixForTemporaryFile()), last
// changed at least kStaleWriteAge before |now| (or after it: a clock set
// back), is deleted. The number deleted.
size_t DeleteStaleWriteTemporaries(
    const base::FilePath& dir,
    base::FunctionRef<bool(std::string_view name)> is_ours,
    base::Time now);

// An advisory lock on |path|, which another browser on the same home
// directory takes for the same files before it changes them: the file
// (created, readable by its owner only, when missing; never through a link,
// never anything but a regular file) locked with base::File::Lock() while
// the object lives. The system releases it when the browser ends, crashed
// or not. Never waits on the lock itself: tried again every few
// milliseconds for at most |wait|, then the caller goes on without it
// (held() false), as it does when anything else is at |path| or the file
// system has no locks; its changes are atomic renames either way.
class MoltFileLock {
 public:
  MoltFileLock(const base::FilePath& path, base::TimeDelta wait);
  MoltFileLock(const MoltFileLock&) = delete;
  MoltFileLock& operator=(const MoltFileLock&) = delete;
  ~MoltFileLock();

  bool held() const { return held_; }

 private:
  base::File file_;
  bool held_ = false;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_REGULAR_FILE_H_
