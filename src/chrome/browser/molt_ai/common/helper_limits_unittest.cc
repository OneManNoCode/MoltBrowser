// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_limits.h"

#include <stdint.h>

#include <limits>
#include <string>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/helper_limits_watch.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace molt_ai {
namespace helper_limits {
namespace {

using Gap = ScratchUsage::Gap;

constexpr uint64_t kMiB = 1u << 20;

ScratchUsage Usage(uint64_t bytes, uint64_t entries, Gap gap = Gap::kNone) {
  ScratchUsage usage;
  usage.bytes = bytes;
  usage.entries = entries;
  usage.gap = gap;
  return usage;
}

ScratchUsage Measure(const base::FilePath& dir, uint64_t max_entries = 1000) {
  return MeasureScratch(dir.value(), max_entries);
}

// A file of |size| bytes, none of them written (sparse where the file
// system can be).
void MakeFileOfLength(const base::FilePath& path, int64_t size) {
  base::File file(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  ASSERT_TRUE(file.IsValid());
  ASSERT_TRUE(file.SetLength(size));
}

TEST(HelperLimitsTest, JudgesGrowthFromWhatTheBrowserPutThere) {
  // A 100 MiB input.
  const ScratchUsage before = Usage(100 * kMiB, 1);
  EXPECT_EQ(JudgeScratch(before, before), ScratchVerdict::kWithinLimits);
  EXPECT_EQ(
      JudgeScratch(before, Usage(before.bytes + kMaxHelperWriteBytes - 1, 2)),
      ScratchVerdict::kWithinLimits);
  EXPECT_EQ(JudgeScratch(before, Usage(before.bytes + kMaxHelperWriteBytes, 2)),
            ScratchVerdict::kTooMuchData);
  // Less than there was is fine.
  EXPECT_EQ(JudgeScratch(before, Usage(0, 0)), ScratchVerdict::kWithinLimits);

  EXPECT_EQ(MaxScratchEntries(before), 1 + kMaxScratchNewEntries);
  EXPECT_EQ(JudgeScratch(before, Usage(0, 1 + kMaxScratchNewEntries)),
            ScratchVerdict::kWithinLimits);
  EXPECT_EQ(JudgeScratch(before, Usage(0, 2 + kMaxScratchNewEntries)),
            ScratchVerdict::kTooManyEntries);

  // A walk that stopped early.
  EXPECT_EQ(JudgeScratch(before, Usage(0, 1, Gap::kTooManyEntries)),
            ScratchVerdict::kTooManyEntries);
  EXPECT_EQ(JudgeScratch(before, Usage(0, 1, Gap::kTooDeep)),
            ScratchVerdict::kTooDeep);
  EXPECT_EQ(JudgeScratch(before, Usage(0, 1, Gap::kUnreadable)),
            ScratchVerdict::kUnmeasurable);

  // No overflow near the top.
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  const ScratchUsage huge = Usage(kMax - 1, kMax);
  EXPECT_EQ(MaxScratchEntries(huge), kMax);
  EXPECT_EQ(JudgeScratch(huge, Usage(kMax - 1, kMax)),
            ScratchVerdict::kWithinLimits);
}

TEST(HelperLimitsTest, SaysWhichLimit) {
  EXPECT_EQ(DescribeScratchVerdict(ScratchVerdict::kWithinLimits), "");
  EXPECT_EQ(DescribeScratchVerdict(ScratchVerdict::kTooMuchData),
            "it wrote 64 MiB or more to its scratch directory");
  EXPECT_EQ(DescribeScratchVerdict(ScratchVerdict::kTooManyEntries),
            "it made more than 64 files in its scratch directory");
  EXPECT_EQ(DescribeScratchVerdict(ScratchVerdict::kTooDeep),
            "it nested directories more than 8 deep in its scratch directory");
  EXPECT_EQ(DescribeScratchVerdict(ScratchVerdict::kUnmeasurable),
            "what it wrote to its scratch directory couldn't be measured");
  EXPECT_EQ(DescribeMemoryLimit(), "it used more than 4 GiB of memory");
}

TEST(HelperLimitsTest, MeasuresFilesAndDirectories) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  EXPECT_EQ(Measure(dir.GetPath()).entries, 0u);
  EXPECT_EQ(Measure(dir.GetPath()).gap, Gap::kNone);

  ASSERT_TRUE(base::WriteFile(dir.GetPath().AppendASCII("input.pnm"),
                              std::string(kMiB, 'x')));
  ASSERT_TRUE(base::WriteFile(dir.GetPath().AppendASCII("empty"), ""));
  const base::FilePath sub = dir.GetPath().AppendASCII("sub");
  ASSERT_TRUE(base::CreateDirectory(sub));
  ASSERT_TRUE(base::WriteFile(sub.AppendASCII("out.txt"), "text"));
  const ScratchUsage usage = Measure(dir.GetPath());
  EXPECT_EQ(usage.gap, Gap::kNone);
  EXPECT_EQ(usage.entries, 4u);
  EXPECT_GE(usage.bytes, kMiB + 4);
  EXPECT_LT(usage.bytes, 2 * kMiB);
}

TEST(HelperLimitsTest, CountsAFileWithoutDataInFull) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  MakeFileOfLength(dir.GetPath().AppendASCII("sparse"), 1LL << 30);
  const ScratchUsage usage = Measure(dir.GetPath());
  EXPECT_EQ(usage.gap, Gap::kNone);
  EXPECT_GE(usage.bytes, uint64_t{1} << 30);
}

TEST(HelperLimitsTest, StopsPastTooManyEntries) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(base::WriteFile(
        dir.GetPath().AppendASCII("file" + std::to_string(i)), "x"));
  }
  EXPECT_EQ(Measure(dir.GetPath(), 10).gap, Gap::kNone);
  const ScratchUsage usage = Measure(dir.GetPath(), 9);
  EXPECT_EQ(usage.gap, Gap::kTooManyEntries);
  EXPECT_EQ(usage.entries, 9u);
}

