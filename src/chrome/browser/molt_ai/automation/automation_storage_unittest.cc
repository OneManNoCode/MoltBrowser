// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationStorage's own directory (a profile's voice routines) and the
// one-time move out of the shared storage (Take()); since round 44, files
// that aren't regular or are too big skipped and never written over, the
// audit log created and read from its end, Update() as one
// read-modify-write whichever thread, and moves cut short reported; since
// round 45, a fresh id when every simple one is taken; and the workflow
// record (workflow_record.h): a file is trusted only as far as this browser
// wrote it, on every read and write path, with its last fire this
// browser's own (never the file's), moves cut short, a browser ending
// between a record change and its file change, the one-time seeding (a new
// user data directory recorded at startup) and two browsers over one
// directory.

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/function_ref.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/workflow_record.h"
#include "chrome/browser/molt_ai/automation/workflow_record_test_util.h"
#include "chrome/browser/molt_ai/common/molt_home_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
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

// The files a Take() set |id| aside as, in |dir|.
std::vector<base::FilePath> SetAsideFiles(const base::FilePath& dir,
                                          const std::string& id) {
  std::vector<base::FilePath> out;
  base::FileEnumerator files(dir, /*recursive=*/false,
                             base::FileEnumerator::FILES,
                             FILE_PATH_LITERAL("*.taken"));
  for (base::FilePath file = files.Next(); !file.empty(); file = files.Next()) {
    if (file.BaseName().AsUTF8Unsafe().rfind(id + ".molt.", 0) == 0) {
      out.push_back(file);
    }
  }
  return out;
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
  EXPECT_TRUE(SetAsideFiles(dir, "broken").empty());
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
  // Set aside by a browser that ended: long ago (under round 43's name and
  // this round's), just now (maybe another browser's, moving it now), and
  // "in the future" (a clock set back).
  const base::Time now = base::Time::Now();
  const struct {
    const char* id;
    const char* set_aside_as;
    base::Time when;
  } set_aside[] = {
      {"voice_1", "voice_1.molt.taken", now - base::Minutes(10)},
      {"voice_2", "voice_2.molt.0123456789abcdef.taken", now},
      {"voice_3", "voice_3.molt.42.taken", now + base::Minutes(60)}};
  for (const auto& entry : set_aside) {
    const base::FilePath taken = dir.AppendASCII(entry.set_aside_as);
    ASSERT_TRUE(base::ReplaceFile(
        dir.AppendASCII(std::string(entry.id) + ".molt"), taken, nullptr));
    ASSERT_TRUE(base::TouchFile(taken, entry.when, entry.when));
  }
  // Names that aren't a set-aside file's: never reported.
  for (const char* name : {"x.taken", "a.b.molt.taken", "c.molt.XYZ.taken",
                           "d.molt..taken", "e.json.taken"}) {
    ASSERT_TRUE(base::WriteFile(dir.AppendASCII(name), "{}"));
  }
  // Being moved in this browser: never reported, however old.
  ASSERT_TRUE(shared.Take("voice_4"));
  std::vector<base::FilePath> taken4 = SetAsideFiles(dir, "voice_4");
  ASSERT_EQ(taken4.size(), 1u);
  ASSERT_TRUE(base::TouchFile(taken4[0], now - base::Minutes(10),
                              now - base::Minutes(10)));

  std::vector<AutomationStorage::SetAside> interrupted =
      shared.InterruptedTakes();
  std::sort(interrupted.begin(), interrupted.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  ASSERT_EQ(interrupted.size(), 2u);
  EXPECT_EQ(interrupted[0].id, "voice_1");
  EXPECT_EQ(interrupted[0].file, dir.AppendASCII("voice_1.molt.taken"));
  EXPECT_EQ(interrupted[1].id, "voice_3");
  std::optional<Script> read = shared.ReadSetAside(interrupted[0]);
  ASSERT_TRUE(read);
  EXPECT_EQ(read->id, "voice_1");
  // Only a set-aside file of this storage, under its own id.
  EXPECT_FALSE(shared.ReadSetAside({"voice_3", interrupted[0].file}));
  EXPECT_FALSE(shared.ReadSetAside(
      {"voice_1", temp.GetPath().AppendASCII("voice_1.molt.taken")}));
  // Put back: never over the script saved under its id since, but beside
  // it.
  ASSERT_TRUE(shared.Save(Named("voice_1", "saved since")));
  EXPECT_TRUE(shared.ReturnSetAside(interrupted[0]));
  std::optional<Script> beside = shared.Load("voice_1_");
  ASSERT_TRUE(beside);
  EXPECT_EQ(beside->name, "voice_1");
  // Known by its file's name: two scripts, never two of one id.
  EXPECT_EQ(beside->id, "voice_1_");
  EXPECT_EQ(shared.Load("voice_1")->name, "saved since");
  shared.FinishTake("voice_4");
  EXPECT_FALSE(base::PathExists(taken4[0]));
  shared.FinishSetAside(interrupted[1]);
  EXPECT_TRUE(SetAsideFiles(dir, "voice_3").empty());
  EXPECT_TRUE(shared.InterruptedTakes().empty());
}

// A script of the same id saved and taken by another browser while one is
// set aside: set aside beside it under a name of its own, never over it.
TEST(AutomationStorageTest, TwoTakesOfOneIdNeverReplaceEachOther) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  AutomationStorage first(dir);
  AutomationStorage second(dir);
  ASSERT_TRUE(first.Save(Named("voice_5", "first")));
  std::optional<Script> one = first.Take("voice_5");
  ASSERT_TRUE(one);
  EXPECT_FALSE(first.Take("voice_5"));
  ASSERT_TRUE(second.Save(Named("voice_5", "second")));
  std::optional<Script> two = second.Take("voice_5");
  ASSERT_TRUE(two);
  EXPECT_EQ(one->name, "first");
  EXPECT_EQ(two->name, "second");
  EXPECT_EQ(SetAsideFiles(dir, "voice_5").size(), 2u);
  // Each finishes or returns its own.
  first.FinishTake("voice_5");
  ASSERT_EQ(SetAsideFiles(dir, "voice_5").size(), 1u);
  EXPECT_TRUE(second.ReturnTaken("voice_5"));
  EXPECT_TRUE(SetAsideFiles(dir, "voice_5").empty());
  EXPECT_EQ(first.Load("voice_5")->name, "second");
}

