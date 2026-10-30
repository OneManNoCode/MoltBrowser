// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Routines taught by voice as workflows (voice_routines.h): the steps and
// their descriptions, what is withheld or refused, the trust each kind of
// routine starts with, a round trip through the .molt format, and a hostile
// file read back; the on-device planner's steps kept as an agent draft keeps
// them, and the sites a routine with them is confirmed for.

#include "chrome/browser/molt_ai/voice/voice_routines.h"

#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"
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

using Action = PlannerStep::Action;

constexpr char kGoal[] = "find me a train from Paris to Lyon";
constexpr char kSearchUrl[] =
    "https://search.brave.com/search?q=trains+to+lyon";

PlannerStep Navigate(std::string url) {
  PlannerStep step;
  step.action = Action::kNavigate;
  step.url = std::move(url);
  return step;
}

PlannerStep Search(std::string query, std::string url = kSearchUrl) {
  PlannerStep step;
  step.action = Action::kSearch;
  step.query = std::move(query);
  step.url = std::move(url);
  return step;
}

PlannerStep Element(Action action,
                    std::string kind,
                    std::string label,
                    std::string host = "trains.example") {
  PlannerStep step;
  step.action = action;
  step.kind = std::move(kind);
  step.label = std::move(label);
  step.host = std::move(host);
  return step;
}

PlannerStep Typed(std::string label, std::string text) {
  PlannerStep step = Element(Action::kType, "input", std::move(label));
  step.text = std::move(text);
  return step;
}

RoutineStep Kept(const PlannerStep& step, std::string_view goal = kGoal) {
  return RoutineStepForPlanner(step, goal, "trains.example");
}

// The planner's run: a web search, the site, the form, the button, a
// scroll.
std::vector<RoutineStep> Trains() {
  PlannerStep select = Element(Action::kSelect, "select", "Class");
  select.option = "First class";
  PlannerStep scroll;
  scroll.action = Action::kScroll;
  scroll.direction = "down";
  return {Kept(Search("trains to lyon")),
          Kept(Navigate("https://trains.example/book")),
          Kept(Typed("To", "Lyon")),
          Kept(select),
          Kept(Element(Action::kClick, "button", "Search")),
          Kept(scroll)};
}

// A workflow isn't copyable (its steps' extra values): through its file.
Script Copy(const Script& script) {
  return *Script::FromJSONString(script.ToJSONString());
}

