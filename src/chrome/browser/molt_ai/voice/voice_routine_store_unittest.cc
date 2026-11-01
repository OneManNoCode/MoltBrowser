// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A profile's voice routines (voice_routine_store.h): each profile lists,
// runs, confirms and deletes only its own; a name in use is replaced only
// when asked, and a name two routines answer to runs neither; routines
// from before profiles are kept by one profile at most, as they were,
// renamed beside the profile's own; files whose contents say they are
// another profile's change nothing; a claim cut short at any point is
// settled without giving anything to a profile (round 44).

#include "chrome/browser/molt_ai/voice/voice_routine_store.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using automation::AutomationStorage;
using automation::Script;
using automation::TrustLevel;

constexpr int64_t kNow = 1'800'000'000;

RoutineStep Said(std::string_view said, std::string_view host = "") {
  ParseContext context;
  std::optional<RoutineStep> step =
      RoutineStepFor(ParseVoiceCommand(said, context), host, 0, {}, false);
  EXPECT_TRUE(step) << said;
  return step.value_or(RoutineStep());
}

std::vector<RoutineStep> MyNews() {
  RoutineStep open = Said("open YouTube");
  open.url = "https://www.youtube.com/";
  return {open, Said("search for news", "www.youtube.com")};
}

std::vector<RoutineStep> Pause() {
  return {Said("pause", "www.youtube.com")};
}

std::vector<std::string> Names(const std::vector<RoutineSummary>& routines) {
  std::vector<std::string> names;
  for (const RoutineSummary& routine : routines) {
    names.push_back(routine.name);
  }
  return names;
}

// Two profiles of one machine, and its shared storage.
class VoiceRoutineStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    shared_root_ = temp_.GetPath().AppendASCII("home");
    shared_.SetRootForTesting(shared_root_);
  }

  base::FilePath ProfileDir(std::string_view name) {
    return VoiceRoutinesDir(temp_.GetPath().AppendASCII(name));
  }
  VoiceRoutineStore Profile1() { return {ProfileDir("Default"), shared_root_}; }

  // A routine saved by round 41 or 42's browser: in the shared storage.
  std::string SaveOld(const std::string& id,
                      const std::string& name,
                      int64_t when = kNow) {
    BuiltRoutine built = BuildRoutine(id, name, MyNews(), when);
    EXPECT_TRUE(built.script);
    EXPECT_TRUE(shared_.Save(*built.script));
    return id;
  }

  base::ScopedTempDir temp_;
  base::FilePath shared_root_;
  AutomationStorage shared_;
};

TEST_F(VoiceRoutineStoreTest, EachProfileHasItsOwn) {
  VoiceRoutineStore first(ProfileDir("Default"), shared_root_);
  VoiceRoutineStore second(ProfileDir("Profile 2"), shared_root_);
  ASSERT_TRUE(first.Save("voice_1", "my news", MyNews(), false, kNow).saved);
  ASSERT_TRUE(second.Save("voice_1", "my news", Pause(), false, kNow).saved);

  // In each profile's directory, not the shared storage.
  EXPECT_TRUE(
      base::PathExists(ProfileDir("Default").AppendASCII("voice_1.molt")));
  EXPECT_TRUE(shared_.ListIds().empty());
  ASSERT_EQ(first.Summaries().size(), 1u);
  ASSERT_EQ(second.Summaries().size(), 1u);
  EXPECT_EQ(first.Summaries()[0].steps, 2);
  EXPECT_EQ(second.Summaries()[0].steps, 2);  // the site, then "pause"
  EXPECT_TRUE(first.Unclaimed().empty());

  // Each runs and deletes its own: the same name and id are another
  // routine in the other profile.
  std::optional<RoutineToRun> run = first.Load("my news", "");
  ASSERT_TRUE(run);
  ASSERT_TRUE(run->ok);
  EXPECT_EQ(run->steps.size(), 2u);
  ASSERT_TRUE(second.Delete("my news", ""));
  EXPECT_TRUE(second.Summaries().empty());
  EXPECT_EQ(first.Summaries().size(), 1u);
  EXPECT_FALSE(second.Load("", "voice_1"));
  EXPECT_FALSE(second.Delete("", "voice_1"));
  EXPECT_FALSE(second.ConfirmSites("voice_1", {"www.youtube.com"}));
  second.DeleteAll();
  EXPECT_EQ(first.Summaries().size(), 1u);
}