// Every simple name a set-aside script could go back under taken (id, id_,
// ... up to the longest id; by scripts or by anything else): back under a
// fresh id made from its own, never left set aside.
TEST(AutomationStorageTest, ReturnedUnderAFreshIdWhenEverySimpleOneIsTaken) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  AutomationStorage storage(dir);
  ASSERT_TRUE(storage.Save(Named("voice_9", "my news")));
  ASSERT_TRUE(storage.Take("voice_9"));
  ASSERT_EQ(SetAsideFiles(dir, "voice_9").size(), 1u);
  std::string simple = "voice_9";
  while (simple.size() <= 128) {
    if (simple.size() == 64) {
      ASSERT_TRUE(base::CreateDirectory(dir.AppendASCII(simple + ".molt")));
    } else {
      ASSERT_TRUE(base::WriteFile(dir.AppendASCII(simple + ".molt"), "{}"));
    }
    simple += '_';
  }

  ASSERT_TRUE(storage.ReturnTaken("voice_9"));
  EXPECT_TRUE(SetAsideFiles(dir, "voice_9").empty());
  EXPECT_TRUE(storage.InterruptedTakes().empty());
  std::string back;
  for (const std::string& id : storage.ListIds()) {
    std::optional<Script> script = storage.Load(id);
    if (script && script->name == "my news") {
      EXPECT_TRUE(back.empty()) << id;
      back = id;
    }
  }
  // "voice_9_" and 16 hex digits.
  ASSERT_EQ(back.size(), 7u + 1u + 16u) << back;
  EXPECT_EQ(back.rfind("voice_9_", 0), 0u) << back;
  EXPECT_EQ(back.find_first_not_of("0123456789abcdef", 8), std::string::npos)
      << back;
  EXPECT_EQ(storage.Load(back)->id, back);

  // A long id made of two-byte characters: cut to fit, never inside one.
  std::string long_id;
  for (int i = 0; i < 60; ++i) {
    long_id += "\xc3\xa9";
  }
  for (simple = long_id; simple.size() <= 128; simple += '_') {
    ASSERT_TRUE(storage.Save(Named(simple, "taken")));
  }
  ASSERT_TRUE(storage.Save(Named(long_id, "my notes")));
  ASSERT_TRUE(storage.Take(long_id));
  ASSERT_TRUE(storage.Save(Named(long_id, "saved since")));
  ASSERT_TRUE(storage.ReturnTaken(long_id));
  std::string fresh;
  for (const std::string& id : storage.ListIds()) {
    std::optional<Script> script = storage.Load(id);
    if (script && script->name == "my notes") {
      fresh = id;
    }
  }
  ASSERT_EQ(fresh.size(), 110u + 1u + 16u) << fresh;
  EXPECT_EQ(fresh.substr(0, 111), long_id.substr(0, 110) + "_");
  EXPECT_EQ(storage.Load(long_id)->name, "saved since");
}

// A new workflow (the AI chat's agent run, a plan, a recording): saved
// under its id or the first free one after it, never over another, however
// many savers pick the same id at once.
TEST(AutomationStorageTest, SaveNewNeverOverwrites) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  AutomationStorage storage(dir);
  std::optional<std::string> first = storage.SaveNew(Named("agent_1", "one"));
  ASSERT_TRUE(first);
  EXPECT_EQ(*first, "agent_1");
  std::optional<std::string> second = storage.SaveNew(Named("agent_1", "two"));
  ASSERT_TRUE(second);
  EXPECT_EQ(*second, "agent_1_");
  EXPECT_EQ(storage.Load("agent_1")->name, "one");
  EXPECT_EQ(storage.Load("agent_1_")->name, "two");

  // Two writers at once, from two threads.
  constexpr int kSaves = 20;
  for (int writer = 0; writer < 2; ++writer) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](base::FilePath dir, int writer) {
              AutomationStorage mine(dir);
              for (int i = 0; i < kSaves; ++i) {
                EXPECT_TRUE(
                    mine.SaveNew(Named("plan_7", std::to_string(writer) + "-" +
                                                     std::to_string(i))));
              }
            },
            dir, writer));
  }
  task_environment.RunUntilIdle();
  std::vector<std::string> names;
  for (const Script& script : storage.ListAll()) {
    if (script.id.rfind("plan_7", 0) == 0) {
      names.push_back(script.name);
    }
  }
  std::sort(names.begin(), names.end());
  ASSERT_EQ(names.size(), 2u * kSaves);
  EXPECT_EQ(std::unique(names.begin(), names.end()), names.end());
  // No simple id left free: saved under a fresh one made from it.
  ASSERT_TRUE(storage.SaveNew(Named(std::string(128, 'x'), "first")));
  std::optional<std::string> fresh =
      storage.SaveNew(Named(std::string(128, 'x'), "second"));
  ASSERT_TRUE(fresh);
  ASSERT_EQ(fresh->size(), 128u);
  EXPECT_EQ(fresh->substr(0, 112), std::string(111, 'x') + "_");
  EXPECT_EQ(storage.Load(*fresh)->name, "second");
  EXPECT_EQ(storage.Load(std::string(128, 'x'))->name, "first");
}

// A save that depends on what is stored (molt://ai-automation keeps the
// stored trust): the check sees it, under the lock, and may refuse.
TEST(AutomationStorageTest, SaveCheckedSeesWhatIsStored) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  AutomationStorage storage(temp.GetPath().AppendASCII("automations"));
  bool saw_nothing = false;
  EXPECT_TRUE(storage.SaveChecked(Named("wf_2", "new"),
                                  [&](const Script* stored, Script& script) {
                                    saw_nothing = stored == nullptr;
                                    return true;
                                  }));
  EXPECT_TRUE(saw_nothing);
  ASSERT_TRUE(storage.Update("wf_2", [](Script& script) {
    script.security.trust = TrustLevel::APPROVED;
    return true;
  }));
  Script edit = Named("wf_2", "edited");
  edit.security.trust = TrustLevel::ADMIN;
  EXPECT_TRUE(storage.SaveChecked(
      std::move(edit), [](const Script* stored, Script& script) {
        EXPECT_TRUE(stored);
        script.security.trust = stored->security.trust;
        return true;
      }));
  std::optional<Script> saved = storage.Load("wf_2");
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->name, "edited");
  EXPECT_EQ(saved->security.trust, TrustLevel::APPROVED);
  // Refused, or its id changed: nothing written.
  EXPECT_FALSE(storage.SaveChecked(
      Named("wf_2", "no"), [](const Script*, Script&) { return false; }));
  EXPECT_FALSE(storage.SaveChecked(Named("wf_2", "moved"),
                                   [](const Script*, Script& script) {
                                     script.id = "wf_3";
                                     return true;
                                   }));
  EXPECT_EQ(storage.Load("wf_2")->name, "edited");
  EXPECT_FALSE(storage.Load("wf_3"));
}

// A browser that ended mid-write left the writer's temporary file beside
// a script: a listing deletes it once it is a minute old, never one of a
// write in progress, nor anything else.
TEST(AutomationStorageTest, AListingDeletesWritesCutShort) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  AutomationStorage storage(dir);
  ASSERT_TRUE(storage.Save(Named("wf_1", "Prices")));
  const auto temporary = [&](const std::string& name, base::TimeDelta age) {
    base::FilePath path;
    base::File file = base::CreateAndOpenTemporaryFileInDir(
        dir, &path, 0, base::FilePath::FromUTF8Unsafe(name).value());
    EXPECT_TRUE(file.IsValid());
    file.Close();
    const base::Time when = base::Time::Now() - age;
    EXPECT_TRUE(base::TouchFile(path, when, when));
    return path;
  };
  const base::FilePath stale = temporary("wf_1.molt", base::Minutes(3));
  const base::FilePath fresh = temporary("wf_2.molt", base::Seconds(1));
  const base::FilePath not_a_script = temporary("notes.txt", base::Minutes(3));
  const std::vector<std::string> ids = storage.ListIds();
  EXPECT_EQ(ids, std::vector<std::string>{"wf_1"});
  EXPECT_FALSE(base::PathExists(stale));
  EXPECT_TRUE(base::PathExists(fresh));
  EXPECT_TRUE(base::PathExists(not_a_script));
  EXPECT_EQ(storage.Load("wf_1")->name, "Prices");
}

// Every change takes the directory's lock file, which other browsers on
// the same home directory take too; anything else at its path doesn't stop
// a change.
TEST(AutomationStorageTest, ChangesTakeTheLockFile) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  AutomationStorage storage(dir);
  ASSERT_TRUE(storage.Save(Named("wf_1", "Prices")));
  const base::FilePath lock = dir.AppendASCII(".storage.lock");
  EXPECT_TRUE(base::PathExists(lock));
  EXPECT_FALSE(base::DirectoryExists(lock));
  // Not a script, and never listed.
  EXPECT_EQ(storage.ListIds(), std::vector<std::string>{"wf_1"});
  ASSERT_TRUE(base::DeleteFile(lock));
  ASSERT_TRUE(base::CreateDirectory(lock));
  EXPECT_TRUE(storage.Update("wf_1", [](Script& script) {
    script.name = "Prices again";
    return true;
  }));
  EXPECT_EQ(storage.Load("wf_1")->name, "Prices again");
}