base::DictValue& VoiceOf(automation::Step& step) {
  return *step.extra.GetDict().FindDict(automation::kVoiceStepKey);
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

TEST(VoiceRoutinesTest, ThePlannersStepsAreKeptAsADraftKeepsThem) {
  const std::vector<RoutineStep> steps = Trains();
  std::vector<std::string> said;
  for (const RoutineStep& step : steps) {
    EXPECT_TRUE(IsAgentStep(step));
    EXPECT_FALSE(IsUnkeptStep(step)) << step.agent.why;
    said.push_back(DescribeRoutineStep(step));
  }
  EXPECT_EQ(JoinSpokenList(said),
            "search the web for trains to lyon, open trains.example, type "
            "Lyon in To, choose First class in Class, click Search and "
            "scroll down");

  BuiltRoutine built = BuildRoutine("voice_8", "my trains", steps, kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  const Script& script = *built.script;
  // The first step opens a page: nothing is opened before it.
  ASSERT_EQ(script.steps.size(), 6u);
  const StepType types[] = {StepType::NAVIGATE, StepType::NAVIGATE,
                            StepType::TYPE,     StepType::CLICK,
                            StepType::CLICK,    StepType::SCROLL};
  const char* descriptions[] = {
      "Search the web for \xE2\x80\x9Ctrains to lyon\xE2\x80\x9D",
      "Open trains.example",
      "Type \xE2\x80\x9CLyon\xE2\x80\x9D in \xE2\x80\x9CTo\xE2\x80\x9D",
      "Choose \xE2\x80\x9C"
      "First class\xE2\x80\x9D in "
      "\xE2\x80\x9C"
      "Class\xE2\x80\x9D",
      "Click \xE2\x80\x9CSearch\xE2\x80\x9D",
      "Scroll down"};
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(script.steps[i].type, types[i]) << i;
    EXPECT_EQ(script.steps[i].description, descriptions[i]) << i;
    EXPECT_TRUE(automation::IsVoiceStep(script.steps[i])) << i;
    // No selector, nothing the runner could replay.
    EXPECT_TRUE(script.steps[i].selector_fallbacks.empty());
    EXPECT_FALSE(script.steps[i].locators);
  }
  EXPECT_EQ(script.steps[0].target, kSearchUrl);
  EXPECT_EQ(script.steps[1].target, "https://trains.example/book");
  EXPECT_TRUE(script.steps[2].target.empty());
  EXPECT_EQ(script.steps[5].value, "down");
  // A draft's rules: Casual, no sites, until the user confirms them.
  EXPECT_EQ(script.security.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(script.security.sites_unconfirmed);
  EXPECT_TRUE(script.security.domain_whitelist.empty());
  EXPECT_EQ(automation::AutomationSecurity::HostsReferencedBy(script.steps),
            (std::vector<std::string>{"search.brave.com", "trains.example"}));
  EXPECT_FALSE(RoutineSitesConfirmed(script));

  // Through the file and back: the same steps, as untrusted.
  std::optional<Script> loaded = Script::FromJSONString(script.ToJSONString());
  ASSERT_TRUE(loaded);
  ReadRoutineResult read = ReadRoutine(*loaded);
  ASSERT_TRUE(read.ok) << read.why;
  EXPECT_TRUE(read.needs_sites_confirmed);
  ASSERT_EQ(read.steps.size(), steps.size());
  for (size_t i = 0; i < steps.size(); ++i) {
    const RoutineStep& a = read.steps[i];
    const RoutineStep& b = steps[i];
    EXPECT_TRUE(a.planned) << i;
    EXPECT_EQ(a.agent.action, b.agent.action) << i;
    EXPECT_EQ(a.agent.kind, b.agent.kind) << i;
    EXPECT_EQ(a.agent.label, b.agent.label) << i;
    EXPECT_EQ(a.agent.text, b.agent.text) << i;
    EXPECT_EQ(a.agent.option, b.agent.option) << i;
    EXPECT_EQ(a.agent.direction, b.agent.direction) << i;
    EXPECT_EQ(a.agent.query, b.agent.query) << i;
    EXPECT_EQ(a.url, b.url) << i;
    EXPECT_EQ(a.host, b.host) << i;
    EXPECT_EQ(DescribeRoutineStep(a), DescribeRoutineStep(b)) << i;
  }

  // Confirmed (the voice gesture or a click in the Agent studio), it runs.
  ASSERT_TRUE(automation::AutomationSecurity::ConfirmDraftSites(
      *loaded, {"trains.example", "search.brave.com"}));
  EXPECT_TRUE(RoutineSitesConfirmed(*loaded));
  EXPECT_FALSE(ReadRoutine(*loaded).needs_sites_confirmed);
}

TEST(VoiceRoutinesTest, ThePlannersStepsAreCheckedAsADraftChecksThem) {
  // Typed text that looks secret, by the field's name, the text, or the
  // request: withheld, asked for at each run.
  for (const auto& [label, text, goal] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"PIN", "4321", kGoal},
           {"Pass\xE2\x80\x8Bword", "letmein", kGoal},
           {"Message", "hunter2!Secret9", kGoal},
           {"Promo code", "SAVE20", "book it with code SAVE20"},
           {"Name", "Ada", std::string(5000, 'a')},
       }) {
    const RoutineStep step = Kept(Typed(label, text), goal);
    ASSERT_TRUE(IsAgentStep(step)) << label;
    EXPECT_FALSE(IsUnkeptStep(step)) << label;
    EXPECT_FALSE(step.variable.empty()) << label;
    EXPECT_TRUE(step.agent.text.empty()) << label;
    EXPECT_EQ(DescribeRoutineStep(step).rfind("type what you tell me in", 0),
              0u)
        << label;
  }
  const RoutineStep plain = Kept(Typed("To", "Lyon"));
  EXPECT_TRUE(plain.variable.empty());
  EXPECT_EQ(plain.agent.text, "Lyon");
  // Withheld text never reaches the file; two such fields, two names.
  BuiltRoutine built =
      BuildRoutine("voice_9", "my trains",
                   {Kept(Navigate("https://trains.example/")),
                    Kept(Typed("PIN", "4321")), Kept(Typed("PIN", "9876"))},
                   kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  EXPECT_EQ(built.withheld, 2);
  const std::string json = built.script->ToJSONString();
  EXPECT_EQ(json.find("4321"), std::string::npos);
  EXPECT_EQ(json.find("9876"), std::string::npos);
  EXPECT_EQ(built.script->steps[1].value, "{{pin}}");
  EXPECT_EQ(built.script->steps[2].value, "{{pin_2}}");
  ReadRoutineResult read = ReadRoutine(*Script::FromJSONString(json));
  ASSERT_TRUE(read.ok) << read.why;
  EXPECT_EQ(read.steps[2].variable, "pin_2");

  // What a routine can't repeat faithfully is a gap, saying why.
  PlannerStep card = Typed("Card", "x");
  card.credential_field = true;
  PlannerStep password = Typed("Secret", "x");
  password.input_type = "Password";
  PlannerStep left;
  left.action = Action::kScroll;
  left.direction = "left";
  PlannerStep card_option = Element(Action::kSelect, "select", "Card");
  card_option.option = "4111 1111 1111 1111";
  PlannerStep tool;
  tool.action = Action::kUnkept;
  tool.why = "the AI used a site's own tool.";
  const std::vector<std::pair<PlannerStep, std::string>> gaps = {
      {card, "password, card or code field"},
      {password, "password, card or code field"},
      {Element(Action::kClick, "button", ""), "without a name"},
      {Element(Action::kClick, "button", "{{pin}}"), "can't be repeated"},
      {Element(Action::kClick, "button", "Go\nnow"), "can't be repeated"},
      {Element(Action::kClick, "button", std::string(61, 'a')),
       "can't be repeated"},
      {Element(Action::kClick, "iframe", "Ad"), "can't be repeated"},
      {Element(Action::kClick, "button", "Go", "not a host!"),
       "can't be repeated"},
      {Navigate("javascript:alert(1)"), "can't be repeated"},
      {Navigate("https://trains.example/r\xC3"), "can't be repeated"},
      {Navigate("https://trains.example/{{pin}}"), "can't be repeated"},
      {Navigate("https://trains.example/" + std::string(1100, 'a')),
       "can't be repeated"},
      {Navigate("https://trains.example/reset?token=abcdef0123456789abcdef"),
       "looks like it holds a password"},
      {Search("my password is hunter2"), "looks like it holds a password"},
      {card_option, "looks like it holds a password"},
      {left, "can't be repeated"},
      {tool, "the AI used a site's own tool."},
  };
  for (size_t i = 0; i < gaps.size(); ++i) {
    const RoutineStep step = Kept(gaps[i].first);
    EXPECT_TRUE(IsUnkeptStep(step)) << i;
    EXPECT_NE(step.agent.why.find(gaps[i].second), std::string::npos)
        << i << ": " << step.agent.why;
  }
  // A step whose page the agent didn't name: the page voice mode is on.
  PlannerStep scroll;
  scroll.action = Action::kScroll;
  scroll.direction = "up";
  EXPECT_EQ(RoutineStepForPlanner(scroll, kGoal, "Trains.Example").host,
            "trains.example");
  EXPECT_EQ(DescribeRoutineStep(Kept(scroll)), "scroll up");
}

TEST(VoiceRoutinesTest, AGapIsntRemembered) {
  std::vector<RoutineStep> steps = Trains();
  steps.insert(steps.begin() + 3,
               UnkeptPlannerStep("the AI used a site's own tool."));
  const BuiltRoutine built = BuildRoutine("voice_10", "trains", steps, kNow);
  EXPECT_FALSE(built.script);
  EXPECT_EQ(built.refusal,
            "I can't remember this: the AI used a site's own tool.");
}

TEST(VoiceRoutinesTest, StartsWhereThePlannersRunBegan) {
  // The run began on a page and clicked there: it opens that page first.
  RoutineStep click = Kept(Element(Action::kClick, "button", "Search"));
  click.url = "https://trains.example/book?from=paris";
  BuiltRoutine built = BuildRoutine("voice_11", "trains", {click}, kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  ASSERT_EQ(built.script->steps.size(), 2u);
  EXPECT_EQ(built.script->steps[0].type, StepType::NAVIGATE);
  EXPECT_EQ(built.script->steps[0].target,
            "https://trains.example/book?from=paris");
  ReadRoutineResult read = ReadRoutine(*built.script);
  ASSERT_TRUE(read.ok) << read.why;
  EXPECT_TRUE(IsAgentStep(read.steps[0]));
  EXPECT_EQ(read.steps[0].agent.action, Action::kNavigate);
  // Without that page (or with one that holds a code): the site's.
  click.url = "https://trains.example/reset?token=abcdef0123456789abcdef";
  built = BuildRoutine("voice_12", "trains", {click}, kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  EXPECT_EQ(built.script->steps[0].target, "https://trains.example/");
  EXPECT_TRUE(ReadRoutine(*built.script).steps[0].planned);
}

TEST(VoiceRoutinesTest, ConfirmingCoversExactlyTheSitesItsStepsUse) {
  const BuiltRoutine built = BuildRoutine("voice_13", "trains", Trains(), kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  Script script = *Script::FromJSONString(built.script->ToJSONString());
  using automation::AutomationSecurity;
  // Not some of them, nor more.
  EXPECT_FALSE(
      AutomationSecurity::ConfirmDraftSites(script, {"trains.example"}));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(
      script, {"trains.example", "search.brave.com", "evil.example"}));
  ASSERT_TRUE(AutomationSecurity::ConfirmDraftSites(
      script, {"search.brave.com", "trains.example"}));
  EXPECT_TRUE(RoutineSitesConfirmed(script));
  EXPECT_FALSE(UnconfirmChangedRoutine(script));

  // A step added in the file on another site: confirmed no more.
  Script added = Copy(script);
  RoutineStep evil =
      Kept(Element(Action::kClick, "button", "Pay", "evil.example"));
  added.steps.push_back(PlannerStepToWorkflowStep(evil));
  EXPECT_FALSE(RoutineSitesConfirmed(added));
  EXPECT_TRUE(ReadRoutine(added).needs_sites_confirmed);
  // Made a draft again, then confirmed for the sites it uses now.
  added.security.trust = TrustLevel::APPROVED;
  EXPECT_TRUE(UnconfirmChangedRoutine(added));
  EXPECT_TRUE(added.security.sites_unconfirmed);
  EXPECT_TRUE(added.security.domain_whitelist.empty());
  EXPECT_EQ(added.security.trust, TrustLevel::CASUAL);
  EXPECT_FALSE(UnconfirmChangedRoutine(added));
  ASSERT_TRUE(AutomationSecurity::ConfirmDraftSites(
      added, {"search.brave.com", "trains.example", "evil.example"}));
  EXPECT_FALSE(ReadRoutine(added).needs_sites_confirmed);

  // A step changed to act on another site, or the whitelist widened in
  // the file: the same.
  Script changed = Copy(script);
  changed.steps[4].extra.GetDict().Set(automation::kStepHostKey,
                                       "evil.example");
  EXPECT_TRUE(ReadRoutine(changed).needs_sites_confirmed);
  Script widened = Copy(script);
  widened.security.domain_whitelist.push_back("evil.example");
  EXPECT_TRUE(ReadRoutine(widened).needs_sites_confirmed);
  // The same sites, written differently: still confirmed.
  Script cased = Copy(script);
  cased.security.domain_whitelist = {"TRAINS.example", "search.brave.com",
                                     "trains.example"};
  EXPECT_FALSE(ReadRoutine(cased).needs_sites_confirmed);

  // A routine of the user's own steps keeps round 41's rule: Approved on
  // the sites of its steps.
  const BuiltRoutine news = BuildRoutine("voice_14", "news", MyNews(), kNow);
  ASSERT_TRUE(news.script);
  EXPECT_TRUE(RoutineSitesConfirmed(*news.script));
  Script own = Copy(*news.script);
  EXPECT_FALSE(UnconfirmChangedRoutine(own));
}

TEST(VoiceRoutinesTest, AHostilePlannerStepIsntRun) {
  const BuiltRoutine built = BuildRoutine("voice_15", "trains", Trains(), kNow);
  ASSERT_TRUE(built.script) << built.refusal;
  const Script good = *Script::FromJSONString(built.script->ToJSONString());
  ASSERT_TRUE(ReadRoutine(good).ok);
  // Each changes step |first| of the routine.
  using Change = std::function<void(automation::Step&)>;
  const std::vector<std::pair<size_t, Change>> changes = {
      {4, [](automation::Step& s) { VoiceOf(s).Set("agent", "drag"); }},
      {4, [](automation::Step& s) { VoiceOf(s).Remove("planned"); }},
      {4, [](automation::Step& s) { VoiceOf(s).Set("planned", false); }},
      {4, [](automation::Step& s) { VoiceOf(s).Set("planned", "yes"); }},
      {4, [](automation::Step& s) { s.type = StepType::TYPE; }},
      {4, [](automation::Step& s) { s.type = StepType::RUN_JS; }},
      {4, [](automation::Step& s) { VoiceOf(s).Set("kind", "script"); }},
      {4, [](automation::Step& s) { VoiceOf(s).Remove("kind"); }},
      {4, [](automation::Step& s) { VoiceOf(s).Set("label", ""); }},
      {4, [](automation::Step& s) { VoiceOf(s).Set("label", 7); }},
      {4,
       [](automation::Step& s) {
         VoiceOf(s).Set("label", std::string(61, 'a'));
       }},
      {4,
       [](automation::Step& s) {
         VoiceOf(s).Set("label",
                        "a\x01"
                        "b");
       }},
      {4, [](automation::Step& s) { VoiceOf(s).Set("label", "{{pin}}"); }},
      {4, [](automation::Step& s) { VoiceOf(s).Set("label", "\xC3"); }},
      {4,
       [](automation::Step& s) {
         s.extra.GetDict().Remove(automation::kStepHostKey);
       }},
      {4,
       [](automation::Step& s) {
         s.extra.GetDict().Set(automation::kStepHostKey, "evil example");
       }},
      {2, [](automation::Step& s) { VoiceOf(s).Set("variable", "Bad Name"); }},
      {2, [](automation::Step& s) { VoiceOf(s).Set("variable", "pin"); }},
      {2, [](automation::Step& s) { VoiceOf(s).Remove("text"); }},
      {2,
       [](automation::Step& s) {
         VoiceOf(s).Set("text", std::string(501, 'a'));
       }},
      {2, [](automation::Step& s) { VoiceOf(s).Set("text", "{{pin}}"); }},
      {3, [](automation::Step& s) { VoiceOf(s).Remove("option"); }},
      {3, [](automation::Step& s) { VoiceOf(s).Set("option", "a\nb"); }},
      {5, [](automation::Step& s) { VoiceOf(s).Set("direction", "left"); }},
      {1, [](automation::Step& s) { s.target = "javascript:alert(1)"; }},
      {1, [](automation::Step& s) { s.target = "file:///etc/passwd"; }},
      {1, [](automation::Step& s) { s.target = ""; }},
      {1, [](automation::Step& s) { s.target = "https://x.example/{{pin}}"; }},
      {1,
       [](automation::Step& s) {
         s.target = "https://trains.example/" + std::string(1100, 'a');
       }},
      {0, [](automation::Step& s) { VoiceOf(s).Remove("query"); }},
      {0, [](automation::Step& s) { VoiceOf(s).Set("query", "a\nb"); }},
      {0, [](automation::Step& s) { s.type = StepType::CLICK; }},
  };
  for (size_t i = 0; i < changes.size(); ++i) {
    Script script = Copy(good);
    changes[i].second(script.steps[changes[i].first]);
    const ReadRoutineResult read = ReadRoutine(script);
    EXPECT_FALSE(read.ok) << i;
    EXPECT_TRUE(read.steps.empty()) << i;
    EXPECT_EQ(read.why, "Step " + std::to_string(changes[i].first + 1) +
                            " of this routine isn't one I can do by voice.")
        << i;
  }
  // A gap is never stored, so never read.
  Script gap = Copy(good);
  VoiceOf(gap.steps[4]).Set("agent", "unkept");
  EXPECT_FALSE(ReadRoutine(gap).ok);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