TEST_F(VoiceRoutineStoreTest, ANameInUseIsReplacedOnlyWhenAsked) {
  VoiceRoutineStore store = Profile1();
  ASSERT_TRUE(store.Save("voice_1", "my news", MyNews(), false, kNow).saved);
  RoutineSaveResult again =
      store.Save("voice_2", "The News", Pause(), false, kNow + 5);
  EXPECT_FALSE(again.saved);
  EXPECT_EQ(again.said, "You already have a routine called The News.");
  ASSERT_EQ(store.Summaries().size(), 1u);

  RoutineSaveResult replaced =
      store.Save("voice_2", "The News", Pause(), true, kNow + 5);
  ASSERT_TRUE(replaced.saved);
  EXPECT_TRUE(replaced.replaced);
  const std::vector<RoutineSummary> routines = store.Summaries();
  ASSERT_EQ(routines.size(), 1u);
  EXPECT_EQ(routines[0].id, "voice_1");
  EXPECT_EQ(routines[0].name, "The News");
  std::optional<RoutineToRun> run = store.Load("news", "");
  ASSERT_TRUE(run);
  ASSERT_EQ(run->steps.size(), 2u);
  EXPECT_EQ(run->steps[1].command.kind, VoiceCommandKind::kPause);

  // No routine of the name: "replace" saves a new one.
  ASSERT_TRUE(store.Save("voice_3", "trains", Pause(), true, kNow).saved);
  EXPECT_EQ(store.Summaries().size(), 2u);
}

TEST_F(VoiceRoutineStoreTest, ANameTwoRoutinesAnswerToRunsNeither) {
  VoiceRoutineStore store = Profile1();
  ASSERT_TRUE(store.Save("voice_1", "my news", MyNews(), false, kNow).saved);
  // A second file of the name, as an edit of the directory would leave it.
  {
    BuiltRoutine built = BuildRoutine("voice_2", "The News", Pause(), kNow);
    ASSERT_TRUE(built.script);
    ASSERT_TRUE(store.storage().Save(*built.script));
  }
  EXPECT_EQ(store.Summaries().size(), 2u);
  EXPECT_FALSE(store.Load("news", ""));
  EXPECT_FALSE(store.Delete("news", ""));
  RoutineSaveResult save = store.Save("voice_3", "news", Pause(), true, kNow);
  EXPECT_FALSE(save.saved);
  EXPECT_EQ(save.said,
            "You have 2 routines called news. Choose another name, or rename "
            "them in Agent mode.");
  // Each is still reached by its id (the Agent studio, settings).
  EXPECT_TRUE(store.Load("", "voice_2"));
  ASSERT_TRUE(store.Delete("", "voice_2"));
  EXPECT_TRUE(store.Load("news", ""));
}

TEST_F(VoiceRoutineStoreTest, OldRoutinesAreKeptByOneProfileOnce) {
  SaveOld("voice_1700000000_1", "my news");
  // An ordinary workflow beside it is nobody's to keep.
  Script workflow;
  workflow.id = "wf_1";
  workflow.name = "Prices";
  ASSERT_TRUE(shared_.Save(workflow));

  VoiceRoutineStore first(ProfileDir("Default"), shared_root_);
  VoiceRoutineStore second(ProfileDir("Profile 2"), shared_root_);
  // Offered to both, the profile's own to neither.
  EXPECT_EQ(Names(first.Unclaimed()), std::vector<std::string>{"my news"});
  EXPECT_EQ(Names(second.Unclaimed()), std::vector<std::string>{"my news"});
  EXPECT_TRUE(first.Summaries().empty());
  EXPECT_FALSE(first.Load("my news", ""));
  EXPECT_FALSE(first.Delete("", "voice_1700000000_1"));

  ClaimResult kept = first.Claim({"voice_1700000000_1", "wf_1", "gone"});
  ASSERT_EQ(kept.kept.size(), 1u);
  EXPECT_EQ(kept.not_kept, 2);
  EXPECT_EQ(kept.kept[0].id, "voice_1700000000_1");
  EXPECT_EQ(kept.kept[0].name, "my news");
  EXPECT_TRUE(kept.kept[0].renamed_from.empty());
  // Moved, as it was: Approved on its site, its steps.
  std::optional<Script> moved = first.storage().Load("voice_1700000000_1");
  ASSERT_TRUE(moved);
  EXPECT_EQ(moved->security.trust, TrustLevel::APPROVED);
  EXPECT_FALSE(moved->security.sites_unconfirmed);
  EXPECT_EQ(moved->steps.size(), 2u);
  std::optional<RoutineToRun> run = first.Load("my news", "");
  ASSERT_TRUE(run);
  EXPECT_TRUE(run->ok);
  EXPECT_FALSE(run->needs_sites_confirmed);
  // Gone for everyone else; the workflow stays shared.
  EXPECT_TRUE(second.Unclaimed().empty());
  EXPECT_TRUE(first.Unclaimed().empty());
  EXPECT_EQ(shared_.ListIds(), std::vector<std::string>{"wf_1"});
  // The other profile can't keep it any more.
  ClaimResult late = second.Claim({"voice_1700000000_1"});
  EXPECT_TRUE(late.kept.empty());
  EXPECT_EQ(late.not_kept, 1);
  EXPECT_TRUE(second.Summaries().empty());
}

