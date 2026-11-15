// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Molt Workflow Record (workflow_record.h) on real files: kept in the
// user data directory only, seeded once and only on an upgrade (never on a
// new user data directory, nor after a loss or a corruption), never read
// through or written over when it isn't a regular file, nor kept in
// ~/.moltbrowser; malformed entries dropped one by one, entries of files
// gone dropped at load (not while set aside), moves copied first, the fire
// time taken from the files only at seeding, set by writes (never back
// below a past fire) and raised by fires (a fire recorded on a clock that
// was ahead giving way), an entry restored exactly, and a session's writes
// before configuration kept.

#include "chrome/browser/molt_ai/automation/workflow_record.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/common/molt_home_dir_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace {

using Origin = WorkflowRecord::Origin;

// A digest as the storage computes them: 64 lowercase hex digits.
std::string Digest(char c) {
  return std::string(64, c);
}

int64_t NowUnix() {
  return static_cast<int64_t>(base::Time::Now().ToTimeT());
}

class WorkflowRecordTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    user_data_dir_ = temp_.GetPath().AppendASCII("User Data");
    ASSERT_TRUE(base::CreateDirectory(user_data_dir_));
    record_file_ = user_data_dir_.Append(kWorkflowRecordFileName);
    directory_ = home_.path().AppendASCII("automations");
    WorkflowRecord::ResetForTesting();
  }
  void TearDown() override { WorkflowRecord::ResetForTesting(); }

  // A browser start: configured, then loaded as the storage loads it.
  WorkflowRecord* Start(bool may_seed,
                        const base::FilePath& directory = base::FilePath()) {
    WorkflowRecord::ResetForTesting();
    const base::FilePath dir = directory.empty() ? directory_ : directory;
    WorkflowRecord::Configure({dir, record_file_, may_seed});
    WorkflowRecord* record = WorkflowRecord::For(dir);
    EXPECT_TRUE(record);
    Load(record);
    return record;
  }
  void Load(WorkflowRecord* record) {
    record->LoadLocked(
        [this] { return present_; },
        [this](
            const std::string& name) -> std::optional<WorkflowRecord::Entry> {
          auto it = contents_.find(name);
          if (it == contents_.end()) {
            return std::nullopt;
          }
          return it->second;
        },
        [this](const std::string& event, const std::string& extra) {
          audit_.push_back(event + " | " + extra);
        });
  }
  // A file in the directory, holding |digest|.
  void Have(const std::string& name,
            const std::string& digest,
            int64_t fired = 0) {
    present_.insert(name);
    contents_[name] = {digest, fired};
  }
  // The names the record file lists.
  std::set<std::string> RecordedNames() {
    std::set<std::string> names;
    std::string contents;
    if (!base::ReadFileToString(record_file_, &contents)) {
      return names;
    }
    std::optional<base::Value> value =
        base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
    if (!value || !value->is_dict()) {
      return names;
    }
    if (const base::DictValue* files = value->GetDict().FindDict("files")) {
      for (const auto item : *files) {
        names.insert(item.first);
      }
    }
    return names;
  }
  bool Exists(const base::FilePath& path) { return base::PathExists(path); }

  ScopedMoltHomeDirForTesting home_;
  base::ScopedTempDir temp_;
  base::FilePath user_data_dir_;
  base::FilePath record_file_;
  base::FilePath directory_;
  std::set<std::string> present_;
  std::map<std::string, WorkflowRecord::Entry> contents_;
  std::vector<std::string> audit_;
};

TEST_F(WorkflowRecordTest,
       TheRecordAndItsMarkerAreCreatedInTheUserDataDirOnly) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  EXPECT_TRUE(record->loaded());
  EXPECT_TRUE(Exists(record_file_));
  EXPECT_TRUE(Exists(WorkflowRecord::MarkerFileFor(record_file_)));
  // Nothing new in MoltBrowser's data folder.
  base::FileEnumerator in_home(
      home_.path(), /*recursive=*/true,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
  EXPECT_TRUE(in_home.Next().empty());
#if BUILDFLAG(IS_POSIX)
  // Readable by its owner only.
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(record_file_, &mode));
  EXPECT_EQ(mode & 077, 0);
  ASSERT_TRUE(base::GetPosixFilePermissions(
      WorkflowRecord::MarkerFileFor(record_file_), &mode));
  EXPECT_EQ(mode & 077, 0);