// The shared storage is in MoltBrowser's own directory, which a test makes
// its own (molt_home_dir.h).
TEST(AutomationStorageTest, TheSharedStorageIsInMoltHomeDir) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  SetMoltHomeDirForTesting(temp.GetPath());
  AutomationStorage shared;
  EXPECT_EQ(shared.directory(), temp.GetPath().AppendASCII("automations"));
  EXPECT_EQ(AutomationStorage::SharedDirectory(), shared.directory());
  ASSERT_TRUE(shared.Save(Named("wf_1", "Prices")));
  EXPECT_TRUE(base::PathExists(
      temp.GetPath().AppendASCII("automations").AppendASCII("wf_1.molt")));
  SetMoltHomeDirForTesting(base::FilePath());
  EXPECT_NE(AutomationStorage::SharedDirectory(),
            temp.GetPath().AppendASCII("automations"));
}

// ---------------------------------------------------------------------------
// The workflow record (workflow_record.h): a file is trusted only as far as
// this browser wrote it; anything else loads as an unconfirmed import.
// ---------------------------------------------------------------------------

constexpr int64_t kFired = 1'800'000'000;
// A last fire in the year 2100.
constexpr int64_t kFarFuture = 4'102'444'800;

int64_t NowUnix() {
  return static_cast<int64_t>(base::Time::Now().ToTimeT());
}

// A workflow that runs every hour.
Script Hourly(const std::string& id) {
  Script script = Named(id, "Prices");
  script.trigger.type = TriggerType::INTERVAL;
  script.trigger.expression = "3600";
  return script;
}

// What a planted file might claim: TRUSTED, scheduled, a site of its own,
// no approvals, a history.
Script Claiming(const std::string& id) {
  Script script = Named(id, "Prices");
  script.security.trust = TrustLevel::TRUSTED;
  script.security.domain_whitelist = {"evil.example"};
  script.security.require_approval_for = {"x"};
  script.trigger.type = TriggerType::CRON;
  script.trigger.expression = "0 9 * * *";
  script.stats.runs = 7;
  script.stats.successes = 7;
  return script;
}

// Loaded as an unconfirmed import (AutomationSecurity::ClampUnvouched()).
bool IsClamped(const Script& script) {
  return script.security.changed_outside && script.security.sites_unconfirmed &&
         script.security.trust == TrustLevel::CASUAL &&
         script.security.domain_whitelist.empty() && script.stats.runs == 0;
}

class AutomationStorageRecordTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    dir_ = temp_.GetPath().AppendASCII("automations");
    user_data_dir_ = temp_.GetPath().AppendASCII("User Data");
    ASSERT_TRUE(base::CreateDirectory(user_data_dir_));
    Restart(/*may_seed=*/false);
    storage_.emplace(dir_);
  }

  // A browser start with |user_data_dir| (this test's by default): its
  // record is loaded at the storage's next use.
  void Restart(bool may_seed,
               const base::FilePath& user_data_dir = base::FilePath()) {
    record_.reset();
    record_.emplace(dir_,
                    (user_data_dir.empty() ? user_data_dir_ : user_data_dir)
                        .Append(kWorkflowRecordFileName),
                    may_seed);
  }

  AutomationStorage& storage() { return *storage_; }
  std::optional<Script> Load(const std::string& id) {
    return storage().Load(id);
  }
  base::FilePath File(const std::string& id) {
    return dir_.AppendASCII(id + ".molt");
  }
  // A file written by another program.
  void Plant(const std::string& id, const Script& script) {
    ASSERT_TRUE(base::CreateDirectory(dir_));
    ASSERT_TRUE(base::WriteFile(File(id), script.ToJSONString()));
  }
  std::string Bytes(const std::string& id) {
    std::string bytes;
    EXPECT_TRUE(base::ReadFileToString(File(id), &bytes));
    return bytes;
  }
  // What the file holds, as it is (not as the storage loads it).
  std::optional<Script> Raw(const std::string& id) {
    return Script::FromJSONString(Bytes(id));
  }
  // The file's last fire time, written by another program.
  void SetLastFiredOutside(const std::string& id, int64_t last_fired) {
    EditRaw(id, [last_fired](base::DictValue& d) {
      d.FindDict("trigger")->Set("last_fired_unix",
                                 static_cast<double>(last_fired));
    });
  }
  // The file's JSON edited by another program.
  void EditRaw(const std::string& id,
               base::FunctionRef<void(base::DictValue&)> edit) {
    std::optional<base::Value> value =
        base::JSONReader::Read(Bytes(id), base::JSON_ALLOW_TRAILING_COMMAS);
    ASSERT_TRUE(value && value->is_dict());
    edit(value->GetDict());
    std::string out;
    ASSERT_TRUE(base::JSONWriter::WriteWithOptions(
        *value, base::JSONWriter::OPTIONS_PRETTY_PRINT, &out));
    ASSERT_TRUE(base::WriteFile(File(id), out));
  }
  // Whether the record vouches for the file |name| as it is now.
  bool Vouched(const std::string& name) {
    std::string bytes;
    if (!base::ReadFileToString(dir_.AppendASCII(name), &bytes)) {
      return false;
    }
    std::optional<Script> script = Script::FromJSONString(bytes);
    WorkflowRecord* record = WorkflowRecord::For(dir_);
    return script && record &&
           record->Vouches(name, AutomationSecurity::SecurityDigest(*script));
  }
  // The names the record file of |user_data_dir| lists.
  std::set<std::string> RecordedNames(
      const base::FilePath& user_data_dir = base::FilePath()) {
    std::set<std::string> names;
    std::string contents;
    if (!base::ReadFileToString(
            (user_data_dir.empty() ? user_data_dir_ : user_data_dir)
                .Append(kWorkflowRecordFileName),
            &contents)) {
      return names;
    }
    std::optional<base::Value> value =
        base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
    if (value && value->is_dict()) {
      if (const base::DictValue* files = value->GetDict().FindDict("files")) {
        for (const auto item : *files) {
          names.insert(item.first);
        }
      }
    }
    return names;
  }

  base::ScopedTempDir temp_;
  base::FilePath dir_;
  base::FilePath user_data_dir_;
  std::optional<ScopedWorkflowRecordForTesting> record_;
  std::optional<AutomationStorage> storage_;
};

