// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/trusted_binary.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace trusted_binary {
namespace {

constexpr uint32_t kRoot = 0;
constexpr uint32_t kUser = 501;
constexpr uint32_t kOtherUser = 502;

// An in-memory filesystem: entries by canonical path, plus symlinks (by
// their own canonical path) whose targets are canonical paths.
class FakeFileSystem : public FileSystemView {
 public:
  void Dir(const std::string& path,
           uint32_t uid = kRoot,
           uint32_t mode = 0755) {
    entries_[path] = {PathInfo::Type::kDirectory, uid, mode, false};
  }
  void File(const std::string& path,
            uint32_t uid = kRoot,
            uint32_t mode = 0755,
            bool executable = true) {
    entries_[path] = {PathInfo::Type::kRegular, uid, mode, executable};
  }
  void Link(const std::string& path, const std::string& target) {
    entries_[path] = {PathInfo::Type::kSymlink, kRoot, 0777, false};
    links_[path] = target;
  }
  // Root-owned 0755 directories for every ancestor of |path|.
  void Parents(const std::string& path) {
    Dir("/");
    for (size_t pos = path.find('/', 1); pos != std::string::npos;
         pos = path.find('/', pos + 1)) {
      if (!entries_.count(path.substr(0, pos))) {
        Dir(path.substr(0, pos));
      }
    }
  }

  std::optional<PathInfo> Lstat(const std::string& path) const override {
    auto it = entries_.find(path);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    PathInfo info;
    info.type = it->second.type;
    info.owner_uid = it->second.uid;
    info.mode = it->second.mode;
    return info;
  }

  std::optional<std::string> RealPath(const std::string& path) const override {
    if (path.empty() || path[0] != '/') {
      return std::nullopt;
    }
    std::string current;
    size_t start = 1;
    while (start <= path.size()) {
      size_t end = path.find('/', start);
      if (end == std::string::npos) {
        end = path.size();
      }
      current += "/" + path.substr(start, end - start);
      for (int hops = 0; links_.count(current); ++hops) {
        if (hops > 8) {
          return std::nullopt;
        }
        current = links_.at(current);
      }
      if (!entries_.count(current)) {
        return std::nullopt;
      }
      start = end + 1;
    }
    return current.empty() ? std::string("/") : current;
  }

  bool IsExecutable(const std::string& path) const override {
    auto it = entries_.find(path);
    return it != entries_.end() && it->second.executable;
  }