#endif
}

TEST_F(WorkflowRecordTest, AnUpgradeSeedsWhatIsThereOnce) {
  Have("a.molt", Digest('a'), 1800000000);
  Have("b.molt.0123.taken", Digest('b'));
  WorkflowRecord* record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kSeeded);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  EXPECT_TRUE(record->Vouches("b.molt.0123.taken", Digest('b')));
  EXPECT_FALSE(record->Vouches("a.molt", Digest('b')));
  EXPECT_EQ(record->LastFired("a.molt"), 1800000000);
  EXPECT_EQ(audit_, std::vector<std::string>{"record_seeded | 2 workflows"});
  EXPECT_TRUE(Exists(WorkflowRecord::MarkerFileFor(record_file_)));
  EXPECT_EQ(RecordedNames(),
            (std::set<std::string>{"a.molt", "b.molt.0123.taken"}));

  // The next start: a file added since is not seeded, nor a last fire
  // time a file says now.
  Have("a.molt", Digest('a'), 1);
  Have("c.molt", Digest('c'));
  audit_.clear();
  record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kSeeded);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  EXPECT_EQ(record->LastFired("a.molt"), 1800000000);
  EXPECT_FALSE(record->Vouches("c.molt", Digest('c')));
  EXPECT_TRUE(audit_.empty());
}

// The flaw a record written only on change would have: a first run that
// writes nothing must still leave the record, so a later start never takes
// files planted in between for an upgrade's.
TEST_F(WorkflowRecordTest, AFreshUserDataDirSeedsNothingEvenOnItsNextStart) {
  Have("a.molt", Digest('a'));
  WorkflowRecord* record = Start(/*may_seed=*/false);
  EXPECT_EQ(record->origin(), Origin::kFresh);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  EXPECT_TRUE(Exists(record_file_));
  EXPECT_TRUE(Exists(WorkflowRecord::MarkerFileFor(record_file_)));
  Have("planted.molt", Digest('p'));
  record = Start(/*may_seed=*/true);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  EXPECT_FALSE(record->Vouches("planted.molt", Digest('p')));
  EXPECT_TRUE(RecordedNames().empty());
}

TEST_F(WorkflowRecordTest, ALostRecordIsNeverSeededAgain) {
  Have("a.molt", Digest('a'));
  Start(/*may_seed=*/true);
  ASSERT_TRUE(base::DeleteFile(record_file_));
  audit_.clear();
  WorkflowRecord* record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kAfterLoss);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  EXPECT_EQ(audit_, std::vector<std::string>{"record_reset | lost"});
  // Written again, empty.
  EXPECT_TRUE(Exists(record_file_));
  EXPECT_TRUE(RecordedNames().empty());
}

TEST_F(WorkflowRecordTest, ACorruptRecordIsKeptAsideAndNeverSeeds) {
  Have("a.molt", Digest('a'));
  ASSERT_TRUE(base::WriteFile(record_file_, "{\"format\": 1, garbage"));
  WorkflowRecord* record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kAfterCorrupt);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  EXPECT_EQ(audit_, std::vector<std::string>{"record_reset | corrupt"});
  std::string bad;
  ASSERT_TRUE(base::ReadFileToString(
      base::FilePath(record_file_.value() + FILE_PATH_LITERAL(".bad")), &bad));
  EXPECT_EQ(bad, "{\"format\": 1, garbage");
  // A valid, empty record in its place, and the marker repaired.
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(record_file_, &contents));
  std::optional<base::Value> value =
      base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  ASSERT_TRUE(value && value->is_dict());
  EXPECT_EQ(value->GetDict().FindInt("format"), 1);
  EXPECT_TRUE(RecordedNames().empty());
  EXPECT_TRUE(Exists(WorkflowRecord::MarkerFileFor(record_file_)));
  // Nor does the next start seed.
  record = Start(/*may_seed=*/true);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
}