TEST_F(AutomationStorageRecordTest,
       AFileWrittenOutsideLoadsAsAnUnconfirmedImport) {
  Plant("wf_1", Claiming("wf_1"));
  const std::string bytes = Bytes("wf_1");
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(IsClamped(*loaded));
  EXPECT_EQ(loaded->security.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
  AutomationSecurity security;
  EXPECT_FALSE(
      security.AllowSubmit(AutomationSecurity::EffectivePolicy(*loaded)));
  // What the user reviews is kept.
  EXPECT_EQ(loaded->trigger.type, TriggerType::CRON);
  EXPECT_EQ(loaded->trigger.expression, "0 9 * * *");
  EXPECT_EQ(loaded->steps.size(), 1u);
  EXPECT_EQ(loaded->name, "Prices");
  // A listing the same, and the file as it was.
  std::vector<Script> all = storage().ListAll();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_TRUE(IsClamped(all[0]));
  EXPECT_EQ(Bytes("wf_1"), bytes);
}

TEST_F(AutomationStorageRecordTest, EverySecurityFieldChangedOutsideIsCaught) {
  using Edit = void (*)(base::DictValue&);
  const std::pair<const char*, Edit> kEdits[] = {
      {"a step",
       [](base::DictValue& d) {
         (*d.FindList("steps"))[0].GetDict().Set("target",
                                                 "https://evil.example/");
       }},
      {"trust",
       [](base::DictValue& d) {
         d.FindDict("security")->Set("trust", "admin");
       }},
      {"whitelist",
       [](base::DictValue& d) {
         base::ListValue sites;
         sites.Append("evil.example");
         d.FindDict("security")->Set("domain_whitelist", std::move(sites));
       }},
      {"trigger",
       [](base::DictValue& d) {
         d.FindDict("trigger")->Set("expression", "* * * * *");
       }},
      {"enabled", [](base::DictValue& d) { d.Set("enabled", false); }},
      {"variables",
       [](base::DictValue& d) {
         d.FindDict("default_variables")->Set("to", "someone@evil.example");
       }},
      {"name", [](base::DictValue& d) { d.Set("name", "Renamed"); }},
  };
  for (const auto& [what, edit] : kEdits) {
    Script saved = Named("wf_1", "Prices");
    saved.trigger.type = TriggerType::CRON;
    saved.trigger.expression = "0 9 * * *";
    saved.security.trust = TrustLevel::APPROVED;
    ASSERT_TRUE(storage().Save(saved));
    std::optional<Script> before = Load("wf_1");
    ASSERT_TRUE(before);
    EXPECT_FALSE(before->security.changed_outside) << what;
    EXPECT_EQ(before->security.trust, TrustLevel::APPROVED) << what;
    EditRaw("wf_1", edit);
    std::optional<Script> after = Load("wf_1");
    ASSERT_TRUE(after);
    EXPECT_TRUE(IsClamped(*after)) << what;
  }
}

TEST_F(AutomationStorageRecordTest, RunBookkeepingMayChangeOutside) {
  ASSERT_TRUE(storage().Save(Named("wf_1", "Prices")));
  EditRaw("wf_1",
          [](base::DictValue& d) { d.FindDict("stats")->Set("runs", 99); });
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->stats.runs, 99);
  // A last fire (another browser's run) too, which isn't this browser's:
  // its own is still none.
  SetLastFiredOutside("wf_1", NowUnix());
  loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, 0);
  EXPECT_TRUE(Vouched("wf_1.molt"));
}

TEST_F(AutomationStorageRecordTest,
       AnEarlierLastFiredNeverMovesTheScheduleBack) {
  Script interval = Named("wf_1", "Prices");
  interval.trigger.type = TriggerType::INTERVAL;
  interval.trigger.expression = "3600";
  ASSERT_TRUE(storage().Save(interval));
  ASSERT_TRUE(storage().RecordFire("wf_1", kFired));
  EXPECT_EQ(Raw("wf_1")->trigger.last_fired_unix, kFired);
  EditRaw("wf_1", [](base::DictValue& d) {
    d.FindDict("trigger")->Set("last_fired_unix", 1.0);
  });
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, kFired);
}

// Saved here (or seeded at the upgrade) and not fired here yet: an earlier
// last fire written outside, which would force a catch-up run, is still
// not believed.
TEST_F(AutomationStorageRecordTest,
       AnEarlierLastFiredOfAWorkflowNotFiredHereIsNotBelieved) {
  Script legacy = Hourly("legacy");
  Plant("legacy", legacy);
  Restart(/*may_seed=*/true);
  ASSERT_FALSE(Load("legacy")->security.changed_outside);
  SetLastFiredOutside("legacy", 1);
  std::optional<Script> loaded = Load("legacy");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, 0);

  ASSERT_TRUE(storage().Save(Hourly("wf_1")));
  SetLastFiredOutside("wf_1", 1);
  loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, 0);
  SetLastFiredOutside("wf_1", NowUnix() - 86400);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, 0);
  // Nor once written back (a Trust click), nor at the next start.
  ASSERT_TRUE(storage().SetTrustLevel("wf_1", TrustLevel::APPROVED));
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, 0);
  SetLastFiredOutside("wf_1", 1);
  Restart(/*may_seed=*/false);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, 0);
}

// A later last fire written outside (another browser's Snooze or Skip, or
// forged) changes nothing either: a bookkeeping write (a run's stats)
// keeps it in the file and records nothing, and this browser's own fire
// holds, across a restart too, and is what a Trust click writes.
TEST_F(AutomationStorageRecordTest,
       ALaterLastFiredWrittenOutsideChangesNothing) {
  ASSERT_TRUE(storage().Save(Hourly("wf_1")));
  const int64_t fired = NowUnix() - 7200;
  ASSERT_TRUE(storage().RecordFire("wf_1", fired));
  SetLastFiredOutside("wf_1", kFarFuture);
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, fired);
  ASSERT_TRUE(storage().UpdateBookkeeping("wf_1", [](Script& script) {
    script.stats.runs = 1;
    return true;
  }));
  EXPECT_EQ(Raw("wf_1")->trigger.last_fired_unix, kFarFuture);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, fired);
  Restart(/*may_seed=*/false);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, fired);
  ASSERT_TRUE(storage().SetTrustLevel("wf_1", TrustLevel::APPROVED));
  EXPECT_EQ(Raw("wf_1")->trigger.last_fired_unix, fired);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, fired);
}

// A schedule saved afresh (the Agent studio's Schedule: a new trigger,
// never fired) starts over, as it did before the record: no catch-up run
// of the old schedule's last fire. It also clears a fire time recorded on
// a clock that was ahead.
TEST_F(AutomationStorageRecordTest, AScheduleSavedAfreshStartsOver) {
  Script daily = Named("wf_1", "Prices");
  daily.trigger.type = TriggerType::CRON;
  daily.trigger.expression = "0 9 * * *";
  ASSERT_TRUE(storage().Save(daily));
  ASSERT_TRUE(storage().RecordFire("wf_1", NowUnix() - 6 * 3600));
  Trigger noon;
  noon.type = TriggerType::CRON;
  noon.expression = "0 12 * * *";
  auto reschedule = [&noon](Script& script) {
    script.trigger = noon;
    return true;
  };
  ASSERT_TRUE(storage().Update("wf_1", reschedule));
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.expression, "0 12 * * *");
  EXPECT_EQ(loaded->trigger.last_fired_unix, 0);
  // A fire on a clock far ahead, then the schedule saved again.
  ASSERT_TRUE(storage().RecordFire("wf_1", kFarFuture));
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, kFarFuture);
  ASSERT_TRUE(storage().Update("wf_1", reschedule));
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, 0);
  Restart(/*may_seed=*/false);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, 0);
}

// A save from a page opened before the last fire writes the fire before
// it: the fire recorded is kept, so the next listing runs no catch-up.
TEST_F(AutomationStorageRecordTest, AStalePagesSaveKeepsTheLastFire) {
  Script daily = Named("wf_1", "Prices");
  daily.trigger.type = TriggerType::CRON;
  daily.trigger.expression = "0 9 * * *";
  ASSERT_TRUE(storage().Save(daily));
  const int64_t now = NowUnix();
  ASSERT_TRUE(storage().RecordFire("wf_1", now - 86400));
  // The page lists it, then the scheduler fires it.
  std::optional<Script> listed = Load("wf_1");
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->trigger.last_fired_unix, now - 86400);
  ASSERT_TRUE(storage().RecordFire("wf_1", now - 60));
  // The page saves its copy, renamed.
  listed->name = "Prices, renamed";
  ASSERT_TRUE(storage().Save(*listed));
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->name, "Prices, renamed");
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, now - 60);
  Restart(/*may_seed=*/false);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, now - 60);
}

