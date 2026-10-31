// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A profile's voice routines (voice_routine_store.h): each profile lists,
// runs, confirms and deletes only its own; a name in use is replaced only
// when asked, and a name two routines answer to runs neither; routines
// from before profiles are kept by one profile at most, as they were,
// renamed beside the profile's own; files whose contents say they are
// another profile's change nothing.

#include "chrome/browser/molt_ai/voice/voice_routine_store.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
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

}  // namespace
}  // namespace voice
}  // namespace molt_ai
