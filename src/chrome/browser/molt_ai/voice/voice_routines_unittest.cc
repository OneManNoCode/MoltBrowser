// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Routines taught by voice as workflows (voice_routines.h): the steps and
// their descriptions, what is withheld or refused, the trust each kind of
// routine starts with, a round trip through the .molt format, and a hostile
// file read back.

#include "chrome/browser/molt_ai/voice/voice_routines.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using automation::Script;
using automation::StepType;
using automation::TrustLevel;

constexpr int64_t kNow = 1'800'000'000;

RoutineStep Said(std::string_view said,
                 std::string_view host = "www.youtube.com",
                 int position = 0) {
  ParseContext context;
  std::optional<RoutineStep> step = RoutineStepFor(
      ParseVoiceCommand(said, context), host, position, {}, false);
  EXPECT_TRUE(step) << said;
  return step.value_or(RoutineStep());
}

std::vector<RoutineStep> MyNews() {
  RoutineStep open = Said("open YouTube", "");
  open.url = "https://www.youtube.com/";
  return {open, Said("search for news"), Said("play the first one", "", 1)};
}

TEST(VoiceRoutinesTest, MyNewsBecomesAnApprovedWorkflow) {
  BuiltRoutine built = BuildRoutine("voice_1", "my news", MyNews(), kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  const Script& script = *built.script;
  EXPECT_EQ(script.id, "voice_1");
  EXPECT_EQ(script.name, "my news");
  EXPECT_EQ(script.created_at_unix, kNow);
  ASSERT_EQ(script.steps.size(), 3u);
  EXPECT_EQ(script.steps[0].type, StepType::NAVIGATE);
  EXPECT_EQ(script.steps[0].target, "https://www.youtube.com/");
  EXPECT_EQ(script.steps[0].description, "Open YouTube");
  EXPECT_EQ(script.steps[1].type, StepType::TYPE);
  EXPECT_EQ(script.steps[1].description, "Search for news");
  EXPECT_EQ(script.steps[2].type, StepType::CLICK);
  EXPECT_EQ(script.steps[2].description, "Play the first one");
  EXPECT_TRUE(automation::HasVoiceSteps(script));
  // The user said each step: Approved on the sites it used.
  EXPECT_EQ(script.security.trust, TrustLevel::APPROVED);
  EXPECT_FALSE(script.security.sites_unconfirmed);
  EXPECT_EQ(script.security.domain_whitelist,
            std::vector<std::string>{"www.youtube.com"});
  EXPECT_EQ(built.withheld, 0);
  // No page text, no selectors.
  for (const automation::Step& step : script.steps) {
    EXPECT_TRUE(step.selector_fallbacks.empty());
    EXPECT_FALSE(step.locators);
    EXPECT_TRUE(step.value.empty());
  }

  // Through the file and back.
  std::optional<Script> loaded = Script::FromJSONString(script.ToJSONString());
  ASSERT_TRUE(loaded);
  ReadRoutineResult read = ReadRoutine(*loaded);
  ASSERT_TRUE(read.ok) << read.why;
  EXPECT_FALSE(read.needs_sites_confirmed);
  ASSERT_EQ(read.steps.size(), 3u);
  EXPECT_EQ(read.steps[0].url, "https://www.youtube.com/");
  EXPECT_EQ(read.steps[0].command.site, "youtube");
  EXPECT_EQ(read.steps[1].command.text, "news");
  EXPECT_EQ(read.steps[1].host, "www.youtube.com");
  EXPECT_EQ(read.steps[2].command.choice.kind, ChoiceRef::Kind::kPosition);
  EXPECT_EQ(read.steps[2].command.choice.ordinal, 1);
  EXPECT_EQ(read.steps[2].command.verb, ChoiceVerb::kPlay);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(DescribeRoutineStep(read.steps[i]),
              DescribeRoutineStep(MyNews()[i]));
  }
}