// Two user data dirs over one folder each keep their own last fire: one's
// fire, written into the file, doesn't move the other's, nor does the
// other's save, which writes its own earlier one there, move the first's
// back.
TEST_F(AutomationStorageRecordTest, EachUserDataDirKeepsItsOwnLastFire) {
  const base::FilePath a = user_data_dir_;
  const base::FilePath b = temp_.GetPath().AppendASCII("Other User Data");
  ASSERT_TRUE(base::CreateDirectory(b));
  const int64_t start = NowUnix() - 7200;
  ASSERT_TRUE(storage().Save(Hourly("wf_1")));
  ASSERT_TRUE(storage().RecordFire("wf_1", start));
  // B's first start of this version seeds it, with the file's last fire.
  Restart(/*may_seed=*/true, b);
  ASSERT_EQ(Load("wf_1")->trigger.last_fired_unix, start);
  // B runs it an hour later.
  ASSERT_TRUE(storage().RecordFire("wf_1", start + 3600));
  ASSERT_EQ(Raw("wf_1")->trigger.last_fired_unix, start + 3600);
  // A still counts from its own fire, and its page saves its copy.
  Restart(/*may_seed=*/false, a);
  std::optional<Script> page = Load("wf_1");
  ASSERT_TRUE(page);
  EXPECT_FALSE(page->security.changed_outside);
  EXPECT_EQ(page->trigger.last_fired_unix, start);
  ASSERT_TRUE(storage().SaveChecked(
      std::move(*page),
      [](const Script* stored, Script&) { return stored != nullptr; }));
  EXPECT_EQ(Raw("wf_1")->trigger.last_fired_unix, start);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, start);
  // B: the same workflow, still vouched for, and still its own fire.
  Restart(/*may_seed=*/true, b);
  std::optional<Script> in_b = Load("wf_1");
  ASSERT_TRUE(in_b);
  EXPECT_FALSE(in_b->security.changed_outside);
  EXPECT_EQ(in_b->trigger.last_fired_unix, start + 3600);
}

// A fire recorded while the clock was ahead gives way to the next fire on
// the corrected clock: it would hold back every catch-up run until then.
TEST_F(AutomationStorageRecordTest,
       AFireOnAClockThatWasAheadGivesWayToTheNext) {
  ASSERT_TRUE(storage().Save(Hourly("wf_1")));
  const int64_t now = NowUnix();
  const int64_t ahead = now + 7 * 86400;
  ASSERT_TRUE(storage().RecordFire("wf_1", ahead));
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, ahead);
  ASSERT_TRUE(storage().RecordFire("wf_1", now));
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, now);
  Restart(/*may_seed=*/false);
  EXPECT_EQ(Load("wf_1")->trigger.last_fired_unix, now);
}

// The record is changed before the file: a browser that ends in between
// (the record as it was then, the file as it was before) leaves the old
// file needing a review, so a trust taken away can't come back by writing
// the old bytes back.
TEST_F(AutomationStorageRecordTest, ABrowserEndingMidWriteLeavesTheOldFile) {
  Script trusted = Named("wf_1", "Prices");
  trusted.security.trust = TrustLevel::TRUSTED;
  ASSERT_TRUE(storage().Save(trusted));
  const std::string old_bytes = Bytes("wf_1");
  const base::FilePath record_file =
      user_data_dir_.Append(kWorkflowRecordFileName);
  std::string record_then;
  storage().SetBeforeFileChangeForTesting(base::BindLambdaForTesting([&] {
    ASSERT_TRUE(base::ReadFileToString(record_file, &record_then));
    EXPECT_EQ(Bytes("wf_1"), old_bytes);
  }));
  ASSERT_TRUE(storage().SetTrustLevel("wf_1", TrustLevel::CASUAL));
  storage().SetBeforeFileChangeForTesting(base::RepeatingClosure());
  ASSERT_FALSE(record_then.empty());
  ASSERT_TRUE(base::WriteFile(record_file, record_then));
  ASSERT_TRUE(base::WriteFile(File("wf_1"), old_bytes));
  Restart(/*may_seed=*/false);
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(IsClamped(*loaded));
}

// The same for a deletion: forgotten before the file goes.
TEST_F(AutomationStorageRecordTest, ABrowserEndingMidDeleteLeavesTheFile) {
  Script trusted = Named("wf_1", "Prices");
  trusted.security.trust = TrustLevel::TRUSTED;
  ASSERT_TRUE(storage().Save(trusted));
  const std::string bytes = Bytes("wf_1");
  const base::FilePath record_file =
      user_data_dir_.Append(kWorkflowRecordFileName);
  std::string record_then;
  storage().SetBeforeFileChangeForTesting(base::BindLambdaForTesting([&] {
    ASSERT_TRUE(base::ReadFileToString(record_file, &record_then));
    EXPECT_TRUE(base::PathExists(File("wf_1")));
  }));
  ASSERT_TRUE(storage().Delete("wf_1"));
  storage().SetBeforeFileChangeForTesting(base::RepeatingClosure());
  ASSERT_FALSE(record_then.empty());
  ASSERT_TRUE(base::WriteFile(record_file, record_then));
  ASSERT_TRUE(base::WriteFile(File("wf_1"), bytes));
  Restart(/*may_seed=*/false);
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(IsClamped(*loaded));
}

#if BUILDFLAG(IS_POSIX)
// A write that fails after the record was set puts the record back exactly
// as it was: the file it couldn't replace is still vouched for.
TEST_F(AutomationStorageRecordTest, AWriteThatFailsLeavesTheRecordAsItWas) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "root writes into a read-only directory";
  }
  Script trusted = Named("wf_1", "Prices");
  trusted.security.trust = TrustLevel::TRUSTED;
  ASSERT_TRUE(storage().Save(trusted));
  const base::FilePath record_file =
      user_data_dir_.Append(kWorkflowRecordFileName);
  std::string record_before;
  ASSERT_TRUE(base::ReadFileToString(record_file, &record_before));
  ASSERT_TRUE(base::SetPosixFilePermissions(dir_, 0500));
  EXPECT_FALSE(storage().SetTrustLevel("wf_1", TrustLevel::CASUAL));
  ASSERT_TRUE(base::SetPosixFilePermissions(dir_, 0700));
  std::string record_after;
  ASSERT_TRUE(base::ReadFileToString(record_file, &record_after));
  EXPECT_EQ(record_after, record_before);
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->security.trust, TrustLevel::TRUSTED);
}
#endif  // BUILDFLAG(IS_POSIX)

TEST_F(AutomationStorageRecordTest, AHandCopyUnderAnotherIdIsNotVouched) {
  Script original = Named("a", "Prices");
  original.security.trust = TrustLevel::APPROVED;
  ASSERT_TRUE(storage().Save(original));
  ASSERT_TRUE(base::CopyFile(File("a"), File("b")));
  EXPECT_TRUE(IsClamped(*Load("b")));
  EXPECT_FALSE(Load("a")->security.changed_outside);
  EXPECT_EQ(Load("a")->security.trust, TrustLevel::APPROVED);
}

TEST_F(AutomationStorageRecordTest, ARolledBackFileIsNotVouched) {
  Script v1 = Named("wf_1", "Prices");
  v1.security.trust = TrustLevel::TRUSTED;
  ASSERT_TRUE(storage().Save(v1));
  const std::string v1_bytes = Bytes("wf_1");
  ASSERT_TRUE(storage().SetTrustLevel("wf_1", TrustLevel::CASUAL));
  ASSERT_TRUE(base::WriteFile(File("wf_1"), v1_bytes));
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->security.trust, TrustLevel::CASUAL);
}

TEST_F(AutomationStorageRecordTest, ADeletedWorkflowPutBackIsNotVouched) {
  Script script = Named("wf_1", "Prices");
  script.security.trust = TrustLevel::APPROVED;
  ASSERT_TRUE(storage().Save(script));
  const std::string bytes = Bytes("wf_1");
  ASSERT_TRUE(storage().Delete("wf_1"));
  ASSERT_TRUE(base::WriteFile(File("wf_1"), bytes));
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->security.trust, TrustLevel::CASUAL);
}

