// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationStorage's own directory (a profile's voice routines) and the
// one-time move out of the shared storage (Take()); since round 44, files
// that aren't regular or are too big skipped and never written over, the
// audit log created and read from its end, Update() as one
// read-modify-write whichever thread, and moves cut short reported; since
// round 45, a fresh id when every simple one is taken.

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/common/molt_home_dir.h"
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

}  // namespace
}  // namespace automation
}  // namespace molt_ai
