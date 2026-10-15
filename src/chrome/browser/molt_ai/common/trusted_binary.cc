// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/trusted_binary.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <utility>

namespace molt_ai {
namespace trusted_binary {

namespace {

constexpr uint32_t kGroupWrite = 0020;
constexpr uint32_t kOtherWrite = 0002;
constexpr uint32_t kAnyExecute = 0111;
constexpr uint32_t kSetUidOrGid = 06000;
constexpr uint32_t kRootUid = 0;

Result Reject(std::string error, bool exists) {
  Result result;
  result.error = std::move(error);
  result.rejected_existing = exists;
  return result;
}

Result Accept(Source source, std::string path) {
  Result result;
  result.source = source;
  result.path = std::move(path);
  return result;
}

// "/a/b/c": absolute, no empty, "." or ".." components, no trailing slash.
bool IsCanonicalAbsolute(const std::string& path) {
  if (path.empty() || path[0] != '/') {
    return false;
  }
  if (path == "/") {
    return true;
  }
  size_t start = 1;
  while (true) {
    const size_t end = path.find('/', start);
    const std::string component = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == std::string::npos) {
      return true;
    }
    start = end + 1;
  }
}

// True iff canonical |path| is strictly below canonical directory |dir|.
bool IsStrictlyInside(const std::string& dir, const std::string& path) {
  if (dir == "/") {
    return path.size() > 1 && path[0] == '/';
  }
  return path.size() > dir.size() + 1 &&
         path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

// The directories that contain canonical |path|, from |top| (inclusive,
// canonical, an ancestor of |path|) down to its parent. With |top| "/":
// "/usr/bin/tor" -> {"/", "/usr", "/usr/bin"}.
std::vector<std::string> DirectoriesDownTo(const std::string& top,
                                           const std::string& path) {
  std::vector<std::string> dirs;
  dirs.push_back(top);
  size_t pos = top == "/" ? 0 : top.size();
  while (true) {
    const size_t next = path.find('/', pos + 1);
    if (next == std::string::npos) {
      return dirs;
    }
    dirs.push_back(path.substr(0, next));
    pos = next;
  }
}

std::string OwnerText(uint32_t uid) {
  return uid == kRootUid ? std::string("root") : "uid " + std::to_string(uid);
}

// Checks the file at canonical |path| and every directory from |top| down
// to it. |allowed_owners| empty = any owner. |forbidden_write| = permission
// bits that must be clear on all of them.
std::optional<std::string> CheckChain(
    const std::string& top,
    const std::string& path,
    const std::vector<uint32_t>& allowed_owners,
    uint32_t forbidden_write,
    const FileSystemView& fs) {
  auto owner_ok = [&](uint32_t uid) {
    if (allowed_owners.empty()) {
      return true;
    }
    for (uint32_t allowed : allowed_owners) {
      if (uid == allowed) {
        return true;
      }
    }
    return false;
  };
  const std::string writable_by =
      forbidden_write & kGroupWrite ? "its group or other users" : "all users";

  std::optional<PathInfo> file = fs.Lstat(path);
  if (!file || file->type != PathInfo::Type::kRegular) {
    return path + " is not a regular file";
  }
  if (!owner_ok(file->owner_uid)) {
    return path + " is owned by " + OwnerText(file->owner_uid);
  }
  if (file->mode & forbidden_write) {
    return path + " can be modified by " + writable_by;
  }
  if (file->mode & kSetUidOrGid) {
    return path + " is setuid or setgid";
  }
  if (!(file->mode & kAnyExecute) || !fs.IsExecutable(path)) {
    return path + " is not executable";
  }
  for (const std::string& dir : DirectoriesDownTo(top, path)) {
    std::optional<PathInfo> info = fs.Lstat(dir);
    if (!info || info->type != PathInfo::Type::kDirectory) {
      return dir + " is not a directory";
    }
    if (!owner_ok(info->owner_uid)) {
      return dir + " is owned by " + OwnerText(info->owner_uid);
    }
    if (info->mode & forbidden_write) {
      return dir + " can be modified by " + writable_by;
    }
  }
  return std::nullopt;
}

// Resolves an absolute |path|. Sets |error| (and |exists|) on failure.
std::optional<std::string> Resolve(const std::string& path,
                                   const FileSystemView& fs,
                                   std::string* error,
                                   bool* exists) {
  *exists = false;
  if (path.empty() || path[0] != '/') {
    *error = "\"" + path + "\" is not an absolute path";
    return std::nullopt;
  }
  std::optional<std::string> real = fs.RealPath(path);
  if (!real) {
    *error = path + " not found";
    return std::nullopt;
  }
  *exists = true;
  if (!IsCanonicalAbsolute(*real)) {
    *error = "could not resolve " + path;
    return std::nullopt;
  }
  return real;
}

class PosixFileSystem : public FileSystemView {
 public:
  std::optional<PathInfo> Lstat(const std::string& path) const override {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
      return std::nullopt;
    }
    PathInfo info;
    if (S_ISREG(st.st_mode)) {
      info.type = PathInfo::Type::kRegular;
    } else if (S_ISDIR(st.st_mode)) {
      info.type = PathInfo::Type::kDirectory;
    } else if (S_ISLNK(st.st_mode)) {
      info.type = PathInfo::Type::kSymlink;
    }
    info.owner_uid = static_cast<uint32_t>(st.st_uid);
    info.mode = static_cast<uint32_t>(st.st_mode) & 07777;
    return info;
  }

  std::optional<std::string> RealPath(const std::string& path) const override {
    struct FreeDeleter {
      void operator()(char* p) const { free(p); }
    };
    std::unique_ptr<char, FreeDeleter> resolved(
        realpath(path.c_str(), nullptr));
    if (!resolved) {
      return std::nullopt;
    }
    return std::string(resolved.get());
  }

  bool IsExecutable(const std::string& path) const override {
    return access(path.c_str(), X_OK) == 0;
  }
};

}  // namespace

Result::Result() = default;
Result::Result(const Result&) = default;
Result& Result::operator=(const Result&) = default;
Result::~Result() = default;

const FileSystemView& RealFileSystem() {
  static const PosixFileSystem* const kFileSystem = new PosixFileSystem();
  return *kFileSystem;
}

uint32_t CurrentUid() {
  return static_cast<uint32_t>(geteuid());
}

Result CheckBundled(const std::string& bundle_dir,
                    const std::string& path,
                    const FileSystemView& fs) {
  std::string error;
  bool exists = false;
  std::optional<std::string> real = Resolve(path, fs, &error, &exists);
  if (!real) {
    return Reject(error, exists);
  }
  bool dir_exists = false;
  std::optional<std::string> real_dir =
      Resolve(bundle_dir, fs, &error, &dir_exists);
  if (!real_dir) {
    return Reject(error, exists);
  }
  // A symlink in the bundle must not lead out of it.
  if (!IsStrictlyInside(*real_dir, *real)) {
    return Reject(path + " resolves outside " + bundle_dir, true);
  }
  // Same trust as the browser executable, which may be owned by whoever
  // installed it: no owner check, but nobody else may swap it.
  if (std::optional<std::string> problem =
          CheckChain(*real_dir, *real, {}, kOtherWrite, fs)) {
    return Reject(*problem, true);
  }
  return Accept(Source::kBundled, *real);
}

Result CheckSystem(const std::string& path, const FileSystemView& fs) {
  std::string error;
  bool exists = false;
  std::optional<std::string> real = Resolve(path, fs, &error, &exists);
  if (!real) {
    return Reject(error, exists);
  }
  if (std::optional<std::string> problem =
          CheckChain("/", *real, {kRootUid}, kGroupWrite | kOtherWrite, fs)) {
    return Reject(*problem, true);
  }
  return Accept(Source::kSystem, *real);
}

Result CheckExplicit(const std::string& path,
                     uint32_t user_uid,
                     const FileSystemView& fs) {
  std::string error;
  bool exists = false;
  std::optional<std::string> real = Resolve(path, fs, &error, &exists);
  if (!real) {
    return Reject(error, exists);
  }
  if (std::optional<std::string> problem = CheckChain(
          "/", *real, {kRootUid, user_uid}, kGroupWrite | kOtherWrite, fs)) {
    return Reject(*problem, true);
  }
  return Accept(Source::kExplicit, *real);
}

Result Find(const std::string& bundle_dir,
            const std::vector<std::string>& bundled_paths,
            const std::vector<std::string>& system_paths,
            const FileSystemView& fs) {
  // Report the first refusal, unless a later candidate actually existed and
  // the first one didn't ("/usr/bin/tor is group-writable" is more useful
  // than "<bundle>/tor not found").
  Result best = Reject("no candidate", false);
  bool have_error = false;
  auto consider = [&best, &have_error](Result result) {
    if (!have_error || (result.rejected_existing && !best.rejected_existing)) {
      best = std::move(result);
      have_error = true;
    }
  };
  if (!bundle_dir.empty()) {
    for (const std::string& path : bundled_paths) {
      Result result = CheckBundled(bundle_dir, path, fs);
      if (result.ok()) {
        return result;
      }
      consider(std::move(result));
    }
  }
  for (const std::string& path : system_paths) {
    Result result = CheckSystem(path, fs);
    if (result.ok()) {
      return result;
    }
    consider(std::move(result));
  }
  return best;
}

}  // namespace trusted_binary
}  // namespace molt_ai