TEST_F(AutomationStorageRecordTest, EveryWritePathVouches) {
  auto vouched = [this](const std::string& id) {
    std::optional<Script> script = Load(id);
    return script && !script->security.changed_outside && Vouched(id + ".molt");
  };
  ASSERT_TRUE(storage().Save(Named("a", "A")));
  EXPECT_TRUE(vouched("a"));
  // Under the first free id.
  std::optional<std::string> fresh = storage().SaveNew(Named("a", "A again"));
  ASSERT_EQ(fresh, "a_");
  EXPECT_TRUE(vouched("a_"));
  EXPECT_TRUE(storage().SaveChecked(
      Named("c", "C"), [](const Script*, Script&) { return true; }));
  EXPECT_TRUE(vouched("c"));
  EXPECT_TRUE(storage().Update("c", [](Script& script) {
    script.name = "C2";
    return true;
  }));
  EXPECT_TRUE(vouched("c"));
  EXPECT_TRUE(storage().SetTrustLevel("c", TrustLevel::APPROVED));
  EXPECT_TRUE(vouched("c"));
  EXPECT_EQ(Load("c")->security.trust, TrustLevel::APPROVED);
  // A move put back under its id, and under a fresh one when a workflow of
  // its id was saved meanwhile.
  ASSERT_TRUE(storage().Save(Named("d", "D")));
  ASSERT_TRUE(storage().Take("d"));
  ASSERT_TRUE(storage().ReturnTaken("d"));
  EXPECT_TRUE(vouched("d"));
  ASSERT_TRUE(storage().Take("d"));
  ASSERT_TRUE(storage().Save(Named("d", "D saved since")));
  ASSERT_TRUE(storage().ReturnTaken("d"));
  EXPECT_TRUE(vouched("d"));
  EXPECT_EQ(Load("d")->name, "D saved since");
  EXPECT_TRUE(vouched("d_"));
  EXPECT_EQ(Load("d_")->name, "D");
}

TEST_F(AutomationStorageRecordTest, WritesNeverVouchForWhatTheyReadUnvouched) {
  // Vouched for as written, but written as the unconfirmed import it read.
  auto expect_import = [this](const std::string& id, const char* what) {
    EXPECT_TRUE(Vouched(id + ".molt")) << what;
    std::optional<Script> script = Load(id);
    ASSERT_TRUE(script) << what;
    EXPECT_EQ(script->security.trust, TrustLevel::CASUAL) << what;
    EXPECT_TRUE(script->security.sites_unconfirmed) << what;
    EXPECT_TRUE(script->security.changed_outside) << what;
    EXPECT_TRUE(script->security.domain_whitelist.empty()) << what;
  };
  Plant("wf_1", Claiming("wf_1"));
  EXPECT_TRUE(storage().Update("wf_1", [](Script& script) {
    script.name = "Mine now";
    return true;
  }));
  expect_import("wf_1", "Update");

  Plant("wf_2", Claiming("wf_2"));
  std::optional<Script> shown = Load("wf_2");
  ASSERT_TRUE(shown);
  shown->name = "Edited";
  EXPECT_TRUE(storage().SaveChecked(
      std::move(*shown), [](const Script* stored, Script& script) {
        AutomationSecurity::ClampTrustForSave(script, stored);
        return true;
      }));
  expect_import("wf_2", "SaveChecked");

  Plant("wf_3", Claiming("wf_3"));
  std::optional<Script> loaded = Load("wf_3");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(storage().Save(*loaded));
  expect_import("wf_3", "Save");
}

TEST_F(AutomationStorageRecordTest,
       SetTrustLevelRefusesAWorkflowNeedingReview) {
  Plant("wf_1", Claiming("wf_1"));
  const std::string bytes = Bytes("wf_1");
  EXPECT_FALSE(storage().SetTrustLevel("wf_1", TrustLevel::APPROVED));
  EXPECT_EQ(Bytes("wf_1"), bytes);
  // Nor a draft from an agent run.
  Script draft = Named("wf_2", "Draft");
  draft.security.sites_unconfirmed = true;
  ASSERT_TRUE(storage().Save(draft));
  const std::string draft_bytes = Bytes("wf_2");
  EXPECT_FALSE(storage().SetTrustLevel("wf_2", TrustLevel::APPROVED));
  EXPECT_EQ(Bytes("wf_2"), draft_bytes);
}

TEST_F(AutomationStorageRecordTest,
       UpdateBookkeepingKeepsTheVerdictAndRefusesSecurityChanges) {
  ASSERT_TRUE(storage().Save(Named("a", "A")));
  EXPECT_TRUE(storage().UpdateBookkeeping("a", [](Script& script) {
    script.stats.runs = 3;
    return true;
  }));
  EXPECT_TRUE(Vouched("a.molt"));
  EXPECT_EQ(Load("a")->stats.runs, 3);

  // A file changed outside: still changed, its security content as it was
  // (another browser's history isn't lost, nor its claims laundered).
  Plant("b", Claiming("b"));
  const base::DictValue security =
      Raw("b")->ToJSON().FindDict("security")->Clone();
  EXPECT_TRUE(storage().UpdateBookkeeping("b", [](Script& script) {
    script.stats.runs = 5;
    return true;
  }));
  EXPECT_FALSE(Vouched("b.molt"));
  std::optional<Script> raw = Raw("b");
  ASSERT_TRUE(raw);
  EXPECT_EQ(raw->stats.runs, 5);
  EXPECT_EQ(*raw->ToJSON().FindDict("security"), security);
  EXPECT_TRUE(IsClamped(*Load("b")));

  // Anything else: refused, nothing written.
  const std::string bytes = Bytes("a");
  EXPECT_FALSE(storage().UpdateBookkeeping("a", [](Script& script) {
    script.security.trust = TrustLevel::ADMIN;
    return true;
  }));
  EXPECT_FALSE(storage().UpdateBookkeeping("a", [](Script& script) {
    script.steps.clear();
    return true;
  }));
  EXPECT_EQ(Bytes("a"), bytes);
  EXPECT_FALSE(
      storage().UpdateBookkeeping("gone", [](Script&) { return true; }));
}

#if BUILDFLAG(IS_POSIX)
TEST_F(AutomationStorageRecordTest,
       RecordFireKeepsTheFloorWhenTheFileIsNotWritten) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "root writes into a read-only directory";
  }
  Script interval = Named("wf_1", "Prices");
  interval.trigger.type = TriggerType::INTERVAL;
  interval.trigger.expression = "3600";
  ASSERT_TRUE(storage().Save(interval));
  ASSERT_TRUE(base::SetPosixFilePermissions(dir_, 0500));
  EXPECT_FALSE(storage().RecordFire("wf_1", kFired));
  std::optional<Script> loaded = Load("wf_1");
  ASSERT_TRUE(base::SetPosixFilePermissions(dir_, 0700));
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, kFired);
  EXPECT_EQ(Raw("wf_1")->trigger.last_fired_unix, 0);
}
#endif  // BUILDFLAG(IS_POSIX)

TEST_F(AutomationStorageRecordTest, TheSecretScrubRewriteKeepsTheVerdict) {
  // A workflow from before secret steps, with a plaintext password in it,
  // there at the upgrade.
  Script legacy = Named("legacy", "Sign in");
  legacy.security.trust = TrustLevel::TRUSTED;
  Step password;
  password.type = StepType::TYPE;
  password.target = "[name=\"passwd\"]";
  password.value = "s3cret!";
  legacy.steps.push_back(std::move(password));
  Plant("legacy", legacy);
  ASSERT_NE(Bytes("legacy").find("s3cret!"), std::string::npos);
  Restart(/*may_seed=*/true);
  std::optional<Script> loaded = Load("legacy");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->security.trust, TrustLevel::TRUSTED);
  EXPECT_EQ(Bytes("legacy").find("s3cret!"), std::string::npos);
  EXPECT_FALSE(Load("legacy")->security.changed_outside);
  // Planted afterwards: scrubbed as well, but never vouched for.
  legacy.id = "planted";
  Plant("planted", legacy);
  loaded = Load("planted");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(IsClamped(*loaded));
  EXPECT_EQ(Bytes("planted").find("s3cret!"), std::string::npos);
  EXPECT_TRUE(IsClamped(*Load("planted")));
}

