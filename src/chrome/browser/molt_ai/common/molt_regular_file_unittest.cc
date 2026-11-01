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

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/scoped_path_override.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

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

}  // namespace
}  // namespace molt_ai