#if BUILDFLAG(IS_POSIX)
TEST_F(WorkflowRecordTest, ARecordThatIsNotARegularFileIsLeftAlone) {
  Have("a.molt", Digest('a'));
  // A link to a file elsewhere: never read through, never written over.
  const base::FilePath target = temp_.GetPath().AppendASCII("elsewhere");
  ASSERT_TRUE(base::WriteFile(target, "not mine"));
  ASSERT_TRUE(base::CreateSymbolicLink(target, record_file_));
  WorkflowRecord* record = Start(/*may_seed=*/true);
  EXPECT_TRUE(record->loaded());
  EXPECT_EQ(record->origin(), Origin::kSessionOnly);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  // This session's writes only, in memory.
  record->Set("b.molt", Digest('b'), 0);
  EXPECT_TRUE(record->Vouches("b.molt", Digest('b')));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(target, &contents));
  EXPECT_EQ(contents, "not mine");
  EXPECT_FALSE(Exists(WorkflowRecord::MarkerFileFor(record_file_)));
  // A directory: the same.
  ASSERT_TRUE(base::DeleteFile(record_file_));
  ASSERT_TRUE(base::CreateDirectory(record_file_));
  record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kSessionOnly);
  EXPECT_TRUE(base::DirectoryExists(record_file_));
  // Tried again at the next start: once it is gone, an upgrade seeds.
  ASSERT_TRUE(base::DeleteFile(record_file_));
  record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kSeeded);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
}
#endif  // BUILDFLAG(IS_POSIX)

TEST_F(WorkflowRecordTest, ARecordUnderMoltHomeIsNeverUsed) {
  record_file_ = home_.path().Append(kWorkflowRecordFileName);
  Have("a.molt", Digest('a'));
  WorkflowRecord* record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kSessionOnly);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  record->Set("b.molt", Digest('b'), 0);
  EXPECT_TRUE(record->Vouches("b.molt", Digest('b')));
  EXPECT_FALSE(Exists(record_file_));
  EXPECT_FALSE(Exists(WorkflowRecord::MarkerFileFor(record_file_)));
  // Nor inside the workflows' directory itself.
  ASSERT_TRUE(base::CreateDirectory(directory_));
  record_file_ = directory_.Append(kWorkflowRecordFileName);
  record = Start(/*may_seed=*/true);
  EXPECT_EQ(record->origin(), Origin::kSessionOnly);
  EXPECT_FALSE(Exists(record_file_));
}

TEST_F(WorkflowRecordTest, ARecordOfAnotherDirectoryVouchesNothing) {
  Have("a.molt", Digest('a'));
  Start(/*may_seed=*/true);
  audit_.clear();
  WorkflowRecord* record =
      Start(/*may_seed=*/true, home_.path().AppendASCII("elsewhere"));
  EXPECT_EQ(record->origin(), Origin::kOtherDirectory);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  EXPECT_EQ(audit_,
            std::vector<std::string>{"record_reset | another directory"});
  EXPECT_TRUE(RecordedNames().empty());
}

