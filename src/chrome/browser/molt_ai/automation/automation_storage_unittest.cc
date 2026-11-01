// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationStorage's own directory (a profile's voice routines) and the
// one-time move out of the shared storage (Take()); since round 44, files
// that aren't regular or are too big skipped and never written over, the
// audit log created and read from its end, Update() as one
// read-modify-write whichever thread, and moves cut short reported.

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace molt_ai {
namespace automation {
namespace {

Script Named(const std::string& id, const std::string& name) {
  Script script;
  script.id = id;
  script.name = name;
  Step step;
  step.type = StepType::NAVIGATE;
  step.target = "https://news.example/";
  script.steps.push_back(std::move(step));
  return script;
}

TEST(AutomationStorageTest, ADirectoryOfItsOwn) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath shared_root = temp.GetPath().AppendASCII("home");
  const base::FilePath own = temp.GetPath().AppendASCII("Profile 1");
  AutomationStorage shared;
  shared.SetRootForTesting(shared_root);
  AutomationStorage mine(own);
  ASSERT_TRUE(mine.Save(Named("voice_1", "my news")));
  // In the directory itself, not under "automations".
  EXPECT_TRUE(base::PathExists(own.AppendASCII("voice_1.molt")));
  EXPECT_TRUE(base::PathExists(own.AppendASCII("audit.log")));
  EXPECT_EQ(mine.ListIds(), std::vector<std::string>{"voice_1"});
  // The shared storage doesn't see it, nor it the shared one's.
  EXPECT_TRUE(shared.ListIds().empty());
  ASSERT_TRUE(shared.Save(Named("wf_1", "Prices")));
  EXPECT_EQ(mine.ListIds(), std::vector<std::string>{"voice_1"});
  EXPECT_FALSE(mine.Load("wf_1"));
  EXPECT_FALSE(shared.Load("voice_1"));
}

TEST(AutomationStorageTest, TakenOnce) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  AutomationStorage shared;
  shared.SetRootForTesting(temp.GetPath());
  ASSERT_TRUE(shared.Save(Named("voice_7", "my news")));

  std::optional<Script> taken = shared.Take("voice_7");
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->id, "voice_7");
  EXPECT_EQ(taken->name, "my news");
  // Gone from the listing at once; a second taker gets nothing.
  EXPECT_TRUE(shared.ListIds().empty());
  EXPECT_FALSE(shared.Load("voice_7"));
  EXPECT_FALSE(shared.Take("voice_7"));
  // Put back, it is there again, and can be taken again.
  ASSERT_TRUE(shared.ReturnTaken("voice_7"));
  EXPECT_EQ(shared.ListIds(), std::vector<std::string>{"voice_7"});
  ASSERT_TRUE(shared.Take("voice_7"));
  shared.FinishTake("voice_7");
  EXPECT_TRUE(shared.ListIds().empty());
  EXPECT_FALSE(shared.ReturnTaken("voice_7"));
  EXPECT_FALSE(shared.Take("voice_7"));
}

TEST(AutomationStorageTest, TakeRefusesWhatItCantRead) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  AutomationStorage shared;
  shared.SetRootForTesting(temp.GetPath());
  ASSERT_TRUE(shared.EnsureDirectory());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  ASSERT_TRUE(base::WriteFile(dir.AppendASCII("broken.molt"), "{not json"));
  EXPECT_FALSE(shared.Take("broken"));
  // Left as it was.
  EXPECT_TRUE(base::PathExists(dir.AppendASCII("broken.molt")));
  EXPECT_FALSE(base::PathExists(dir.AppendASCII("broken.molt.taken")));
  // Nor ids that aren't file names.
  EXPECT_FALSE(shared.Take("../broken"));
  EXPECT_FALSE(shared.Take(""));
  // A file whose id says another is known by its file's name.
  Script other = Named("someone_else", "theirs");
  ASSERT_TRUE(
      base::WriteFile(dir.AppendASCII("mine.molt"), other.ToJSONString()));
  std::optional<Script> taken = shared.Take("mine");
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->id, "mine");
}