TEST_F(VoiceRoutineStoreTest, AKeptRoutineIsRenamedBesideTheProfiles) {
  VoiceRoutineStore store = Profile1();
  ASSERT_TRUE(store.Save("voice_1", "my news", Pause(), false, kNow).saved);
  // The same id and name, from before (two profiles saved at once).
  SaveOld("voice_1", "My News");
  SaveOld("voice_9", "trains");
  ClaimResult kept = store.Claim({"voice_1", "voice_9"});
  ASSERT_EQ(kept.kept.size(), 2u);
  EXPECT_EQ(kept.kept[0].name, "My News (2)");
  EXPECT_EQ(kept.kept[0].renamed_from, "My News");
  EXPECT_NE(kept.kept[0].id, "voice_1");
  EXPECT_EQ(kept.kept[1].name, "trains");
  // The profile's own is untouched, and each name runs exactly one.
  std::optional<RoutineToRun> own = store.Load("", "voice_1");
  ASSERT_TRUE(own);
  EXPECT_EQ(own->name, "my news");
  EXPECT_EQ(own->steps.size(), 2u);
  std::optional<RoutineToRun> copy = store.Load("my news two", "");
  ASSERT_TRUE(copy);
  EXPECT_EQ(copy->id, kept.kept[0].id);
  EXPECT_EQ(store.Summaries().size(), 3u);
}

TEST_F(VoiceRoutineStoreTest, FilesThatClaimAnotherProfileChangeNothing) {
  // A routine file that says it is another profile's, in the shared
  // storage: offered like any other, kept only on this profile's word.
  BuiltRoutine built = BuildRoutine("voice_5", "their news", MyNews(), kNow);
  ASSERT_TRUE(built.script);
  base::DictValue json = built.script->ToJSON();
  json.Set("profile", "Profile 2");
  json.Set("owner", "1f2e3d4c5b6a");
  ASSERT_TRUE(shared_.EnsureDirectory());
  const base::FilePath shared_dir = shared_root_.AppendASCII("automations");
  std::string text;
  ASSERT_TRUE(base::JSONWriter::Write(json, &text));
  ASSERT_TRUE(base::WriteFile(shared_dir.AppendASCII("voice_5.molt"), text));
  VoiceRoutineStore first(ProfileDir("Default"), shared_root_);
  VoiceRoutineStore second(ProfileDir("Profile 2"), shared_root_);
  EXPECT_TRUE(second.Summaries().empty());
  EXPECT_EQ(first.Unclaimed().size(), 1u);
  EXPECT_EQ(second.Unclaimed().size(), 1u);

  // The same file in a profile's directory is that profile's, whatever it
  // says, and the other profile never sees it.
  ASSERT_TRUE(base::CreateDirectory(ProfileDir("Default")));
  ASSERT_TRUE(
      base::WriteFile(ProfileDir("Default").AppendASCII("voice_6.molt"), text));
  EXPECT_EQ(Names(first.Summaries()), std::vector<std::string>{"their news"});
  EXPECT_TRUE(second.Summaries().empty());
  EXPECT_FALSE(second.Load("their news", ""));

  // Not a routine (no voice step), or not JSON: never listed as one, nor
  // deleted with "forget everything".
  Script workflow;
  workflow.id = "wf_2";
  workflow.name = "Prices";
  ASSERT_TRUE(first.storage().Save(workflow));
  ASSERT_TRUE(base::WriteFile(ProfileDir("Default").AppendASCII("x.molt"),
                              "{\"name\": \"x\", \"steps\": 7"));
  EXPECT_EQ(first.Summaries().size(), 1u);
  first.DeleteAll();
  EXPECT_TRUE(first.Summaries().empty());
  EXPECT_TRUE(first.storage().Load("wf_2"));
}