TEST_F(WorkflowRecordTest, MalformedEntriesAreDroppedOneByOne) {
  const std::string good = Digest('a');
  const std::string long_name = std::string(196, 'x') + ".molt";  // 201
  ASSERT_EQ(long_name.size(), 201u);
  for (const std::string& name :
       {std::string("a.molt"), std::string("b.molt.0123.taken"),
        std::string("x/y.molt"), std::string(".hidden.molt"), long_name,
        std::string("short.molt"), std::string("upper.molt"),
        std::string("fired.molt"), std::string("negative.molt"),
        std::string("notes.txt")}) {
    present_.insert(name);
  }
  auto entry = [](const std::string& digest) {
    return base::DictValue().Set("digest", digest);
  };
  base::DictValue files;
  files.Set("a.molt", entry(good).Set("fired", 1800000000.0));
  files.Set("b.molt.0123.taken", entry(good));
  files.Set("x/y.molt", entry(good));
  files.Set(".hidden.molt", entry(good));
  files.Set("", entry(good));
  files.Set(long_name, entry(good));
  files.Set("short.molt", entry(std::string(63, 'a')));
  files.Set("upper.molt", entry(std::string(64, 'A')));
  files.Set("fired.molt", entry(good).Set("fired", "x"));
  files.Set("negative.molt", entry(good).Set("fired", -1.0));
  files.Set("notes.txt", entry(good));
  base::DictValue root;
  root.Set("format", 1);
  root.Set("directory", directory_.AsUTF8Unsafe());
  root.Set("origin", "seeded");
  root.Set("files", std::move(files));
  std::string record;
  ASSERT_TRUE(base::JSONWriter::Write(root, &record));
  ASSERT_TRUE(base::WriteFile(record_file_, record));
  ASSERT_TRUE(
      base::WriteFile(WorkflowRecord::MarkerFileFor(record_file_), "1\n"));
  WorkflowRecord* loaded = Start(/*may_seed=*/false);
  EXPECT_EQ(loaded->origin(), Origin::kSeeded);
  EXPECT_TRUE(loaded->Vouches("a.molt", good));
  EXPECT_EQ(loaded->LastFired("a.molt"), 1800000000);
  EXPECT_TRUE(loaded->Vouches("b.molt.0123.taken", good));
  EXPECT_EQ(loaded->LastFired("b.molt.0123.taken"), 0);
  for (const std::string& dropped :
       {std::string("x/y.molt"), std::string(".hidden.molt"), std::string(),
        long_name, std::string("short.molt"), std::string("upper.molt"),
        std::string("fired.molt"), std::string("negative.molt"),
        std::string("notes.txt")}) {
    EXPECT_FALSE(loaded->Vouches(dropped, good)) << dropped;
  }
  EXPECT_FALSE(loaded->Vouches("short.molt", std::string(63, 'a')));
  EXPECT_FALSE(loaded->Vouches("upper.molt", std::string(64, 'A')));
  EXPECT_EQ(RecordedNames(),
            (std::set<std::string>{"a.molt", "b.molt.0123.taken"}));
}

TEST_F(WorkflowRecordTest, EntriesForFilesNoLongerThereAreDroppedAtLoad) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  Have("a.molt", Digest('a'));
  Have("b.molt", Digest('b'));
  record->Set("a.molt", Digest('a'), 0);
  record->Set("b.molt", Digest('b'), 0);
  EXPECT_EQ(RecordedNames(), (std::set<std::string>{"a.molt", "b.molt"}));
  // b.molt deleted outside this browser.
  present_.erase("b.molt");
  record = Start(/*may_seed=*/false);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  EXPECT_FALSE(record->Vouches("b.molt", Digest('b')));
  EXPECT_EQ(RecordedNames(), std::set<std::string>{"a.molt"});
}

// Another browser's move in progress at this start (or one cut short):
// the script is there under its set-aside name only, and keeps its entry
// for when it is put back; once it is gone for good, the entry goes.
TEST_F(WorkflowRecordTest, AnEntryIsKeptWhileItsFileIsSetAside) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  Have("a.molt", Digest('a'));
  Have("ab.molt", Digest('b'));
  record->Set("a.molt", Digest('a'), 0);
  record->Set("ab.molt", Digest('b'), 0);
  present_ = {"a.molt.00ff.taken"};
  record = Start(/*may_seed=*/false);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  // Not another script's set-aside file.
  EXPECT_FALSE(record->Vouches("ab.molt", Digest('b')));
  present_ = {"a.molt"};
  record = Start(/*may_seed=*/false);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  present_.clear();
  record = Start(/*may_seed=*/false);
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
}