TEST(HelperLimitsTest, StopsAtDirectoriesTooDeep) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  base::FilePath path = dir.GetPath();
  for (int depth = 1; depth <= kMaxScratchDepth; ++depth) {
    path = path.AppendASCII("d");
    ASSERT_TRUE(base::CreateDirectory(path));
  }
  ASSERT_TRUE(base::WriteFile(path.AppendASCII("deepest"), "x"));
  EXPECT_EQ(Measure(dir.GetPath()).gap, Gap::kNone);
  ASSERT_TRUE(base::CreateDirectory(path.AppendASCII("d")));
  EXPECT_EQ(Measure(dir.GetPath()).gap, Gap::kTooDeep);
}

TEST(HelperLimitsTest, ADirectoryThatIsntThereIsUnreadable) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  EXPECT_EQ(Measure(dir.GetPath().AppendASCII("missing")).gap,
            Gap::kUnreadable);
  // Nor a file.
  const base::FilePath file = dir.GetPath().AppendASCII("file");
  ASSERT_TRUE(base::WriteFile(file, "x"));
  EXPECT_EQ(Measure(file).gap, Gap::kUnreadable);
}

#if BUILDFLAG(IS_POSIX)
TEST(HelperLimitsTest, NeverFollowsALink) {
  base::ScopedTempDir dir;
  base::ScopedTempDir elsewhere;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ASSERT_TRUE(elsewhere.CreateUniqueTempDir());
  const base::FilePath big = elsewhere.GetPath().AppendASCII("big");
  MakeFileOfLength(big, 1LL << 30);
  ASSERT_TRUE(base::CreateSymbolicLink(big, dir.GetPath().AppendASCII("file")));
  ASSERT_TRUE(base::CreateSymbolicLink(elsewhere.GetPath(),
                                       dir.GetPath().AppendASCII("dir")));
  const ScratchUsage usage = Measure(dir.GetPath());
  EXPECT_EQ(usage.gap, Gap::kNone);
  EXPECT_EQ(usage.entries, 2u);
  EXPECT_LT(usage.bytes, kMiB);
  // The directory itself must not be a link either.
  const base::FilePath link = elsewhere.GetPath().AppendASCII("link");
  ASSERT_TRUE(base::CreateSymbolicLink(dir.GetPath(), link));
  EXPECT_EQ(Measure(link).gap, Gap::kUnreadable);
}

TEST(HelperLimitsTest, WhatCantBeReadIsUnreadable) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "root reads any directory";
  }
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath sub = dir.GetPath().AppendASCII("sub");
  ASSERT_TRUE(base::CreateDirectory(sub));
  ASSERT_TRUE(base::SetPosixFilePermissions(sub, 0));
  EXPECT_EQ(Measure(dir.GetPath()).gap, Gap::kUnreadable);
  ASSERT_TRUE(base::SetPosixFilePermissions(sub, 0700));
  ASSERT_TRUE(base::SetPosixFilePermissions(dir.GetPath(), 0));
  EXPECT_EQ(Measure(dir.GetPath()).gap, Gap::kUnreadable);
  ASSERT_TRUE(base::SetPosixFilePermissions(dir.GetPath(), 0700));
}
#endif  // BUILDFLAG(IS_POSIX)