TEST_F(VoiceRoutineStoreTest, NamesForWhatTheStudioBringsIn) {
  VoiceRoutineStore store = Profile1();
  ASSERT_TRUE(store.Save("voice_1", "my news", MyNews(), false, kNow).saved);
  BuiltRoutine built = BuildRoutine("wf_7", "My News", Pause(), kNow);
  ASSERT_TRUE(built.script);
  EXPECT_EQ(store.NameFor(*built.script), "My News (2)");
  // Its own name, when it is the routine being edited.
  built.script->id = "voice_1";
  EXPECT_EQ(store.NameFor(*built.script), "My News");
}

// --- A claim cut short (round 44) ------------------------------------------
// A claim: Take() renames <id>.molt to <id>.molt.taken (1), the routine is
// saved in the profile (2, a temporary file then a rename), FinishTake()
// deletes the set-aside file (3). The browser may end between any two.

// What a claim set aside, as the browser left it when it ended |ago|.
void LeaveSetAside(const base::FilePath& shared_dir,
                   const std::string& id,
                   const std::string& contents,
                   base::TimeDelta ago) {
  const base::FilePath taken = shared_dir.AppendASCII(id + ".molt.taken");
  ASSERT_TRUE(base::WriteFile(taken, contents));
  const base::Time when = base::Time::Now() - ago;
  ASSERT_TRUE(base::TouchFile(taken, when, when));
}

std::string Contents(const base::FilePath& path) {
  std::string contents;
  EXPECT_TRUE(base::ReadFileToString(path, &contents));
  return contents;
}

TEST_F(VoiceRoutineStoreTest, IsClaimedCopyComparesAllButNameAndId) {
  BuiltRoutine built = BuildRoutine("voice_1", "my news", MyNews(), kNow);
  ASSERT_TRUE(built.script);
  const Script& set_aside = *built.script;
  BuiltRoutine again = BuildRoutine("voice_1__", "my news (2)", MyNews(), kNow);
  ASSERT_TRUE(again.script);
  EXPECT_TRUE(IsClaimedCopy(*again.script, set_aside));
  again.script->id = "voice_1";
  EXPECT_TRUE(IsClaimedCopy(*again.script, set_aside));
  // Another id, a step, a time, a site or the trust: another routine.
  again.script->id = "voice_10";
  EXPECT_FALSE(IsClaimedCopy(*again.script, set_aside));
  again.script->id = "voice";
  EXPECT_FALSE(IsClaimedCopy(*again.script, set_aside));
  BuiltRoutine other = BuildRoutine("voice_1", "my news", Pause(), kNow);
  ASSERT_TRUE(other.script);
  EXPECT_FALSE(IsClaimedCopy(*other.script, set_aside));
  BuiltRoutine later = BuildRoutine("voice_1", "my news", MyNews(), kNow + 1);
  ASSERT_TRUE(later.script);
  EXPECT_FALSE(IsClaimedCopy(*later.script, set_aside));
  BuiltRoutine casual = BuildRoutine("voice_1", "my news", MyNews(), kNow);
  ASSERT_TRUE(casual.script);
  casual.script->security.trust = TrustLevel::CASUAL;
  EXPECT_FALSE(IsClaimedCopy(*casual.script, set_aside));
}