TEST_F(WorkflowRecordTest, CopyThenForgetCarriesAnEntryAcrossARestart) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  record->Set("a.molt", Digest('a'), 7);
  record->Set("b.molt", Digest('b'), 0);
  // A move: the entry copied before the rename, the old one forgotten
  // after.
  EXPECT_TRUE(record->Copy("a.molt", "a.molt.00ff.taken"));
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  EXPECT_TRUE(record->Vouches("a.molt.00ff.taken", Digest('a')));
  EXPECT_EQ(record->LastFired("a.molt.00ff.taken"), 7);
  record->Forget("a.molt");
  EXPECT_FALSE(record->Vouches("a.molt", Digest('a')));
  EXPECT_TRUE(record->Vouches("a.molt.00ff.taken", Digest('a')));
  // Nothing to copy: the target's entry stays.
  EXPECT_FALSE(record->Copy("none.molt.00ff.taken", "b.molt"));
  EXPECT_TRUE(record->Vouches("b.molt", Digest('b')));
  record->Set("c.molt", Digest('c'), 0);
  record->Forget("c.molt");
  EXPECT_FALSE(record->Vouches("c.molt", Digest('c')));
  // As the record file says at the next start.
  present_ = {"a.molt.00ff.taken", "b.molt", "c.molt"};
  record = Start(/*may_seed=*/false);
  EXPECT_TRUE(record->Vouches("a.molt.00ff.taken", Digest('a')));
  EXPECT_TRUE(record->Vouches("b.molt", Digest('b')));
  EXPECT_FALSE(record->Vouches("c.molt", Digest('c')));
}

// A move cut short on either side of its rename: whichever name holds the
// file at the next start has its entry.
TEST_F(WorkflowRecordTest, AMoveCutShortKeepsTheEntryOfWhereTheFileIs) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  record->Set("a.molt", Digest('a'), 7);
  ASSERT_TRUE(record->Copy("a.molt", "a.molt.00ff.taken"));
  // Before the rename.
  present_ = {"a.molt"};
  record = Start(/*may_seed=*/false);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  EXPECT_EQ(RecordedNames(), std::set<std::string>{"a.molt"});
  // After it, before the old name was forgotten.
  ASSERT_TRUE(record->Copy("a.molt", "a.molt.00ff.taken"));
  present_ = {"a.molt.00ff.taken"};
  record = Start(/*may_seed=*/false);
  EXPECT_TRUE(record->Vouches("a.molt.00ff.taken", Digest('a')));
  EXPECT_EQ(record->LastFired("a.molt.00ff.taken"), 7);
}

// A fire raises the fire time; a write sets it to what it wrote when that
// is later, or 0 (a schedule saved afresh starts over).
TEST_F(WorkflowRecordTest, AFireRaisesTheFireTimeAndAWriteSetsIt) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  record->Set("a.molt", Digest('a'), 100);
  EXPECT_EQ(record->LastFired("a.molt"), 100);
  record->NoteFired("a.molt", 50);
  EXPECT_EQ(record->LastFired("a.molt"), 100);
  record->NoteFired("a.molt", 200);
  EXPECT_EQ(record->LastFired("a.molt"), 200);
  record->Set("a.molt", Digest('b'), 300);
  EXPECT_EQ(record->LastFired("a.molt"), 300);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('b')));
  record->Set("a.molt", Digest('b'), 0);
  EXPECT_EQ(record->LastFired("a.molt"), 0);
  record->NoteFired("a.molt", 200);
  EXPECT_EQ(record->LastFired("a.molt"), 200);
  // No entry, no fire time: a fire of a file it doesn't vouch for.
  record->NoteFired("none.molt", 300);
  EXPECT_EQ(record->LastFired("none.molt"), 0);
  EXPECT_FALSE(record->Vouches("none.molt", Digest('a')));
  // Kept across a restart.
  present_ = {"a.molt"};
  record = Start(/*may_seed=*/false);
  EXPECT_EQ(record->LastFired("a.molt"), 200);
  // An empty digest is never recorded, nor matched.
  record->Set("a.molt", std::string(), 0);
  EXPECT_FALSE(record->Vouches("a.molt", std::string()));
  EXPECT_FALSE(record->Vouches("a.molt", Digest('b')));
}

// An earlier fire time written than the one recorded comes from a page
// opened before that fire: the past fire is kept (else the next listing
// would run it again). One recorded in the future (a snooze, a clock that
// was ahead) gives way to it.
TEST_F(WorkflowRecordTest, AnEarlierFireTimeWrittenKeepsAPastFire) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  const int64_t now = NowUnix();
  record->Set("a.molt", Digest('a'), now - 86400);
  record->NoteFired("a.molt", now - 60);
  // The page's copy, from before that fire, saved with a change.
  record->Set("a.molt", Digest('b'), now - 86400);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('b')));
  EXPECT_EQ(record->LastFired("a.molt"), now - 60);
  // A snooze, then a write from before it.
  record->Set("a.molt", Digest('b'), now + 3600);
  EXPECT_EQ(record->LastFired("a.molt"), now + 3600);
  record->Set("a.molt", Digest('b'), now - 60);
  EXPECT_EQ(record->LastFired("a.molt"), now - 60);
  // As at the next start.
  present_ = {"a.molt"};
  record = Start(/*may_seed=*/false);
  EXPECT_EQ(record->LastFired("a.molt"), now - 60);
}

