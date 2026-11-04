// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser's own files are read and written only when they are regular
// files (molt_regular_file.h): a FIFO, a socket, a directory or a link not
// followed is never opened, nor written over; a link to a regular file in
// the home directory is read and written through where the file allows it.

#include "chrome/browser/molt_ai/common/molt_regular_file.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/base_paths.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/scoped_path_override.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

#if BUILDFLAG(IS_POSIX)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#endif

namespace molt_ai {
namespace {

class MoltRegularFileTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    home_ = temp_.GetPath().AppendASCII("home");
    elsewhere_ = temp_.GetPath().AppendASCII("elsewhere");
    ASSERT_TRUE(base::CreateDirectory(home_));
    ASSERT_TRUE(base::CreateDirectory(elsewhere_));
    home_override_.emplace(base::DIR_HOME, home_);
  }

  base::FilePath InHome(const char* name) { return home_.AppendASCII(name); }
  std::string Contents(const base::FilePath& path) {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path, &contents));
    return contents;
  }

  base::ScopedTempDir temp_;
  base::FilePath home_;
  base::FilePath elsewhere_;
  std::optional<base::ScopedPathOverride> home_override_;
};

TEST_F(MoltRegularFileTest, ReadsARegularFileWithinItsCap) {
  const base::FilePath file = InHome("settings.json");
  EXPECT_EQ(ReadMoltFile(file, 64, MoltFileLinks::kNotFollowed).kind,
            MoltFileKind::kMissing);
  EXPECT_EQ(
      ReadMoltFile(InHome("no/dir.json"), 64, MoltFileLinks::kNotFollowed).kind,
      MoltFileKind::kMissing);
  ASSERT_TRUE(base::WriteFile(file, "{\"a\": 1}"));
  MoltFileRead read = ReadMoltFile(file, 64, MoltFileLinks::kNotFollowed);
  EXPECT_EQ(read.kind, MoltFileKind::kRegular);
  EXPECT_EQ(read.contents, "{\"a\": 1}");
  // Exactly the cap is read; a byte more isn't.
  read = ReadMoltFile(file, 8, MoltFileLinks::kNotFollowed);
  EXPECT_EQ(read.kind, MoltFileKind::kRegular);
  read = ReadMoltFile(file, 7, MoltFileLinks::kNotFollowed);
  EXPECT_EQ(read.kind, MoltFileKind::kTooBig);
  EXPECT_TRUE(read.contents.empty());
  // A huge file isn't read at all.
  const base::FilePath huge = InHome("huge.molt");
  ASSERT_TRUE(base::WriteFile(huge, std::string(256 * 1024, 'x')));
  EXPECT_EQ(ReadMoltFile(huge, 64 * 1024, MoltFileLinks::kNotFollowed).kind,
            MoltFileKind::kTooBig);
  EXPECT_EQ(MoltFileKindAt(huge, MoltFileLinks::kNotFollowed),
            MoltFileKind::kRegular);
}

TEST_F(MoltRegularFileTest, ADirectoryIsNotRegular) {
  const base::FilePath dir = InHome("settings.json");
  ASSERT_TRUE(base::CreateDirectory(dir));
  EXPECT_EQ(MoltFileKindAt(dir, MoltFileLinks::kFollowedWithinHome),
            MoltFileKind::kNotRegular);
  EXPECT_EQ(ReadMoltFile(dir, 64, MoltFileLinks::kFollowedWithinHome).kind,
            MoltFileKind::kNotRegular);
  EXPECT_FALSE(MoltFileWritePath(dir, MoltFileLinks::kFollowedWithinHome));
  EXPECT_FALSE(AppendToMoltFile(dir, "line\n"));
  EXPECT_FALSE(ReadMoltFileTail(dir, 64));
  EXPECT_TRUE(base::DirectoryExists(dir));
}

TEST_F(MoltRegularFileTest, WritesGoToTheFileOrWhereItIsMissing) {
  const base::FilePath file = InHome("settings.json");
  EXPECT_EQ(MoltFileWritePath(file, MoltFileLinks::kNotFollowed), file);
  ASSERT_TRUE(base::WriteFile(file, "{}"));
  EXPECT_EQ(MoltFileWritePath(file, MoltFileLinks::kNotFollowed), file);
  EXPECT_EQ(MoltFileWritePath(file, MoltFileLinks::kFollowedWithinHome), file);
}