// Cut short after (1), or during (2): the profile doesn't have it. Put back
// in the shared storage, offered again, and claimed as ever.
TEST_F(VoiceRoutineStoreTest, AClaimCutShortBeforeItsSaveIsPutBack) {
  SaveOld("voice_1700000000_1", "my news");
  const base::FilePath shared_dir = shared_root_.AppendASCII("automations");
  const base::FilePath file = shared_dir.AppendASCII("voice_1700000000_1.molt");
  const std::string original = Contents(file);
  ASSERT_TRUE(base::DeleteFile(file));
  LeaveSetAside(shared_dir, "voice_1700000000_1", original, base::Minutes(5));
  // (2): a temporary file of the save that never finished.
  ASSERT_TRUE(base::CreateDirectory(ProfileDir("Default")));
  ASSERT_TRUE(base::WriteFile(
      ProfileDir("Default").AppendASCII(".org.chromium.Chromium.cut123"),
      original.substr(0, original.size() / 2)));
  // Neither storage lists it until it is settled.
  EXPECT_TRUE(shared_.ListIds().empty());

  VoiceRoutineStore store = Profile1();
  EXPECT_TRUE(store.Summaries().empty());
  const std::vector<RoutineSummary> offered = store.Unclaimed();
  ASSERT_EQ(Names(offered), std::vector<std::string>{"my news"});
  EXPECT_EQ(offered[0].id, "voice_1700000000_1");
  // As it was, never in this profile without a "keep".
  EXPECT_EQ(Contents(file), original);
  EXPECT_FALSE(base::PathExists(
      shared_dir.AppendASCII("voice_1700000000_1.molt.taken")));
  EXPECT_TRUE(store.Summaries().empty());
  // Another profile is offered it too, and a claim keeps it once.
  VoiceRoutineStore second(ProfileDir("Profile 2"), shared_root_);
  EXPECT_EQ(Names(second.Unclaimed()), std::vector<std::string>{"my news"});
  ClaimResult kept = store.Claim({"voice_1700000000_1"});
  ASSERT_EQ(kept.kept.size(), 1u);
  EXPECT_TRUE(second.Unclaimed().empty());
}

// Cut short after (2): the profile has it, as the claim saved it (here
// renamed and with another id, beside a routine of its name and id). The
// set-aside file is deleted, as (3) would have; it isn't offered again.
TEST_F(VoiceRoutineStoreTest, AClaimCutShortAfterItsSaveIsFinished) {
  VoiceRoutineStore store = Profile1();
  ASSERT_TRUE(store.Save("voice_1", "my news", Pause(), false, kNow).saved);
  SaveOld("voice_1", "My News", kNow - 100);
  const base::FilePath shared_dir = shared_root_.AppendASCII("automations");
  const std::string original = Contents(shared_dir.AppendASCII("voice_1.molt"));
  ClaimResult kept = store.Claim({"voice_1"});
  ASSERT_EQ(kept.kept.size(), 1u);
  EXPECT_EQ(kept.kept[0].id, "voice_1_");
  EXPECT_EQ(kept.kept[0].name, "My News (2)");
  // As the browser would have left it had it ended before (3).
  LeaveSetAside(shared_dir, "voice_1", original, base::Minutes(5));

  EXPECT_TRUE(store.Unclaimed().empty());
  EXPECT_FALSE(base::PathExists(shared_dir.AppendASCII("voice_1.molt.taken")));
  EXPECT_TRUE(shared_.ListIds().empty());
  EXPECT_EQ(store.Summaries().size(), 2u);
  // Settled once: nothing more to do.
  EXPECT_EQ(store.RecoverInterruptedClaims(), 0);
}

// Cut short after (2), but the profile's copy changed since (or is gone):
// not the routine the claim saved. Put back, offered again; the profile's
// own stays as it is.
TEST_F(VoiceRoutineStoreTest, ACopyChangedSinceIsNotTheClaimedRoutine) {
  SaveOld("voice_2", "trains");
  const base::FilePath shared_dir = shared_root_.AppendASCII("automations");
  const std::string original = Contents(shared_dir.AppendASCII("voice_2.molt"));
  VoiceRoutineStore store = Profile1();
  ASSERT_EQ(store.Claim({"voice_2"}).kept.size(), 1u);
  std::optional<Script> copy = store.storage().Load("voice_2");
  ASSERT_TRUE(copy);
  copy->steps.pop_back();
  ASSERT_TRUE(store.storage().Save(*copy));
  LeaveSetAside(shared_dir, "voice_2", original, base::Minutes(5));

  EXPECT_EQ(Names(store.Unclaimed()), std::vector<std::string>{"trains"});
  ASSERT_EQ(store.Summaries().size(), 1u);
  EXPECT_EQ(store.Summaries()[0].steps, 1);
}