TEST(VoiceRoutinesTest, EveryKindRoundTrips) {
  for (const char* said :
       {"open example dot com in a new tab", "search YouTube for cats",
        "search the web for trains", "search this site for lamps",
        "find opening hours on this page", "go back", "reload",
        "next page", "scroll down 3 pages", "scroll up a bit",
        "scroll to the top", "zoom in", "pause", "resume", "louder",
        "mute", "skip ahead 30 seconds", "go back 10 seconds",
        "next track", "full screen", "turn on captions", "read the page",
        "read the headings", "read the results", "the last one",
        "the second to last one", "play the live one by Queen",
        "open the one from the BBC", "type hello world"}) {
    const RoutineStep step = Said(said);
    BuiltRoutine built = BuildRoutine("voice_2", "everything", {step}, kNow);
    ASSERT_TRUE(built.script) << said << ": " << built.refusal;
    std::optional<Script> loaded =
        Script::FromJSONString(built.script->ToJSONString());
    ASSERT_TRUE(loaded) << said;
    ReadRoutineResult read = ReadRoutine(*loaded);
    ASSERT_TRUE(read.ok) << said << ": " << read.why;
    ASSERT_FALSE(read.steps.empty());
    EXPECT_EQ(read.steps.back().command.kind, step.command.kind) << said;
    EXPECT_EQ(DescribeRoutineStep(read.steps.back()),
              DescribeRoutineStep(step))
        << said;
  }
}

