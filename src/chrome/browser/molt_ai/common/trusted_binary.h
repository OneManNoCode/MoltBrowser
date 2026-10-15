// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which helper executables MoltBrowser may run (tor, tesseract, whisper-cli).
// Audit S18: these used to be found by trying Homebrew, /usr/local, /opt/local
// and PATH. Those locations are writable without root (/opt/homebrew is owned
// by the user on macOS), so a planted binary would run with the user's
// privileges and be fed browsing data (Tor traffic, PDFs, microphone audio).
//
// Accepted, in this order:
//   1. Bundled: the copy shipped inside MoltBrowser (MoltBrowser.app/Contents/
//      Resources/<x>/ on macOS, <install dir>/<x>/ elsewhere). It has the
//      same trust as the browser executable, so it only has to stay inside
//      the bundle after symlinks are resolved, be a regular executable file
//      and not be writable by everyone.
//   2. System (Linux only, chosen by the caller): fixed paths such as
//      /usr/bin/tor. The resolved file and every directory from "/" down to
//      it must be owned by root and not writable by group or others.
//   3. Explicit (tor only, --moltnet-tor-binary): an absolute path the user
//      passed on the command line, checked like (2) except that the user's
//      own uid is also accepted as owner.
// Never: PATH, Homebrew, /usr/local, /opt/local, the home directory.
//
// Every accepted path is returned canonical (symlinks resolved); callers
// execute that path, not the one they asked about. The checks also reject
// setuid/setgid files.
//
// POSIX only. The policy runs against a FileSystemView so it is unit-tested
// without root or real files (trusted_binary_unittest.cc). Windows has no
// system fallback; its callers only accept the bundled copy.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_TRUSTED_BINARY_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_TRUSTED_BINARY_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

namespace molt_ai {
namespace trusted_binary {

// What the checks need to know about one path, without following a final
// symlink (lstat() semantics).
struct PathInfo {
  enum class Type { kRegular, kDirectory, kSymlink, kOther };
  Type type = Type::kOther;
  uint32_t owner_uid = 0;
  // st_mode & 07777: permission bits plus setuid/setgid/sticky.
  uint32_t mode = 0;
};

class FileSystemView {
 public:
  virtual ~FileSystemView() = default;
  // lstat(). nullopt if |path| does not exist or cannot be examined.
  virtual std::optional<PathInfo> Lstat(const std::string& path) const = 0;
  // realpath(): absolute, without symlinks, "." or "..". nullopt if |path|
  // does not exist.
  virtual std::optional<std::string> RealPath(
      const std::string& path) const = 0;
  // access(|path|, X_OK) for this process.
  virtual bool IsExecutable(const std::string& path) const = 0;
};

// The real filesystem (lstat/realpath/access). Blocking.
const FileSystemView& RealFileSystem();

// geteuid() of this process.
uint32_t CurrentUid();

enum class Source { kNone, kBundled, kSystem, kExplicit };

struct Result {
  Result();
  Result(const Result&);
  Result& operator=(const Result&);
  ~Result();

  bool ok() const { return source != Source::kNone; }

  Source source = Source::kNone;
  // Canonical path to execute. Empty unless ok().
  std::string path;
  // Unless ok(): why the most relevant candidate was refused, for logs and
  // UI. A candidate that exists but failed a check wins over "not found".
  std::string error;
  // Unless ok(): true iff some candidate existed but was refused.
  bool rejected_existing = false;
};

// |path| (inside |bundle_dir|) as a bundled binary.
Result CheckBundled(const std::string& bundle_dir,
                    const std::string& path,
                    const FileSystemView& fs);

// |path| as a root-owned system binary.
Result CheckSystem(const std::string& path, const FileSystemView& fs);

// |path| as an explicit command-line override: like CheckSystem, but files
// and directories owned by |user_uid| are accepted too.
Result CheckExplicit(const std::string& path,
                     uint32_t user_uid,
                     const FileSystemView& fs);

// The first of |bundled_paths| (each inside |bundle_dir|) that passes
// CheckBundled, else the first of |system_paths| that passes CheckSystem.
// |bundle_dir| may be empty (nothing is bundled on this platform).
Result Find(const std::string& bundle_dir,
            const std::vector<std::string>& bundled_paths,
            const std::vector<std::string>& system_paths,
            const FileSystemView& fs);

}  // namespace trusted_binary
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_TRUSTED_BINARY_H_
