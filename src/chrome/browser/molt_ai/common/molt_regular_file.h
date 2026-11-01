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
//     On Windows links aren't told apart.
//
// A hung network file system can still block a read: stat() itself waits
// there. Blocking (file I/O), on a sequence that may block. base only, so
// molt_ai_unittests tests it on real files.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_REGULAR_FILE_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_REGULAR_FILE_H_

#include <stddef.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

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

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_REGULAR_FILE_H_