TEST_F(AutomationStorageRecordTest, TakeAndReturnCarryTheVerdict) {
  Script script = Named("a", "A");
  script.security.trust = TrustLevel::APPROVED;
  ASSERT_TRUE(storage().Save(script));
  std::optional<Script> taken = storage().Take("a");
  ASSERT_TRUE(taken);
  EXPECT_FALSE(taken->security.changed_outside);
  EXPECT_EQ(taken->security.trust, TrustLevel::APPROVED);
  ASSERT_TRUE(storage().ReturnTaken("a"));
  EXPECT_FALSE(Load("a")->security.changed_outside);
  EXPECT_EQ(Load("a")->security.trust, TrustLevel::APPROVED);
  // Returned as a_, since a was saved meanwhile.
  ASSERT_TRUE(storage().Take("a"));
  ASSERT_TRUE(storage().Save(Named("a", "saved since")));
  ASSERT_TRUE(storage().ReturnTaken("a"));
  std::optional<Script> back = Load("a_");
  ASSERT_TRUE(back);
  EXPECT_FALSE(back->security.changed_outside);
  EXPECT_EQ(back->security.trust, TrustLevel::APPROVED);
  // A changed one stays changed, taken and put back.
  Plant("b", Claiming("b"));
  taken = storage().Take("b");
  ASSERT_TRUE(taken);
  EXPECT_TRUE(IsClamped(*taken));
  ASSERT_TRUE(storage().ReturnTaken("b"));
  EXPECT_TRUE(IsClamped(*Load("b")));
}

// A move cut short on either side of its rename (the browser ended, or
// another browser's move was in progress at this start): put back, the
// workflow is vouched for as it was.
TEST_F(AutomationStorageRecordTest, AMoveCutShortIsVouchedForOncePutBack) {
  Script script = Named("a", "A");
  script.security.trust = TrustLevel::APPROVED;
  ASSERT_TRUE(storage().Save(script));
  const base::Time before_grace = base::Time::Now() - base::Minutes(5);
  // Renamed, before the record had it under the new name.
  const base::FilePath renamed = dir_.AppendASCII("a.molt.00ff.taken");
  ASSERT_TRUE(base::ReplaceFile(File("a"), renamed, nullptr));
  ASSERT_TRUE(base::TouchFile(renamed, before_grace, before_grace));
  Restart(/*may_seed=*/false);
  std::vector<AutomationStorage::SetAside> cut = storage().InterruptedTakes();
  ASSERT_EQ(cut.size(), 1u);
  ASSERT_TRUE(storage().ReturnSetAside(cut[0]));
  std::optional<Script> back = Load("a");
  ASSERT_TRUE(back);
  EXPECT_FALSE(back->security.changed_outside);
  EXPECT_EQ(back->security.trust, TrustLevel::APPROVED);

  // Renamed after the record had it under both names (as Take() does),
  // before the old one was forgotten.
  WorkflowRecord* record = WorkflowRecord::For(dir_);
  ASSERT_TRUE(record && record->loaded());
  ASSERT_TRUE(record->Copy("a.molt", "a.molt.0abc.taken"));
  const base::FilePath copied = dir_.AppendASCII("a.molt.0abc.taken");
  ASSERT_TRUE(base::ReplaceFile(File("a"), copied, nullptr));
  ASSERT_TRUE(base::TouchFile(copied, before_grace, before_grace));
  Restart(/*may_seed=*/false);
  cut = storage().InterruptedTakes();
  ASSERT_EQ(cut.size(), 1u);
  std::optional<Script> read = storage().ReadSetAside(cut[0]);
  ASSERT_TRUE(read);
  EXPECT_FALSE(read->security.changed_outside);
  EXPECT_EQ(read->security.trust, TrustLevel::APPROVED);
  ASSERT_TRUE(storage().ReturnSetAside(cut[0]));
  back = Load("a");
  ASSERT_TRUE(back);
  EXPECT_FALSE(back->security.changed_outside);
  EXPECT_EQ(back->security.trust, TrustLevel::APPROVED);
  EXPECT_EQ(RecordedNames(), std::set<std::string>{"a.molt"});
}

TEST_F(AutomationStorageRecordTest, FinishTakeForgetsTheSetAsideFile) {
  ASSERT_TRUE(storage().Save(Named("a", "A")));
  ASSERT_TRUE(storage().Take("a"));
  const std::vector<base::FilePath> set_aside = SetAsideFiles(dir_, "a");
  ASSERT_EQ(set_aside.size(), 1u);
  const std::string name = set_aside[0].BaseName().AsUTF8Unsafe();
  EXPECT_EQ(RecordedNames(), std::set<std::string>{name});
  storage().FinishTake("a");
  EXPECT_FALSE(base::PathExists(set_aside[0]));
  EXPECT_TRUE(RecordedNames().empty());
}

// A read never takes a save of this browser's in progress for a change:
// the file renamed before the record is set, or the reverse (shaped like
// UpdatesNeverInterleave).
TEST_F(AutomationStorageRecordTest,
       ReadsNeverSeeASaveInProgressAsChangedOutside) {
  base::test::TaskEnvironment task_environment;
  ASSERT_TRUE(storage().Save(Named("wf_1", "Prices")));
  constexpr int kRounds = 40;
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath dir) {
            AutomationStorage studio(dir);
            for (int i = 0; i < kRounds; ++i) {
              EXPECT_TRUE(studio.Update("wf_1", [i](Script& script) {
                script.steps[0].target =
                    "https://news.example/" + base::NumberToString(i);
                return true;
              }));
            }
          },
          dir_));
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath dir) {
            AutomationStorage reader(dir);
            for (int i = 0; i < 2 * kRounds; ++i) {
              std::optional<Script> script = reader.Load("wf_1");
              ASSERT_TRUE(script);
              EXPECT_FALSE(script->security.changed_outside) << i;
            }
          },
          dir_));
  task_environment.RunUntilIdle();
  EXPECT_FALSE(Load("wf_1")->security.changed_outside);
}