TEST_F(MoltRegularFileTest, AppendsCreatesAndReadsTheTail) {
  const base::FilePath log = InHome("audit.log");
  EXPECT_EQ(ReadMoltFileTail(log, 64), std::string());
  // Created when missing (base::AppendToFile() doesn't).
  ASSERT_TRUE(AppendToMoltFile(log, "one\n"));
  ASSERT_TRUE(AppendToMoltFile(log, "two\n"));
  ASSERT_TRUE(AppendToMoltFile(log, "three\n"));
  EXPECT_EQ(Contents(log), "one\ntwo\nthree\n");
  EXPECT_EQ(ReadMoltFileTail(log, 64), "one\ntwo\nthree\n");
  // Cut: from the first whole line in the last bytes (a line that starts
  // exactly at them included).
  EXPECT_EQ(ReadMoltFileTail(log, 9), "three\n");
  EXPECT_EQ(ReadMoltFileTail(log, 10), "two\nthree\n");
  EXPECT_EQ(ReadMoltFileTail(log, 11), "two\nthree\n");
  // No line ends in them: nothing.
  EXPECT_EQ(ReadMoltFileTail(log, 3), "");
#if BUILDFLAG(IS_POSIX)
  struct stat st;
  ASSERT_EQ(stat(log.value().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
#endif
}

#if BUILDFLAG(IS_POSIX)

// A FIFO with no writer: opening it to read would wait forever.
TEST_F(MoltRegularFileTest, AFifoIsNeverOpened) {
  const base::FilePath fifo = InHome("settings.json");
  ASSERT_EQ(mkfifo(fifo.value().c_str(), 0600), 0);
  for (MoltFileLinks links :
       {MoltFileLinks::kNotFollowed, MoltFileLinks::kFollowedWithinHome}) {
    EXPECT_EQ(MoltFileKindAt(fifo, links), MoltFileKind::kNotRegular);
    EXPECT_EQ(ReadMoltFile(fifo, 1024, links).kind, MoltFileKind::kNotRegular);
    EXPECT_FALSE(MoltFileWritePath(fifo, links));
  }
  // Nor written to (an open() for writing would wait for a reader).
  EXPECT_FALSE(AppendToMoltFile(fifo, "line\n"));
  EXPECT_FALSE(ReadMoltFileTail(fifo, 64));
  struct stat st;
  ASSERT_EQ(lstat(fifo.value().c_str(), &st), 0);
  EXPECT_TRUE(S_ISFIFO(st.st_mode));
}

TEST_F(MoltRegularFileTest, ASocketIsNeverOpened) {
  // A short path: sun_path is about 100 bytes.
  base::ScopedTempDir short_dir;
  ASSERT_TRUE(short_dir.CreateUniqueTempDir());
  const base::FilePath path = short_dir.GetPath().AppendASCII("s.json");
  if (path.value().size() >= sizeof(sockaddr_un::sun_path)) {
    GTEST_SKIP() << "temporary directory path too long for a socket";
  }
  base::ScopedFD sock(socket(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_TRUE(sock.is_valid());
  sockaddr_un address = {};
  address.sun_family = AF_UNIX;
  path.value().copy(address.sun_path, path.value().size());
  ASSERT_EQ(
      bind(sock.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)),
      0);
  EXPECT_EQ(ReadMoltFile(path, 64, MoltFileLinks::kNotFollowed).kind,
            MoltFileKind::kNotRegular);
  EXPECT_FALSE(MoltFileWritePath(path, MoltFileLinks::kNotFollowed));
  EXPECT_FALSE(AppendToMoltFile(path, "line\n"));
}

// A link to a regular file inside the home directory, where the file may
// be one (settings.json kept by a dotfile manager): read and written
// through. Elsewhere: not.
TEST_F(MoltRegularFileTest, LinksAreFollowedOnlyWithinHomeAndWhereAllowed) {
  const base::FilePath dotfiles = InHome("dotfiles");
  ASSERT_TRUE(base::CreateDirectory(dotfiles));
  const base::FilePath target = dotfiles.AppendASCII("settings.json");
  ASSERT_TRUE(base::WriteFile(target, "{\"voice_mode\": true}"));
  const base::FilePath link = InHome("settings.json");
  ASSERT_TRUE(base::CreateSymbolicLink(target, link));

  MoltFileRead read =
      ReadMoltFile(link, 64, MoltFileLinks::kFollowedWithinHome);
  EXPECT_EQ(read.kind, MoltFileKind::kRegular);
  EXPECT_EQ(read.contents, "{\"voice_mode\": true}");
  std::optional<base::FilePath> write =
      MoltFileWritePath(link, MoltFileLinks::kFollowedWithinHome);
  ASSERT_TRUE(write);
  EXPECT_EQ(*write, base::MakeAbsoluteFilePath(target));
  // Never for the files moved and deleted by name.
  EXPECT_EQ(ReadMoltFile(link, 64, MoltFileLinks::kNotFollowed).kind,
            MoltFileKind::kNotRegular);
  EXPECT_FALSE(MoltFileWritePath(link, MoltFileLinks::kNotFollowed));
  EXPECT_FALSE(AppendToMoltFile(link, "line\n"));
  EXPECT_EQ(Contents(target), "{\"voice_mode\": true}");

  // Outside the home directory: neither read nor written.
  const base::FilePath outside = elsewhere_.AppendASCII("settings.json");
  ASSERT_TRUE(base::WriteFile(outside, "{}"));
  const base::FilePath out_link = InHome("out.json");
  ASSERT_TRUE(base::CreateSymbolicLink(outside, out_link));
  EXPECT_EQ(ReadMoltFile(out_link, 64, MoltFileLinks::kFollowedWithinHome).kind,
            MoltFileKind::kNotRegular);
  EXPECT_FALSE(MoltFileWritePath(out_link, MoltFileLinks::kFollowedWithinHome));

  // Nor through ".." out of it.
  const base::FilePath up_link = InHome("up.json");
  ASSERT_TRUE(base::CreateSymbolicLink(
      base::FilePath("../elsewhere/settings.json"), up_link));
  EXPECT_EQ(ReadMoltFile(up_link, 64, MoltFileLinks::kFollowedWithinHome).kind,
            MoltFileKind::kNotRegular);

  // To nothing, or to a FIFO in the home directory: neither.
  const base::FilePath dangling = InHome("dangling.json");
  ASSERT_TRUE(
      base::CreateSymbolicLink(dotfiles.AppendASCII("gone.json"), dangling));
  EXPECT_EQ(MoltFileKindAt(dangling, MoltFileLinks::kFollowedWithinHome),
            MoltFileKind::kNotRegular);
  EXPECT_FALSE(MoltFileWritePath(dangling, MoltFileLinks::kFollowedWithinHome));
  const base::FilePath fifo = dotfiles.AppendASCII("fifo.json");
  ASSERT_EQ(mkfifo(fifo.value().c_str(), 0600), 0);
  const base::FilePath fifo_link = InHome("fifo_link.json");
  ASSERT_TRUE(base::CreateSymbolicLink(fifo, fifo_link));
  EXPECT_EQ(
      ReadMoltFile(fifo_link, 64, MoltFileLinks::kFollowedWithinHome).kind,
      MoltFileKind::kNotRegular);

  // A chain of links ending at a regular file in the home directory is
  // followed.
  const base::FilePath chain = InHome("chain.json");
  ASSERT_TRUE(base::CreateSymbolicLink(link, chain));
  read = ReadMoltFile(chain, 64, MoltFileLinks::kFollowedWithinHome);
  EXPECT_EQ(read.kind, MoltFileKind::kRegular);
}

#endif  // BUILDFLAG(IS_POSIX)

// The writer's temporary file for |name| in |dir|, as Chromium names it
// (base::CreateAndOpenTemporaryFileInDir() with the name as the prefix),
// last changed |age| ago.
base::FilePath WriteTemporary(const base::FilePath& dir,
                              const std::string& name,
                              base::TimeDelta age) {
  base::FilePath temporary;
  base::File file = base::CreateAndOpenTemporaryFileInDir(
      dir, &temporary, /*additional_flags=*/0,
      base::FilePath::FromUTF8Unsafe(name).value());
  EXPECT_TRUE(file.IsValid());
  file.Close();
  const base::Time when = base::Time::Now() - age;
  EXPECT_TRUE(base::TouchFile(temporary, when, when));
  return temporary;
}

// A crash between the temporary file and the rename leaves the former:
// only those of the files asked for, a minute old, regular, are deleted.
TEST_F(MoltRegularFileTest, OnlyStaleTemporariesOfOurFilesAreDeleted) {
  const base::FilePath stale =
      WriteTemporary(home_, "settings.json", base::Minutes(5));
  const base::FilePath fresh =
      WriteTemporary(home_, "settings.json", base::Seconds(2));
  const base::FilePath other =
      WriteTemporary(home_, "Preferences", base::Minutes(5));
  // A file of that name the user keeps is no temporary file.
  const base::FilePath kept = InHome("settings.json.bak");
  ASSERT_TRUE(base::WriteFile(kept, "{}"));
  ASSERT_TRUE(base::WriteFile(InHome("settings.json"), "{}"));

  const auto settings = [](std::string_view name) {
    return name == "settings.json";
  };
  EXPECT_EQ(DeleteStaleWriteTemporaries(home_, settings, base::Time::Now()),
            1u);
  EXPECT_FALSE(base::PathExists(stale));
  EXPECT_TRUE(base::PathExists(fresh));
  EXPECT_TRUE(base::PathExists(other));
  EXPECT_TRUE(base::PathExists(kept));
  EXPECT_EQ(Contents(InHome("settings.json")), "{}");
  // A minute later, the one written meanwhile too; a clock set back counts
  // as late.
  EXPECT_EQ(DeleteStaleWriteTemporaries(home_, settings,
                                        base::Time::Now() + base::Minutes(2)),
            1u);
  EXPECT_FALSE(base::PathExists(fresh));
  const base::FilePath future =
      WriteTemporary(home_, "settings.json", -base::Minutes(10));
  EXPECT_EQ(DeleteStaleWriteTemporaries(home_, settings, base::Time::Now()),
            1u);
  EXPECT_FALSE(base::PathExists(future));
  EXPECT_TRUE(base::PathExists(other));
  // Nothing there, or no directory: nothing.
  EXPECT_EQ(
      DeleteStaleWriteTemporaries(InHome("none"), settings, base::Time::Now()),
      0u);
}

// The lock file: created, regular, held while the lock lives, and taken
// again after it.
TEST_F(MoltRegularFileTest, ALockFileIsTakenAndGiven) {
  const base::FilePath path = InHome(".storage.lock");
  {
    MoltFileLock lock(path, base::Milliseconds(50));
    EXPECT_TRUE(lock.held());
    EXPECT_EQ(MoltFileKindAt(path, MoltFileLinks::kNotFollowed),
              MoltFileKind::kRegular);
  }
  MoltFileLock again(path, base::Milliseconds(50));
  EXPECT_TRUE(again.held());
  // No directory: no lock, and no wait.
  MoltFileLock none(InHome("none/.storage.lock"), base::Seconds(5));
  EXPECT_FALSE(none.held());
}

#if BUILDFLAG(IS_POSIX)

// Anything but a regular file at the lock's path is never opened: the
// caller goes on without the lock, at once.
TEST_F(MoltRegularFileTest, ALockIsNeverAFifoOrALink) {
  const base::FilePath fifo = InHome(".storage.lock");
  ASSERT_EQ(mkfifo(fifo.value().c_str(), 0600), 0);
  MoltFileLock on_fifo(fifo, base::Seconds(5));
  EXPECT_FALSE(on_fifo.held());

  const base::FilePath target = InHome("target");
  ASSERT_TRUE(base::WriteFile(target, ""));
  const base::FilePath link = InHome("link.lock");
  ASSERT_TRUE(base::CreateSymbolicLink(target, link));
  MoltFileLock on_link(link, base::Seconds(5));
  EXPECT_FALSE(on_link.held());
}

#endif  // BUILDFLAG(IS_POSIX)

#if BUILDFLAG(IS_WIN)

// A symbolic link (a name-surrogate reparse point) is told apart as POSIX
// tells a link apart: never followed for the files moved by name, followed
// within the home directory for settings.json and the learning file.
// Creating one needs Developer Mode or the privilege: skipped without.
bool MakeLink(const base::FilePath& target,
              const base::FilePath& link,
              bool directory) {
  return ::CreateSymbolicLinkW(
             link.value().c_str(), target.value().c_str(),
             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE |
                 (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0)) != 0;
}

TEST_F(MoltRegularFileTest, ReparsePointsAreLinks) {
  const base::FilePath dotfiles = InHome("dotfiles");
  ASSERT_TRUE(base::CreateDirectory(dotfiles));
  const base::FilePath target = dotfiles.AppendASCII("settings.json");
  ASSERT_TRUE(base::WriteFile(target, "{\"voice_mode\": true}"));
  const base::FilePath link = InHome("settings.json");
  if (!MakeLink(target, link, /*directory=*/false)) {
    GTEST_SKIP() << "symbolic links can't be made here";
  }

  // Within the home directory, for the files rewritten whole: followed.
  MoltFileRead read =
      ReadMoltFile(link, 64, MoltFileLinks::kFollowedWithinHome);
  EXPECT_EQ(read.kind, MoltFileKind::kRegular);
  EXPECT_EQ(read.contents, "{\"voice_mode\": true}");
  // A write goes to the file the link names (its final path, long names
  // and all), and the link stays.
  std::optional<base::FilePath> write =
      MoltFileWritePath(link, MoltFileLinks::kFollowedWithinHome);
  ASSERT_TRUE(write);
  ASSERT_TRUE(base::WriteFile(*write, "{\"voice_mode\": false}"));
  EXPECT_EQ(Contents(target), "{\"voice_mode\": false}");
  EXPECT_EQ(MoltFileKindAt(link, MoltFileLinks::kNotFollowed),
            MoltFileKind::kNotRegular);
  // Never for the files moved and deleted by name (a .molt, audit.log).
  EXPECT_EQ(ReadMoltFile(link, 64, MoltFileLinks::kNotFollowed).kind,
            MoltFileKind::kNotRegular);
  EXPECT_FALSE(MoltFileWritePath(link, MoltFileLinks::kNotFollowed));
  EXPECT_FALSE(AppendToMoltFile(link, "line\n"));
  EXPECT_FALSE(ReadMoltFileTail(link, 64));
  EXPECT_EQ(Contents(target), "{\"voice_mode\": false}");

  // Outside the home directory, to nothing, or to a directory: neither.
  const base::FilePath outside = elsewhere_.AppendASCII("settings.json");
  ASSERT_TRUE(base::WriteFile(outside, "{}"));
  const base::FilePath out_link = InHome("out.json");
  ASSERT_TRUE(MakeLink(outside, out_link, /*directory=*/false));
  EXPECT_EQ(MoltFileKindAt(out_link, MoltFileLinks::kFollowedWithinHome),
            MoltFileKind::kNotRegular);
  EXPECT_FALSE(MoltFileWritePath(out_link, MoltFileLinks::kFollowedWithinHome));
  const base::FilePath dangling = InHome("dangling.json");
  ASSERT_TRUE(MakeLink(dotfiles.AppendASCII("gone.json"), dangling, false));
  EXPECT_EQ(MoltFileKindAt(dangling, MoltFileLinks::kFollowedWithinHome),
            MoltFileKind::kNotRegular);
  const base::FilePath dir_link = InHome("dir.json");
  ASSERT_TRUE(MakeLink(dotfiles, dir_link, /*directory=*/true));
  EXPECT_EQ(MoltFileKindAt(dir_link, MoltFileLinks::kFollowedWithinHome),
            MoltFileKind::kNotRegular);
  EXPECT_EQ(MoltFileKindAt(dir_link, MoltFileLinks::kNotFollowed),
            MoltFileKind::kNotRegular);

  // The lock file is never one either.
  MoltFileLock on_link(link, base::Milliseconds(50));
  EXPECT_FALSE(on_link.held());
}

#endif  // BUILDFLAG(IS_WIN)

}  // namespace
}  // namespace molt_ai