// A fire recorded while the clock was ahead is the last fire only until
// the next fire on the corrected clock (else no catch-up would run until
// the clock reached it); a fire within the slack of it leaves it.
TEST_F(WorkflowRecordTest, AFireRecordedWhileTheClockWasAheadGivesWay) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  const int64_t now = NowUnix();
  const int64_t ahead = now + 7 * 86400;
  record->Set("a.molt", Digest('a'), 0);
  record->NoteFired("a.molt", ahead);
  EXPECT_EQ(record->LastFired("a.molt"), ahead);
  record->NoteFired("a.molt", ahead - 60);
  EXPECT_EQ(record->LastFired("a.molt"), ahead);
  record->NoteFired("a.molt", now);
  EXPECT_EQ(record->LastFired("a.molt"), now);
  present_ = {"a.molt"};
  record = Start(/*may_seed=*/false);
  EXPECT_EQ(record->LastFired("a.molt"), now);
  // No entry: still nothing.
  record->NoteFired("none.molt", now);
  EXPECT_FALSE(record->EntryOf("none.molt"));
}

// A write whose file couldn't be written puts the entry back exactly as it
// was, in the record file too; none before, none after.
TEST_F(WorkflowRecordTest, RestorePutsAnEntryBackExactly) {
  WorkflowRecord* record = Start(/*may_seed=*/false);
  record->Set("a.molt", Digest('a'), 7);
  const std::optional<WorkflowRecord::Entry> before = record->EntryOf("a.molt");
  ASSERT_TRUE(before);
  EXPECT_EQ(before->digest, Digest('a'));
  EXPECT_EQ(before->fired, 7);
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(record_file_, &bytes));
  record->Set("a.molt", Digest('b'), 0);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('b')));
  record->Restore("a.molt", before);
  EXPECT_TRUE(record->Vouches("a.molt", Digest('a')));
  EXPECT_EQ(record->LastFired("a.molt"), 7);
  std::string restored;
  ASSERT_TRUE(base::ReadFileToString(record_file_, &restored));
  EXPECT_EQ(restored, bytes);
  EXPECT_FALSE(record->EntryOf("b.molt"));
  record->Set("b.molt", Digest('b'), 0);
  record->Restore("b.molt", std::nullopt);
  EXPECT_FALSE(record->EntryOf("b.molt"));
  EXPECT_FALSE(record->Vouches("b.molt", Digest('b')));
  EXPECT_EQ(RecordedNames(), std::set<std::string>{"a.molt"});
}

TEST_F(WorkflowRecordTest, WritesBeforeConfigurationAreKept) {
  // A storage of the directory before the browser configured its record
  // (a test fixture's saves): session only.
  EXPECT_EQ(WorkflowRecord::For(directory_), nullptr);
  WorkflowRecord* early = WorkflowRecord::SessionOnlyFor(directory_);
  Load(early);
  EXPECT_EQ(early->origin(), Origin::kSessionOnly);
  Have("a.molt", Digest('a'));
  early->Set("a.molt", Digest('a'), 0);
  EXPECT_FALSE(Exists(record_file_));
  // Configured: the same record, loaded again, its session's writes kept
  // (and now written down).
  WorkflowRecord::Configure({directory_, record_file_, /*may_seed=*/false});
  WorkflowRecord* configured = WorkflowRecord::For(directory_);
  EXPECT_EQ(configured, early);
  EXPECT_FALSE(configured->loaded());
  Load(configured);
  EXPECT_EQ(configured->origin(), Origin::kFresh);
  EXPECT_TRUE(configured->Vouches("a.molt", Digest('a')));
  EXPECT_EQ(RecordedNames(), std::set<std::string>{"a.molt"});
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