 private:
  struct Entry {
    PathInfo::Type type;
    uint32_t uid;
    uint32_t mode;
    bool executable;
  };
  std::map<std::string, Entry> entries_;
  std::map<std::string, std::string> links_;
};

// A typical Linux /usr/bin/tor.
FakeFileSystem SystemTor() {
  FakeFileSystem fs;
  fs.Parents("/usr/bin/tor");
  fs.File("/usr/bin/tor");
  return fs;
}

TEST(TrustedBinaryTest, SystemBinaryAccepted) {
  FakeFileSystem fs = SystemTor();
  Result result = CheckSystem("/usr/bin/tor", fs);
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(Source::kSystem, result.source);
  EXPECT_EQ("/usr/bin/tor", result.path);
}

TEST(TrustedBinaryTest, SystemBinaryThroughRootOwnedSymlinks) {
  // Arch / Fedora: /usr/sbin -> bin.
  FakeFileSystem fs = SystemTor();
  fs.Link("/usr/sbin", "/usr/bin");
  Result result = CheckSystem("/usr/sbin/tor", fs);
  ASSERT_TRUE(result.ok()) << result.error;
  // The canonical path is what gets executed.
  EXPECT_EQ("/usr/bin/tor", result.path);
}

TEST(TrustedBinaryTest, SystemBinaryRejected) {
  {
    FakeFileSystem fs = SystemTor();
    fs.File("/usr/bin/tor", kUser);
    Result result = CheckSystem("/usr/bin/tor", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.rejected_existing);
    EXPECT_EQ("/usr/bin/tor is owned by uid 501", result.error);
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.File("/usr/bin/tor", kRoot, 0775);
    Result result = CheckSystem("/usr/bin/tor", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ("/usr/bin/tor can be modified by its group or other users",
              result.error);
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.File("/usr/bin/tor", kRoot, 0757);
    EXPECT_FALSE(CheckSystem("/usr/bin/tor", fs).ok());
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.Dir("/usr/bin", kRoot, 0775);
    Result result = CheckSystem("/usr/bin/tor", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ("/usr/bin can be modified by its group or other users",
              result.error);
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.Dir("/", kRoot, 01777);
    EXPECT_FALSE(CheckSystem("/usr/bin/tor", fs).ok());
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.Dir("/usr", kUser);
    Result result = CheckSystem("/usr/bin/tor", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ("/usr is owned by uid 501", result.error);
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.File("/usr/bin/tor", kRoot, 04755);
    Result result = CheckSystem("/usr/bin/tor", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ("/usr/bin/tor is setuid or setgid", result.error);
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.File("/usr/bin/tor", kRoot, 02755);
    EXPECT_FALSE(CheckSystem("/usr/bin/tor", fs).ok());
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.File("/usr/bin/tor", kRoot, 0644);
    Result result = CheckSystem("/usr/bin/tor", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ("/usr/bin/tor is not executable", result.error);
  }
  {
    // Mode says executable but access(X_OK) disagrees (e.g. noexec mount).
    FakeFileSystem fs = SystemTor();
    fs.File("/usr/bin/tor", kRoot, 0755, /*executable=*/false);
    EXPECT_FALSE(CheckSystem("/usr/bin/tor", fs).ok());
  }
  {
    FakeFileSystem fs = SystemTor();
    fs.Dir("/usr/bin/tor");
    Result result = CheckSystem("/usr/bin/tor", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ("/usr/bin/tor is not a regular file", result.error);
  }
}

TEST(TrustedBinaryTest, SystemSymlinkIntoUserDirectoryRejected) {
  FakeFileSystem fs = SystemTor();
  fs.Parents("/home/me/bin/tor");
  fs.Dir("/home/me", kUser);
  fs.Dir("/home/me/bin", kUser);
  fs.File("/home/me/bin/tor", kUser);
  fs.Link("/usr/bin/tor", "/home/me/bin/tor");
  Result result = CheckSystem("/usr/bin/tor", fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ("/home/me/bin/tor is owned by uid 501", result.error);
}

TEST(TrustedBinaryTest, SystemMissingOrMalformed) {
  FakeFileSystem fs = SystemTor();
  Result result = CheckSystem("/usr/sbin/tor", fs);
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.rejected_existing);
  EXPECT_EQ("/usr/sbin/tor not found", result.error);

  result = CheckSystem("usr/bin/tor", fs);
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.rejected_existing);
  EXPECT_FALSE(CheckSystem("", fs).ok());
  EXPECT_FALSE(CheckSystem("tor", fs).ok());
}

// The FileSystemView must hand back canonical paths; anything else is
// refused rather than trusted.
TEST(TrustedBinaryTest, NonCanonicalRealPathRejected) {
  class Weird : public FakeFileSystem {
   public:
    std::optional<std::string> RealPath(const std::string&) const override {
      return std::string("/usr/bin/../bin/tor");
    }
  };
  Weird fs;
  fs.Parents("/usr/bin/tor");
  fs.File("/usr/bin/tor");
  Result result = CheckSystem("/usr/bin/tor", fs);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ("could not resolve /usr/bin/tor", result.error);
}

// macOS: the app is often owned by whoever dragged it to /Applications,
// which itself is admin-group-writable. Bundled binaries are trusted like
// the browser: inside the bundle, not writable by everyone.
FakeFileSystem MacBundle() {
  FakeFileSystem fs;
  fs.Parents("/Applications/MoltBrowser.app/Contents/Resources/tor/tor");
  fs.Dir("/Applications", kRoot, 0775);
  fs.Dir("/Applications/MoltBrowser.app", kUser);
  fs.Dir("/Applications/MoltBrowser.app/Contents", kUser);
  fs.Dir("/Applications/MoltBrowser.app/Contents/Resources", kUser);
  fs.Dir("/Applications/MoltBrowser.app/Contents/Resources/tor", kUser);
  fs.File("/Applications/MoltBrowser.app/Contents/Resources/tor/tor", kUser);
  return fs;
}
constexpr char kMacBundleDir[] =
    "/Applications/MoltBrowser.app/Contents/Resources/tor";
constexpr char kMacTor[] =
    "/Applications/MoltBrowser.app/Contents/Resources/tor/tor";

TEST(TrustedBinaryTest, BundledAccepted) {
  FakeFileSystem fs = MacBundle();
  Result result = CheckBundled(kMacBundleDir, kMacTor, fs);
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(Source::kBundled, result.source);
  EXPECT_EQ(kMacTor, result.path);

  // Group-writable is tolerated inside the bundle (same as the browser).
  fs.File(kMacTor, kUser, 0775);
  EXPECT_TRUE(CheckBundled(kMacBundleDir, kMacTor, fs).ok());
}

TEST(TrustedBinaryTest, BundledRejected) {
  {
    FakeFileSystem fs = MacBundle();
    fs.File(kMacTor, kUser, 0757);
    Result result = CheckBundled(kMacBundleDir, kMacTor, fs);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.rejected_existing);
    EXPECT_EQ(std::string(kMacTor) + " can be modified by all users",
              result.error);
  }
  {
    FakeFileSystem fs = MacBundle();
    fs.Dir(kMacBundleDir, kUser, 0777);
    EXPECT_FALSE(CheckBundled(kMacBundleDir, kMacTor, fs).ok());
  }
  {
    FakeFileSystem fs = MacBundle();
    fs.File(kMacTor, kUser, 04755);
    EXPECT_FALSE(CheckBundled(kMacBundleDir, kMacTor, fs).ok());
  }
  {
    FakeFileSystem fs = MacBundle();
    fs.File(kMacTor, kUser, 0644);
    EXPECT_FALSE(CheckBundled(kMacBundleDir, kMacTor, fs).ok());
  }
  {
    // A symlink that leads out of the bundle.
    FakeFileSystem fs = MacBundle();
    fs.Parents("/tmp/evil/tor");
    fs.File("/tmp/evil/tor", kUser);
    fs.Link(kMacTor, "/tmp/evil/tor");
    Result result = CheckBundled(kMacBundleDir, kMacTor, fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(std::string(kMacTor) + " resolves outside " + kMacBundleDir,
              result.error);
  }
  {
    // A world-writable directory between the bundle dir and the binary.
    FakeFileSystem fs;
    fs.Parents("/opt/moltbrowser/ocr/tesseract");
    fs.Dir("/opt/moltbrowser/ocr", kRoot, 0777);
    fs.File("/opt/moltbrowser/ocr/tesseract");
    EXPECT_TRUE(CheckBundled("/opt/moltbrowser", "/opt/moltbrowser/x", fs)
                    .error.find("not found") != std::string::npos);
    Result result =
        CheckBundled("/opt/moltbrowser", "/opt/moltbrowser/ocr/tesseract", fs);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ("/opt/moltbrowser/ocr can be modified by all users",
              result.error);
  }
  {
    FakeFileSystem fs = MacBundle();
    // The bundle dir itself is not inside itself.
    EXPECT_FALSE(CheckBundled(kMacBundleDir, kMacBundleDir, fs).ok());
    EXPECT_FALSE(CheckBundled("", kMacTor, fs).ok());
    EXPECT_FALSE(CheckBundled(kMacBundleDir, "", fs).ok());
  }
}

TEST(TrustedBinaryTest, ExplicitOverride) {
  FakeFileSystem fs;
  fs.Parents("/home/me/tor/tor");
  fs.Dir("/home/me", kUser);
  fs.Dir("/home/me/tor", kUser, 0700);
  fs.File("/home/me/tor/tor", kUser, 0700);
  Result result = CheckExplicit("/home/me/tor/tor", kUser, fs);
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(Source::kExplicit, result.source);
  // Someone else's binary, or one others can modify, is refused.
  EXPECT_FALSE(CheckExplicit("/home/me/tor/tor", kOtherUser, fs).ok());
  fs.Dir("/home/me", kUser, 0775);
  EXPECT_FALSE(CheckExplicit("/home/me/tor/tor", kUser, fs).ok());
  // Root-owned system binaries are fine too.
  FakeFileSystem system = SystemTor();
  EXPECT_TRUE(CheckExplicit("/usr/bin/tor", kUser, system).ok());
  EXPECT_FALSE(CheckExplicit("tor", kUser, system).ok());
}

TEST(TrustedBinaryTest, FindOrder) {
  FakeFileSystem fs = SystemTor();
  fs.Parents("/opt/moltbrowser/tor/tor");
  fs.File("/opt/moltbrowser/tor/tor");
  const std::vector<std::string> bundled = {"/opt/moltbrowser/tor/tor"};
  const std::vector<std::string> system = {"/usr/bin/tor", "/usr/sbin/tor"};

  // Bundled first.
  Result result = Find("/opt/moltbrowser/tor", bundled, system, fs);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(Source::kBundled, result.source);
  EXPECT_EQ("/opt/moltbrowser/tor/tor", result.path);

  // A tampered bundled copy is skipped, not trusted.
  fs.File("/opt/moltbrowser/tor/tor", kRoot, 0777);
  result = Find("/opt/moltbrowser/tor", bundled, system, fs);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(Source::kSystem, result.source);
  EXPECT_EQ("/usr/bin/tor", result.path);

  // No bundle dir on this platform: system only.
  result = Find("", bundled, system, fs);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(Source::kSystem, result.source);

  // No system paths (macOS / Windows): bundled only.
  result = Find("/opt/moltbrowser/tor", bundled, {}, fs);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.rejected_existing);
}

TEST(TrustedBinaryTest, FindReportsTheMostUsefulError) {
  FakeFileSystem fs = SystemTor();
  fs.File("/usr/bin/tor", kRoot, 0775);
  Result result = Find("/opt/moltbrowser/tor", {"/opt/moltbrowser/tor/tor"},
                       {"/usr/sbin/tor", "/usr/bin/tor"}, fs);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.rejected_existing);
  EXPECT_EQ("/usr/bin/tor can be modified by its group or other users",
            result.error);

  // Nothing exists at all.
  FakeFileSystem empty;
  result = Find("/opt/moltbrowser/tor", {"/opt/moltbrowser/tor/tor"},
                {"/usr/bin/tor"}, empty);
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.rejected_existing);
  EXPECT_EQ("/opt/moltbrowser/tor/tor not found", result.error);
}

// ---- The real filesystem ----

class ScopedTempDirectory {
 public:
  ScopedTempDirectory() {
    const char* tmp = getenv("TMPDIR");
    std::string pattern =
        std::string(tmp && *tmp ? tmp : "/tmp") + "/molt_trusted_XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (mkdtemp(buffer.data())) {
      path_ = buffer.data();
    }
  }
  ~ScopedTempDirectory() {
    for (auto it = created_.rbegin(); it != created_.rend(); ++it) {
      unlink(it->c_str());
      rmdir(it->c_str());
    }
    if (!path_.empty()) {
      rmdir(path_.c_str());
    }
  }
  const std::string& path() const { return path_; }
  void Track(const std::string& path) { created_.push_back(path); }

 private:
  std::string path_;
  std::vector<std::string> created_;
};

bool WriteExecutable(const std::string& path, mode_t mode) {
  const int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0700);
  if (fd < 0) {
    return false;
  }
  close(fd);
  return chmod(path.c_str(), mode) == 0;
}

TEST(TrustedBinaryTest, RealFileSystemBundled) {
  ScopedTempDirectory temp;
  ASSERT_FALSE(temp.path().empty());
  const FileSystemView& fs = RealFileSystem();
  const std::string bundle = temp.path() + "/tor";
  ASSERT_EQ(0, mkdir(bundle.c_str(), 0755));
  temp.Track(bundle);
  const std::string tor = bundle + "/tor";
  ASSERT_TRUE(WriteExecutable(tor, 0755));
  temp.Track(tor);

  Result result = CheckBundled(bundle, tor, fs);
  ASSERT_TRUE(result.ok()) << result.error;
  // mkdtemp may live under a symlinked TMPDIR (/tmp -> /private/tmp on
  // macOS); the result is canonical.
  std::optional<std::string> canonical = fs.RealPath(tor);
  ASSERT_TRUE(canonical);
  EXPECT_EQ(*canonical, result.path);

  ASSERT_EQ(0, chmod(tor.c_str(), 0757));
  EXPECT_FALSE(CheckBundled(bundle, tor, fs).ok());
  ASSERT_EQ(0, chmod(tor.c_str(), 0644));
  EXPECT_FALSE(CheckBundled(bundle, tor, fs).ok());
  ASSERT_EQ(0, chmod(tor.c_str(), 0755));

  // A symlink in the bundle pointing out of it.
  const std::string outside = temp.path() + "/outside";
  ASSERT_TRUE(WriteExecutable(outside, 0755));
  temp.Track(outside);
  const std::string link = bundle + "/link";
  ASSERT_EQ(0, symlink(outside.c_str(), link.c_str()));
  temp.Track(link);
  EXPECT_FALSE(CheckBundled(bundle, link, fs).ok());

  EXPECT_FALSE(CheckBundled(bundle, bundle + "/missing", fs).ok());
}

TEST(TrustedBinaryTest, RealFileSystemSystemShell) {
  // /bin/sh is root-owned on every POSIX system MoltBrowser supports, but a
  // test machine may still differ; only check the result is consistent.
  Result result = CheckSystem("/bin/sh", RealFileSystem());
  if (result.ok()) {
    EXPECT_EQ(Source::kSystem, result.source);
    EXPECT_EQ('/', result.path[0]);
  } else {
    EXPECT_FALSE(result.error.empty());
  }
  // A temp file owned by the current (non-root) user is never a system
  // binary.
  if (CurrentUid() != 0) {
    ScopedTempDirectory temp;
    ASSERT_FALSE(temp.path().empty());
    const std::string file = temp.path() + "/tor";
    ASSERT_TRUE(WriteExecutable(file, 0755));
    temp.Track(file);
    Result system = CheckSystem(file, RealFileSystem());
    EXPECT_FALSE(system.ok());
    EXPECT_TRUE(system.rejected_existing);
  }
}

}  // namespace
}  // namespace trusted_binary
}  // namespace molt_ai