TEST(VoiceRoutinesTest, StartsOnTheSiteItWasSaidOn) {
  BuiltRoutine built = BuildRoutine(
      "voice_6", "news", {Said("search for news"), Said("pause")}, kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  ASSERT_EQ(built.script->steps.size(), 3u);
  EXPECT_EQ(built.script->steps[0].type, StepType::NAVIGATE);
  EXPECT_EQ(built.script->steps[0].target, "https://www.youtube.com/");
  EXPECT_EQ(built.script->steps[0].description, "Open www.youtube.com");
  ReadRoutineResult read = ReadRoutine(*built.script);
  ASSERT_TRUE(read.ok) << read.why;
  EXPECT_EQ(read.steps[0].command.kind, VoiceCommandKind::kOpenSite);
  EXPECT_EQ(read.steps[0].command.site, "www.youtube.com");
  EXPECT_TRUE(read.steps[0].command.site_is_address);
  // A step said on no web page: nothing to open first.
  built = BuildRoutine("voice_7", "news", {Said("pause", "")}, kNow);
  ASSERT_TRUE(built.script);
  EXPECT_EQ(built.script->steps.size(), 1u);
}

TEST(VoiceRoutinesTest, SecretsAreAskedForAtEachRun) {
  std::vector<RoutineStep> steps = MyNews();
  steps.push_back(Said("type hunter2!Secret"));
  steps.push_back(Said("search for 4111 1111 1111 1111"));
  BuiltRoutine built = BuildRoutine("voice_3", "my news", steps, kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  EXPECT_EQ(built.withheld, 2);
  const std::string json = built.script->ToJSONString();
  EXPECT_EQ(json.find("hunter2"), std::string::npos);
  EXPECT_EQ(json.find("4111"), std::string::npos);
  EXPECT_EQ(built.script->steps[3].value, "{{text}}");
  EXPECT_EQ(built.script->steps[3].description, "Type what you tell me");
  EXPECT_EQ(built.script->default_variables.count("text"), 1u);
  ReadRoutineResult read =
      ReadRoutine(*Script::FromJSONString(json));
  ASSERT_TRUE(read.ok) << read.why;
  EXPECT_EQ(read.steps[3].variable, "text");
  EXPECT_TRUE(read.steps[3].command.text.empty());
  EXPECT_EQ(read.steps[4].variable, "search");
}

TEST(VoiceRoutinesTest, RefusesWhatItCantKeep) {
  // A page address that holds a code.
  std::vector<RoutineStep> steps = MyNews();
  steps[0].url = "https://example.com/reset?token=abcdef0123456789abcdef";
  EXPECT_FALSE(BuildRoutine("v", "my news", steps, kNow).script);
  // Template text, which the studio would read as a variable.
  steps = MyNews();
  RoutineStep typed = Said("type hello");
  typed.command.text = "{{pin}}";
  steps.push_back(typed);
  EXPECT_FALSE(BuildRoutine("v", "my news", steps, kNow).script);
  // A name that isn't one, or looks secret.
  EXPECT_FALSE(BuildRoutine("v", "yes", MyNews(), kNow).script);
  EXPECT_FALSE(BuildRoutine("v", " my news ", MyNews(), kNow).script);
  EXPECT_FALSE(BuildRoutine("v", "hunter2!Secret", MyNews(), kNow).script);
  // Nothing, or too much.
  EXPECT_FALSE(BuildRoutine("v", "my news", {}, kNow).script);
  steps.assign(kMaxRoutineSteps + 1, Said("scroll down"));
  EXPECT_FALSE(BuildRoutine("v", "my news", steps, kNow).script);
}

TEST(VoiceRoutinesTest, ThePlannersStepsMakeADraft) {
  std::vector<RoutineStep> steps = MyNews();
  steps[2].planned = true;
  BuiltRoutine built = BuildRoutine("voice_4", "trains", steps, kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  const automation::SecurityPolicy& policy = built.script->security;
  EXPECT_EQ(policy.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(policy.sites_unconfirmed);
  EXPECT_TRUE(policy.domain_whitelist.empty());
  ReadRoutineResult read =
      ReadRoutine(*Script::FromJSONString(built.script->ToJSONString()));
  ASSERT_TRUE(read.ok);
  EXPECT_TRUE(read.needs_sites_confirmed);
  EXPECT_TRUE(read.steps[2].planned);
  // Once the user confirms its sites (a real click), it runs.
  Script confirmed = *Script::FromJSONString(built.script->ToJSONString());
  ASSERT_TRUE(automation::AutomationSecurity::ConfirmDraftSites(
      confirmed, automation::AutomationSecurity::HostsReferencedBy(
                     confirmed.steps)));
  EXPECT_FALSE(ReadRoutine(confirmed).needs_sites_confirmed);
}

TEST(VoiceRoutinesTest, AHostileFileIsntRunByVoice) {
  const BuiltRoutine built = BuildRoutine("voice_5", "my news", MyNews(), kNow);
  ASSERT_TRUE(built.script);
  auto with_voice = [&](auto&& change) {
    Script script = *Script::FromJSONString(built.script->ToJSONString());
    base::DictValue* voice =
        script.steps[1].extra.GetDict().FindDict(automation::kVoiceStepKey);
    change(*voice, script);
    return ReadRoutine(script);
  };
  // Each of these makes the routine one voice mode won't run.
  const std::vector<std::function<void(base::DictValue&, Script&)>> changes =
      {
          [](base::DictValue& v, Script&) { v.Set("command", "voice_mode_off"); },
          [](base::DictValue& v, Script&) { v.Set("command", "remember_routine"); },
          [](base::DictValue& v, Script&) { v.Set("command", "nonsense"); },
          [](base::DictValue& v, Script&) { v.Remove("command"); },
          [](base::DictValue& v, Script&) { v.Set("query", 5); },
          [](base::DictValue& v, Script&) { v.Set("query", std::string(600, 'a')); },
          [](base::DictValue& v, Script&) { v.Set("query", "a\nb"); },
          [](base::DictValue& v, Script&) { v.Set("query", "{{pin}}"); },
          [](base::DictValue& v, Script&) { v.Set("scope", "everywhere"); },
          [](base::DictValue& v, Script&) { v.Set("variable", "Bad Name"); },
          [](base::DictValue& v, Script&) { v.Set("planned", "yes"); },
          [](base::DictValue&, Script& s) { s.steps[1].type = StepType::RUN_JS; },
          [](base::DictValue&, Script& s) {
            s.steps[0].target = "javascript:alert(1)";
          },
          [](base::DictValue&, Script& s) {
            s.steps.push_back(automation::Step());
          },
          [](base::DictValue&, Script& s) { s.steps.clear(); },
      };
  for (size_t i = 0; i < changes.size(); ++i) {
    const ReadRoutineResult read = with_voice(changes[i]);
    EXPECT_FALSE(read.ok) << i;
    EXPECT_TRUE(read.steps.empty()) << i;
    EXPECT_FALSE(read.why.empty()) << i;
  }
  // A choice: words must be folded words, within the cap.
  Script script = *Script::FromJSONString(built.script->ToJSONString());
  base::DictValue* choose =
      script.steps[2].extra.GetDict().FindDict(automation::kVoiceStepKey);
  choose->Set("by", "name");
  base::ListValue words;
  words.Append("Queen");  // not folded
  choose->Set("words", std::move(words));
  EXPECT_FALSE(ReadRoutine(script).ok);
  base::ListValue many;
  for (int i = 0; i < 20; ++i) {
    many.Append("word");
  }
  choose->Set("words", std::move(many));
  EXPECT_FALSE(ReadRoutine(script).ok);
  base::ListValue fine;
  fine.Append("queen");
  choose->Set("words", std::move(fine));
  EXPECT_TRUE(ReadRoutine(script).ok);
  // A workflow the studio recorded isn't a voice routine.
  Script recorded;
  recorded.id = "wf_1";
  automation::Step click;
  click.type = StepType::CLICK;
  click.target = "#go";
  recorded.steps.push_back(std::move(click));
  EXPECT_FALSE(automation::HasVoiceSteps(recorded));
  EXPECT_FALSE(ReadRoutine(recorded).ok);
}

TEST(VoiceRoutinesTest, AnImportedRoutineRunsOnlyOnConfirmedSites) {
  // A file that claims to be a routine the user taught, saved as an
  // import or a copy is (molt://ai-automation, the Agent studio).
  const BuiltRoutine built = BuildRoutine("voice_6", "my news", MyNews(), kNow);
  ASSERT_TRUE(built.script);
  Script imported = *Script::FromJSONString(built.script->ToJSONString());
  imported.steps[0].target = "https://evil.example/";
  automation::AutomationSecurity::ClampTrustForSave(imported, nullptr);
  const ReadRoutineResult read = ReadRoutine(imported);
  ASSERT_TRUE(read.ok);
  EXPECT_TRUE(read.needs_sites_confirmed);
  const automation::SecurityPolicy policy =
      automation::AutomationSecurity::EffectivePolicy(imported);
  EXPECT_FALSE(automation::AutomationSecurity().AllowNavigation(
      policy, "https://evil.example/"));
  // What voice mode saved itself runs as it is.
  EXPECT_FALSE(ReadRoutine(*built.script).needs_sites_confirmed);
}

TEST(VoiceRoutinesTest, SitesAreConfirmedByVoiceOnlyWhenAllAreSaid) {
  EXPECT_TRUE(RoutineSitesSayable({"www.youtube.com"}));
  EXPECT_TRUE(RoutineSitesSayable({"news.example", "shop.example"}));
  EXPECT_FALSE(RoutineSitesSayable({}));
  // A site the prompt would shorten, or couldn't say.
  EXPECT_FALSE(RoutineSitesSayable(
      {"news.example", std::string(90, 'a') + ".evil.example"}));
  EXPECT_FALSE(RoutineSitesSayable({"news.example", "[::1]"}));
  EXPECT_FALSE(RoutineSitesSayable({"news.example", "!!!"}));
  // More than the prompt says of its text: the last ones would go unsaid.
  std::vector<std::string> many;
  for (int i = 0; i < 20; ++i) {
    many.push_back("site-number-" + std::to_string(i) + ".example");
  }
  EXPECT_FALSE(RoutineSitesSayable(many));
  many.resize(5);
  EXPECT_TRUE(RoutineSitesSayable(many));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