// A set-aside file that may be another browser's move in progress (set
// aside within the grace period), or this browser's: left alone.
TEST_F(VoiceRoutineStoreTest, AMoveInProgressIsLeftAlone) {
  SaveOld("voice_3", "mail");
  SaveOld("voice_4", "weather");
  const base::FilePath shared_dir = shared_root_.AppendASCII("automations");
  const base::FilePath file3 = shared_dir.AppendASCII("voice_3.molt");
  const std::string original = Contents(file3);
  ASSERT_TRUE(base::DeleteFile(file3));
  LeaveSetAside(shared_dir, "voice_3", original, base::Seconds(0));
  // This browser's own move, however old its file looks.
  ASSERT_TRUE(shared_.Take("voice_4"));
  const base::FilePath taken4 = shared_dir.AppendASCII("voice_4.molt.taken");
  const base::Time old = base::Time::Now() - base::Minutes(10);
  ASSERT_TRUE(base::TouchFile(taken4, old, old));

  VoiceRoutineStore store = Profile1();
  EXPECT_TRUE(store.Unclaimed().empty());
  EXPECT_TRUE(base::PathExists(shared_dir.AppendASCII("voice_3.molt.taken")));
  EXPECT_TRUE(base::PathExists(taken4));
  ASSERT_TRUE(shared_.ReturnTaken("voice_4"));
  EXPECT_EQ(Names(store.Unclaimed()), std::vector<std::string>{"weather"});
}

// What the browser set aside but isn't a routine (unreadable, or not a
// voice routine) is put back as it was: listed or skipped as before.
TEST_F(VoiceRoutineStoreTest, ASetAsideFileThatIsntARoutineIsPutBack) {
  ASSERT_TRUE(shared_.EnsureDirectory());
  const base::FilePath shared_dir = shared_root_.AppendASCII("automations");
  LeaveSetAside(shared_dir, "broken",
                "{\"id\": \"broken\", \"steps\": ", base::Minutes(5));
  Script workflow;
  workflow.id = "wf_3";
  workflow.name = "Prices";
  LeaveSetAside(shared_dir, "wf_3", workflow.ToJSONString(), base::Minutes(5));
  VoiceRoutineStore store = Profile1();
  EXPECT_TRUE(store.Unclaimed().empty());
  EXPECT_EQ(Contents(shared_dir.AppendASCII("broken.molt")),
            "{\"id\": \"broken\", \"steps\": ");
  EXPECT_EQ(shared_.ListIds().size(), 2u);
  EXPECT_TRUE(shared_.Load("wf_3"));
  EXPECT_TRUE(store.Summaries().empty());
}

// The profile's routines on one sequence: the Agent studio's change and
// voice mode's, posted together, never interleave (a routine named twice
// is renamed "(2)", never saved twice under one name).
TEST_F(VoiceRoutineStoreTest, TwoWritersOnTheProfilesSequence) {
  base::test::TaskEnvironment task_environment;
  scoped_refptr<base::SequencedTaskRunner> sequence =
      base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()});
  const base::FilePath dir = ProfileDir("Default");
  const base::FilePath root = shared_root_;
  constexpr int kEach = 10;
  for (int i = 0; i < kEach; ++i) {
    // Voice mode: "remember this as my news" (refused once one exists).
    sequence->PostTask(FROM_HERE,
                       base::BindOnce(
                           [](base::FilePath dir, base::FilePath root, int i) {
                             VoiceRoutineStore store(dir, root);
                             store.Save("voice_" + base::NumberToString(i),
                                        "my news", MyNews(), false, kNow + i);
                           },
                           dir, root, i));
    // The studio: an import of "my news", renamed beside the others.
    sequence->PostTask(FROM_HERE,
                       base::BindOnce(
                           [](base::FilePath dir, base::FilePath root, int i) {
                             VoiceRoutineStore store(dir, root);
                             BuiltRoutine built =
                                 BuildRoutine("wf_" + base::NumberToString(i),
                                              "my news", Pause(), kNow);
                             ASSERT_TRUE(built.script);
                             built.script->name = store.NameFor(*built.script);
                             ASSERT_FALSE(built.script->name.empty());
                             ASSERT_TRUE(store.storage().Save(*built.script));
                           },
                           dir, root, i));
  }
  base::RunLoop run_loop;
  sequence->PostTask(FROM_HERE, run_loop.QuitClosure());
  run_loop.Run();
  VoiceRoutineStore store(dir, root);
  std::vector<std::string> names = Names(store.Summaries());
  ASSERT_EQ(names.size(), static_cast<size_t>(kEach + 1));
  for (const std::string& name : names) {
    EXPECT_EQ(MatchRoutineName(names, name).count, 1) << name;
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