// Two user data dirs (an installed and a portable browser) over one
// ~/.moltbrowser: runs in one leave the other's record intact (its last
// fire too); a change to what a workflow does needs a review in the other;
// reviews converge.
TEST_F(AutomationStorageRecordTest, TwoUserDataDirsOverOneDirectory) {
  const base::FilePath a = user_data_dir_;
  const base::FilePath b = temp_.GetPath().AppendASCII("Other User Data");
  ASSERT_TRUE(base::CreateDirectory(b));
  // A, a new user data dir, saves a workflow.
  Script workflow = Named("wf_1", "Prices");
  workflow.security.trust = TrustLevel::APPROVED;
  ASSERT_TRUE(storage().Save(workflow));
  // B's first start of this version seeds it.
  Restart(/*may_seed=*/true, b);
  EXPECT_FALSE(Load("wf_1")->security.changed_outside);
  // B runs it: bookkeeping only.
  ASSERT_TRUE(storage().UpdateBookkeeping("wf_1", [](Script& script) {
    script.stats.runs = 1;
    return true;
  }));
  ASSERT_TRUE(storage().RecordFire("wf_1", kFired));
  Restart(/*may_seed=*/false, a);
  std::optional<Script> in_a = Load("wf_1");
  ASSERT_TRUE(in_a);
  EXPECT_FALSE(in_a->security.changed_outside);
  EXPECT_EQ(in_a->stats.runs, 1);
  // B's fire is B's: A never fired it.
  EXPECT_EQ(in_a->trigger.last_fired_unix, 0);
  // B changes what it is: A sees it changed.
  Restart(/*may_seed=*/true, b);
  ASSERT_TRUE(storage().Update("wf_1", [](Script& script) {
    script.name = "Prices (B)";
    return true;
  }));
  Restart(/*may_seed=*/false, a);
  in_a = Load("wf_1");
  ASSERT_TRUE(in_a);
  EXPECT_TRUE(IsClamped(*in_a));
  EXPECT_EQ(in_a->name, "Prices (B)");
  // A confirms it: CASUAL on its site.
  ASSERT_TRUE(storage().Update("wf_1", [](Script& script) {
    return AutomationSecurity::ConfirmDraftSites(script, {"news.example"});
  }));
  in_a = Load("wf_1");
  EXPECT_FALSE(in_a->security.changed_outside);
  EXPECT_EQ(in_a->security.trust, TrustLevel::CASUAL);
  // B sees A's write changed; its confirm writes the same content, which
  // A still vouches for.
  Restart(/*may_seed=*/true, b);
  EXPECT_TRUE(IsClamped(*Load("wf_1")));
  ASSERT_TRUE(storage().Update("wf_1", [](Script& script) {
    return AutomationSecurity::ConfirmDraftSites(script, {"news.example"});
  }));
  EXPECT_FALSE(Load("wf_1")->security.changed_outside);
  Restart(/*may_seed=*/false, a);
  EXPECT_FALSE(Load("wf_1")->security.changed_outside);
  EXPECT_EQ(RecordedNames(a), std::set<std::string>{"wf_1.molt"});
  EXPECT_EQ(RecordedNames(b), std::set<std::string>{"wf_1.molt"});
}

TEST_F(AutomationStorageRecordTest, SeedingKeepsEveryLegacyWorkflowAsItWas) {
  // What the previous build ran: a TRUSTED scheduled workflow, a move cut
  // short, and (POSIX) a link, which is never a workflow.
  Plant("legacy", Claiming("legacy"));
  Script old = Named("old", "Old");
  old.security.trust = TrustLevel::APPROVED;
  const base::FilePath old_file = dir_.AppendASCII("old.molt.0123abcd.taken");
  ASSERT_TRUE(base::WriteFile(old_file, old.ToJSONString()));
#if BUILDFLAG(IS_POSIX)
  const base::FilePath target = temp_.GetPath().AppendASCII("target.json");
  ASSERT_TRUE(base::WriteFile(target, Claiming("link").ToJSONString()));
  ASSERT_TRUE(base::CreateSymbolicLink(target, File("link")));
#endif
  Restart(/*may_seed=*/true);
  std::optional<Script> legacy = Load("legacy");
  ASSERT_TRUE(legacy);
  EXPECT_FALSE(legacy->security.changed_outside);
  EXPECT_EQ(legacy->security.trust, TrustLevel::TRUSTED);
  EXPECT_EQ(legacy->security.domain_whitelist,
            std::vector<std::string>{"evil.example"});
  EXPECT_EQ(legacy->stats.runs, 7);
  EXPECT_TRUE(AutomationSecurity::MayFireUnattended(*legacy));
  std::optional<Script> set_aside = storage().ReadSetAside({"old", old_file});
  ASSERT_TRUE(set_aside);
  EXPECT_FALSE(set_aside->security.changed_outside);
  EXPECT_EQ(set_aside->security.trust, TrustLevel::APPROVED);
  EXPECT_EQ(RecordedNames(),
            (std::set<std::string>{"legacy.molt", "old.molt.0123abcd.taken"}));
  bool seeded = false;
  for (const std::string& line : storage().ReadAuditTail()) {
    seeded |= line.find("record_seeded | 2 workflows") != std::string::npos;
  }
  EXPECT_TRUE(seeded);
  // A file added afterwards needs a review.
  Plant("later", Claiming("later"));
  EXPECT_TRUE(IsClamped(*Load("later")));
}

TEST_F(AutomationStorageRecordTest, ADirectoryWithoutARecordIsUnchanged) {
  // A profile's voice routines, a test's directory: files as they are.
  const base::FilePath own = temp_.GetPath().AppendASCII("Profile 1");
  ASSERT_TRUE(base::CreateDirectory(own));
  ASSERT_TRUE(base::WriteFile(own.AppendASCII("wf_1.molt"),
                              Claiming("wf_1").ToJSONString()));
  AutomationStorage mine(own);
  std::optional<Script> loaded = mine.Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->security.trust, TrustLevel::TRUSTED);
}

// At startup the record is configured and loaded at once (what
// ConfigureWorkflowRecordOnce() does): a first launch that reads no
// workflow (a guest window, the profile picker) still records a new user
// data directory as one, so files planted before the next start (which may
// seed: not Chrome's first run any more) aren't seeded.
TEST_F(AutomationStorageRecordTest,
       ANewUserDataDirIsRecordedAtStartupEvenIfNothingIsRead) {
  base::test::TaskEnvironment task_environment;
  WorkflowRecord::ResetForTesting();
  SetMoltHomeDirForTesting(temp_.GetPath().AppendASCII("home"));
  const base::FilePath udd = temp_.GetPath().AppendASCII("New User Data");
  ASSERT_TRUE(base::CreateDirectory(udd));
  const base::FilePath record_file = udd.Append(kWorkflowRecordFileName);
  AutomationStorage::ConfigureSharedRecord(record_file, /*may_seed=*/false);
  task_environment.RunUntilIdle();
  EXPECT_TRUE(base::PathExists(record_file));
  EXPECT_TRUE(base::PathExists(WorkflowRecord::MarkerFileFor(record_file)));
  const base::FilePath shared_dir = AutomationStorage::SharedDirectory();
  ASSERT_TRUE(base::CreateDirectory(shared_dir));
  ASSERT_TRUE(base::WriteFile(shared_dir.AppendASCII("planted.molt"),
                              Claiming("planted").ToJSONString()));
  WorkflowRecord::ResetForTesting();
  AutomationStorage::ConfigureSharedRecord(record_file, /*may_seed=*/true);
  task_environment.RunUntilIdle();
  {
    AutomationStorage shared;
    std::optional<Script> loaded = shared.Load("planted");
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(IsClamped(*loaded));
  }
  EXPECT_TRUE(RecordedNames(udd).empty());
  WorkflowRecord::ResetForTesting();
  SetMoltHomeDirForTesting(base::FilePath());
}

TEST_F(AutomationStorageRecordTest,
       TheRealSharedDirectoryWithoutConfigurationFailsClosed) {
  WorkflowRecord::ResetForTesting();
  SetMoltHomeDirForTesting(temp_.GetPath().AppendASCII("home"));
  const base::FilePath shared_dir = AutomationStorage::SharedDirectory();
  ASSERT_TRUE(base::CreateDirectory(shared_dir));
  ASSERT_TRUE(base::WriteFile(shared_dir.AppendASCII("planted.molt"),
                              Claiming("planted").ToJSONString()));
  {
    AutomationStorage shared;
    EXPECT_TRUE(IsClamped(*shared.Load("planted")));
    // This session's own writes only.
    ASSERT_TRUE(shared.Save(Named("mine", "Mine")));
    EXPECT_FALSE(shared.Load("mine")->security.changed_outside);
  }
  // Another process: nothing was kept.
  WorkflowRecord::ResetForTesting();
  {
    AutomationStorage shared;
    EXPECT_TRUE(shared.Load("mine")->security.changed_outside);
  }
  WorkflowRecord::ResetForTesting();
  SetMoltHomeDirForTesting(base::FilePath());
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