#if BUILDFLAG(IS_MAC)
TEST(HelperLimitsTest, CountsAResourceFork) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath file = dir.GetPath().AppendASCII("out.txt");
  ASSERT_TRUE(base::WriteFile(file, "text"));
  ASSERT_TRUE(base::WriteFile(file.AppendASCII("..namedfork/rsrc"),
                              std::string(4 * kMiB, 'x')));
  const ScratchUsage usage = Measure(dir.GetPath());
  EXPECT_EQ(usage.gap, Gap::kNone);
  EXPECT_EQ(usage.entries, 1u);
  EXPECT_GE(usage.bytes, 4 * kMiB);
}
#endif  // BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_WIN)
TEST(HelperLimitsTest, CountsEveryStream) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath file = dir.GetPath().AppendASCII("out.txt");
  ASSERT_TRUE(base::WriteFile(file, "text"));
  ASSERT_TRUE(base::WriteFile(base::FilePath(file.value() + L":hidden"),
                              std::string(4 * kMiB, 'x')));
  ScratchUsage usage = Measure(dir.GetPath());
  EXPECT_EQ(usage.gap, Gap::kNone);
  EXPECT_EQ(usage.entries, 1u);
  EXPECT_GE(usage.bytes, 4 * kMiB);
  // A stream on the directory itself.
  ASSERT_TRUE(base::WriteFile(base::FilePath(dir.GetPath().value() + L":dir"),
                              std::string(8 * kMiB, 'x')));
  usage = Measure(dir.GetPath());
  EXPECT_EQ(usage.entries, 1u);
  EXPECT_GE(usage.bytes, 12 * kMiB);
}

TEST(HelperLimitsTest, MeasuresAFileStillBeingWritten) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  base::File file(dir.GetPath().AppendASCII("growing"),
                  base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  ASSERT_TRUE(file.IsValid());
  const std::string block(kMiB, 'x');
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(file.WriteAtCurrentPosAndCheck(base::as_byte_span(block)));
  }
  // Still open: the directory's own record of its size may lag.
  EXPECT_GE(Measure(dir.GetPath()).bytes, 4 * kMiB);
}

TEST(HelperLimitsTest, PathsLongerThanMaxPath) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath sub = dir.GetPath().AppendASCII(std::string(150, 'n'));
  ASSERT_TRUE(base::CreateDirectory(sub));
  // Its path is longer than MAX_PATH.
  const base::FilePath file = sub.AppendASCII(std::string(150, 'm'));
  ASSERT_GT(file.value().size(), static_cast<size_t>(MAX_PATH));
  ASSERT_TRUE(base::WriteFile(base::FilePath(L"\\\\?\\" + file.value()),
                              std::string(kMiB, 'x')));
  const ScratchUsage usage = Measure(dir.GetPath());
  EXPECT_EQ(usage.gap, Gap::kNone);
  EXPECT_EQ(usage.entries, 2u);
  EXPECT_GE(usage.bytes, kMiB);
}
#endif  // BUILDFLAG(IS_WIN)

TEST(HelperLimitsWatchTest, ChecksWhenDue) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  // The browser's input counts from the start.
  MakeFileOfLength(dir.GetPath().AppendASCII("input.pnm"), 100 * kMiB);
  HelperLimitsWatch watch(dir.GetPath());
  ASSERT_TRUE(watch.Start());
  const base::TimeTicks now = base::TimeTicks::Now();
  EXPECT_TRUE(watch.Check(now));
  EXPECT_GT(watch.next_check(), now);

  MakeFileOfLength(dir.GetPath().AppendASCII("out.txt"),
                   static_cast<int64_t>(kMaxHelperWriteBytes));
  // Not due yet: nothing is measured.
  EXPECT_TRUE(watch.Check(now));
  EXPECT_TRUE(watch.error().empty());
  EXPECT_FALSE(watch.Check(watch.next_check()));
  EXPECT_EQ(watch.error(), "it wrote 64 MiB or more to its scratch directory");
  // It stays past the limit.
  EXPECT_FALSE(watch.Check(now, /*force=*/true));
}

TEST(HelperLimitsWatchTest, AlwaysWhenForced) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  HelperLimitsWatch watch(dir.GetPath());
  ASSERT_TRUE(watch.Start());
  EXPECT_TRUE(watch.Check(base::TimeTicks::Now()));
  for (int i = 0; i <= static_cast<int>(kMaxScratchNewEntries); ++i) {
    ASSERT_TRUE(base::WriteFile(
        dir.GetPath().AppendASCII("file" + std::to_string(i)), ""));
  }
  EXPECT_FALSE(watch.Check(base::TimeTicks::Now(), /*force=*/true));
  EXPECT_EQ(watch.error(),
            "it made more than 64 files in its scratch directory");
}

TEST(HelperLimitsWatchTest, AScratchDirectoryThatCantBeMeasured) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  HelperLimitsWatch watch(dir.GetPath().AppendASCII("missing"));
  EXPECT_FALSE(watch.Start());
  EXPECT_EQ(watch.error(), "its scratch directory couldn't be measured");
}

}  // namespace
}  // namespace helper_limits
}  // namespace molt_ai