// The scripts' files that aren't a regular file of at most
// kMaxScriptFileBytes: listed, loaded and taken never (and never opened, so
// a FIFO can't block), and never written over.
TEST(AutomationStorageTest, OnlyRegularFilesWithinTheCapAreScripts) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath dir = temp.GetPath().AppendASCII("Profile 1");
  AutomationStorage storage(dir);
  ASSERT_TRUE(storage.Save(Named("ok", "Prices")));

  // Too big: its id stays in use, but it isn't read.
  const base::FilePath huge = dir.AppendASCII("huge.molt");
  ASSERT_TRUE(base::WriteFile(huge, "{\"id\": \"huge\", \"name\": \"" +
                                        std::string(kMaxScriptFileBytes, 'x') +
                                        "\"}"));
  EXPECT_FALSE(storage.Load("huge"));
  EXPECT_FALSE(storage.Take("huge"));
  EXPECT_TRUE(base::PathExists(huge));
  ASSERT_EQ(storage.ListAll().size(), 1u);
  EXPECT_EQ(storage.ListAll()[0].id, "ok");
  // Nor written: a script whose file would be over the cap isn't saved.
  EXPECT_FALSE(
      storage.Save(Named("big", std::string(kMaxScriptFileBytes, 'y'))));
  EXPECT_FALSE(base::PathExists(dir.AppendASCII("big.molt")));

#if BUILDFLAG(IS_POSIX)
  // A FIFO with no writer (its open() would wait), a link and a directory
  // named as scripts.
  const base::FilePath fifo = dir.AppendASCII("fifo.molt");
  ASSERT_EQ(mkfifo(fifo.value().c_str(), 0600), 0);
  const base::FilePath target = temp.GetPath().AppendASCII("target.json");
  ASSERT_TRUE(base::WriteFile(target, Named("link", "Linked").ToJSONString()));
  const base::FilePath link = dir.AppendASCII("link.molt");
  ASSERT_TRUE(base::CreateSymbolicLink(target, link));
  ASSERT_TRUE(base::CreateDirectory(dir.AppendASCII("dir.molt")));

  std::vector<std::string> ids = storage.ListIds();
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<std::string>{"huge", "ok"}));
  for (const char* id : {"fifo", "link", "dir"}) {
    EXPECT_FALSE(storage.Load(id)) << id;
    EXPECT_FALSE(storage.Take(id)) << id;
    EXPECT_FALSE(storage.Save(Named(id, "Mine"))) << id;
  }
  struct stat st;
  ASSERT_EQ(lstat(fifo.value().c_str(), &st), 0);
  EXPECT_TRUE(S_ISFIFO(st.st_mode));
  ASSERT_EQ(lstat(link.value().c_str(), &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(target, &contents));
  EXPECT_EQ(contents, Named("link", "Linked").ToJSONString());
  EXPECT_TRUE(base::DirectoryExists(dir.AppendASCII("dir.molt")));
  EXPECT_EQ(storage.ListAll().size(), 1u);
#endif
}

// audit.log is created on the first line (base::AppendToFile() alone
// doesn't), and read from its end.
TEST(AutomationStorageTest, TheAuditLogIsCreatedAndReadFromItsEnd) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath dir = temp.GetPath().AppendASCII("Profile 1");
  AutomationStorage storage(dir);
  ASSERT_TRUE(storage.Save(Named("voice_1", "my news")));
  storage.AppendAudit("voice_1", "voice_routine_saved", "");
  std::vector<std::string> lines = storage.ReadAuditTail(10);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_NE(lines[0].find("| voice_1 | saved"), std::string::npos);
  EXPECT_NE(lines[1].find("| voice_1 | voice_routine_saved"),
            std::string::npos);
  EXPECT_EQ(storage.ReadAuditTail(1).size(), 1u);

#if BUILDFLAG(IS_POSIX)
  // A FIFO there: nothing waits, nothing is read, scripts still save.
  const base::FilePath log = dir.AppendASCII("audit.log");
  ASSERT_TRUE(base::DeleteFile(log));
  ASSERT_EQ(mkfifo(log.value().c_str(), 0600), 0);
  storage.AppendAudit("voice_1", "anything", "");
  EXPECT_TRUE(storage.ReadAuditTail(10).empty());
  EXPECT_TRUE(storage.Save(Named("voice_2", "mail")));
#endif
}

// Two threads each make 40 read-modify-writes of the same script, through
// two storages of the same directory (the Agent studio's, made with the
// directory, and the runner's, the default one): none is lost.
TEST(AutomationStorageTest, UpdatesNeverInterleave) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath root = temp.GetPath();
  {
    AutomationStorage shared;
    shared.SetRootForTesting(root);
    ASSERT_TRUE(shared.Save(Named("wf_1", "Prices")));
  }
  constexpr int kUpdates = 40;
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath shared_root) {
            AutomationStorage studio(
                AutomationStorage::SharedDirectory(shared_root));
            for (int i = 0; i < kUpdates; ++i) {
              EXPECT_TRUE(studio.Update("wf_1", [](Script& script) {
                script.default_variables["n"] += "s";
                return true;
              }));
            }
          },
          root));
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath shared_root) {
            AutomationStorage runner;
            runner.SetRootForTesting(shared_root);
            for (int i = 0; i < kUpdates; ++i) {
              EXPECT_TRUE(runner.Update("wf_1", [](Script& script) {
                ++script.stats.runs;
                return true;
              }));
            }
          },
          root));
  task_environment.RunUntilIdle();
  AutomationStorage shared;
  shared.SetRootForTesting(root);
  std::optional<Script> script = shared.Load("wf_1");
  ASSERT_TRUE(script);
  EXPECT_EQ(script->stats.runs, kUpdates);
  EXPECT_EQ(script->default_variables["n"], std::string(kUpdates, 's'));
  // An update that declines, or of nothing, writes nothing.
  EXPECT_FALSE(shared.Update("wf_1", [](Script&) { return false; }));
  EXPECT_FALSE(shared.Update("gone", [](Script&) { return true; }));
  EXPECT_FALSE(shared.Load("gone"));
}

// A move cut short (the browser ended between Take() and its end):
// reported once no browser may still be making it, never while this one
// is.
TEST(AutomationStorageTest, MovesCutShortAreReported) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  AutomationStorage shared;
  shared.SetRootForTesting(temp.GetPath());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  for (const char* id : {"voice_1", "voice_2", "voice_3", "voice_4"}) {
    ASSERT_TRUE(shared.Save(Named(id, id)));
  }
  // Set aside by a browser that ended: long ago, just now (maybe another
  // browser's, moving it now), and "in the future" (a clock set back).
  const base::Time now = base::Time::Now();
  const struct {
    const char* id;
    base::Time when;
  } set_aside[] = {{"voice_1", now - base::Minutes(10)},
                   {"voice_2", now},
                   {"voice_3", now + base::Minutes(60)}};
  for (const auto& entry : set_aside) {
    const base::FilePath taken =
        dir.AppendASCII(std::string(entry.id) + ".molt.taken");
    ASSERT_TRUE(base::ReplaceFile(
        dir.AppendASCII(std::string(entry.id) + ".molt"), taken, nullptr));
    ASSERT_TRUE(base::TouchFile(taken, entry.when, entry.when));
  }
  // Being moved in this browser: never reported, however old.
  ASSERT_TRUE(shared.Take("voice_4"));
  const base::FilePath taken4 = dir.AppendASCII("voice_4.molt.taken");
  ASSERT_TRUE(base::TouchFile(taken4, now - base::Minutes(10),
                              now - base::Minutes(10)));

  std::vector<std::string> interrupted = shared.InterruptedTakes();
  std::sort(interrupted.begin(), interrupted.end());
  EXPECT_EQ(interrupted, (std::vector<std::string>{"voice_1", "voice_3"}));
  std::optional<Script> read = shared.ReadTaken("voice_1");
  ASSERT_TRUE(read);
  EXPECT_EQ(read->id, "voice_1");
  // Not taken again while set aside.
  ASSERT_TRUE(shared.Save(Named("voice_1", "saved since")));
  EXPECT_FALSE(shared.Take("voice_1"));
  // Put back: never over the script saved under its id since, but beside
  // it.
  EXPECT_TRUE(shared.ReturnTaken("voice_1"));
  std::optional<Script> beside = shared.Load("voice_1_");
  ASSERT_TRUE(beside);
  EXPECT_EQ(beside->name, "voice_1");
  EXPECT_EQ(shared.Load("voice_1")->name, "saved since");
  shared.FinishTake("voice_4");
  EXPECT_FALSE(base::PathExists(taken4));
  EXPECT_EQ(shared.InterruptedTakes(), std::vector<std::string>{"voice_3"});
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
