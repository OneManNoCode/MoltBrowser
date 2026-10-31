// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Routines by voice (voice_routines_logic.h) and the loop's learning
// commands (voice_controller_logic.h): which commands become steps and how
// choices are kept, what "this" is, names, and "remember this", "run",
// "forget", "what are my routines", "forget the word", "forget everything"
// and a site's hints as the loop plans them.

#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_test_trees.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using test::TreeBuilder;
using K = VoiceStepKind;

constexpr uint32_t kField = kStateFocusable | kStateEditable;

VoiceCommand Parse(std::string_view said, bool choice = false) {
  ParseContext context;
  context.expecting_choice = choice;
  return ParseVoiceCommand(said, context);
}

std::optional<RoutineStep> StepFor(std::string_view said,
                                   int position = 0,
                                   std::string_view label = {},
                                   bool answered_offer = false) {
  return RoutineStepFor(Parse(said), "www.youtube.com",
                        position, label, answered_offer);
}

std::string Described(std::string_view said, int position = 0) {
  std::optional<RoutineStep> step = StepFor(said, position);
  return step ? DescribeRoutineStep(*step) : std::string("(none)");
}

VoiceEnvironment Env() {
  VoiceEnvironment env;
  env.page_id = 1;
  env.host = "www.youtube.com";
  env.web_page = true;
  env.now_ms = 1000000;
  env.learning.routines_available = true;
  env.learning.enabled = true;
  return env;
}

std::string Said(const VoiceTurn& turn) {
  std::string out;
  for (const VoiceStep& step : turn.steps) {
    if (step.kind == K::kSpeak) {
      if (!out.empty()) {
        out.push_back('|');
      }
      out.append(step.text);
    }
  }
  return out;
}

const VoiceStep* Find(const std::vector<VoiceStep>& steps, VoiceStepKind kind) {
  for (const VoiceStep& step : steps) {
    if (step.kind == kind) {
      return &step;
    }
  }
  return nullptr;
}

TEST(VoiceRoutinesLogicTest, CommandsBecomeSteps) {
  EXPECT_EQ(Described("open YouTube"), "open YouTube");
  EXPECT_EQ(Described("open example dot com in a new tab"),
            "open example dot com in a new tab");
  EXPECT_EQ(Described("search for news"), "search for news");
  EXPECT_EQ(Described("search YouTube for cats"), "search YouTube for cats");
  EXPECT_EQ(Described("search the web for trains"),
            "search the web for trains");
  EXPECT_EQ(Described("play the first one"), "play the first one");
  EXPECT_EQ(Described("the last one"), "choose the last one");
  EXPECT_EQ(Described("play the live one by Queen"),
            "play the live one by queen");
  EXPECT_EQ(Described("skip ahead 30 seconds"), "skip ahead 30 seconds");
  EXPECT_EQ(Described("pause"), "pause");
  EXPECT_EQ(Described("go back"), "go back");
  EXPECT_EQ(Described("read the results"), "read the results");
  EXPECT_EQ(Described("type hello world"), "type hello world");
  EXPECT_EQ(Described("scroll down"), "scroll down");
  // What isn't a routine's step.
  for (const char* said :
       {"what can I say", "turn off voice mode", "stop", "repeat",
        "remember this as my news", "run my news", "forget my news",
        "what are my routines", "close the tab", "switch to tab 2",
        "find me a train to Lyon", "yes", "next heading"}) {
    EXPECT_FALSE(StepFor(said)) << said;
  }
  // Only the fields the kind uses.
  std::optional<RoutineStep> step = StepFor("search YouTube for cats");
  ASSERT_TRUE(step);
  EXPECT_EQ(step->command.kind, VoiceCommandKind::kSearch);
  EXPECT_EQ(step->command.text, "cats");
  EXPECT_EQ(step->command.scope, SearchScope::kNamedSite);
  EXPECT_EQ(step->command.site, "youtube");
  EXPECT_TRUE(step->command.heard.empty());
  EXPECT_EQ(step->host, "www.youtube.com");
  step = RoutineStepFor(Parse("pause"), "not a host!", 0, {}, false);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->host, "");
}

TEST(VoiceRoutinesLogicTest, ChoicesReplayAsTheyWereMade) {
  // By position: the same position on the new page.
  std::optional<RoutineStep> step = StepFor("play the second one", 2);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->command.choice.kind, ChoiceRef::Kind::kPosition);
  EXPECT_EQ(step->command.choice.ordinal, 2);
  step = StepFor("the second to last one", 19);
  ASSERT_TRUE(step);
  EXPECT_TRUE(step->command.choice.from_end);
  EXPECT_EQ(DescribeRoutineStep(*step), "choose the second to last one");
  // By name: the user's words.
  step = StepFor("play the live one by Queen", 4);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->command.choice.kind, ChoiceRef::Kind::kDescription);
  EXPECT_EQ(step->command.choice.by_words, std::vector<std::string>{"queen"});
  // A number or "the next one" names a place at that moment: kept as what
  // it chose.
  step = StepFor("number four", 4);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->command.choice.kind, ChoiceRef::Kind::kPosition);
  EXPECT_EQ(DescribeRoutineStep(*step), "choose the fourth one");
  step = StepFor("play the next one", 3);
  ASSERT_TRUE(step);
  EXPECT_EQ(DescribeRoutineStep(*step), "play the third one");
  step = StepFor("click 12", 0, "Sign in");
  ASSERT_TRUE(step);
  EXPECT_EQ(step->command.choice.kind, ChoiceRef::Kind::kDescription);
  EXPECT_EQ(DescribeRoutineStep(*step), "click the sign in one");
  EXPECT_FALSE(StepFor("click 12", 0, ""));
  // Answering "which one?": what it chose.
  step = RoutineStepFor(Parse("the live one", /*choice=*/true),
                        "www.youtube.com", 7, "", /*answered_offer=*/true);
  ASSERT_TRUE(step);
  EXPECT_EQ(step->command.choice.kind, ChoiceRef::Kind::kPosition);
  EXPECT_EQ(step->command.choice.ordinal, 7);
}

TEST(VoiceRoutinesLogicTest, ThisIsTheTaskSoFar) {
  RoutineRecorder recorder;
  const int v0 = recorder.version();
  recorder.Add(*StepFor("search for cats"));
  recorder.Add(*StepFor("pause"));
  // Opening a site starts a new task.
  recorder.Add(*StepFor("open YouTube"));
  recorder.Add(*StepFor("search for news"));
  recorder.Add(*StepFor("play the first one", 1));
  ASSERT_EQ(recorder.steps().size(), 3u);
  std::vector<std::string> said;
  for (const RoutineStep& step : recorder.steps()) {
    said.push_back(DescribeRoutineStep(step));
  }
  EXPECT_EQ(JoinSpokenList(said),
            "open YouTube, search for news and play the first one");
  EXPECT_NE(recorder.version(), v0);
  EXPECT_FALSE(recorder.planned());
  // The planner's steps never start a task.
  RoutineStep planned = *StepFor("open example dot com");
  planned.planned = true;
  recorder.Add(planned);
  EXPECT_EQ(recorder.steps().size(), 4u);
  EXPECT_TRUE(recorder.planned());
  // At most kMaxThisSteps.
  for (int i = 0; i < 30; ++i) {
    recorder.Add(*StepFor("scroll down"));
  }
  EXPECT_EQ(recorder.steps().size(), kMaxThisSteps);
  const int before = recorder.version();
  recorder.Clear();
  EXPECT_TRUE(recorder.steps().empty());
  EXPECT_NE(recorder.version(), before);
  EXPECT_EQ(JoinSpokenList({}), "");
  EXPECT_EQ(JoinSpokenList({"a"}), "a");
  EXPECT_EQ(JoinSpokenList({"a", "b"}), "a and b");
}

// The planner's steps in "this": in order with the user's, never a task's
// start, said as the browser says them; a gap stops "this" from being
// saved until a new task begins.
TEST(VoiceRoutinesLogicTest, ThisKeepsThePlannersSteps) {
  RoutineRecorder recorder;
  recorder.Add(*StepFor("open YouTube"));
  RoutineStep click;
  click.planned = true;
  click.host = "www.youtube.com";
  click.agent.action = PlannerStep::Action::kClick;
  click.agent.kind = "link";
  click.agent.label = "Live Aid \u2013 Queen";
  recorder.Add(click);
  RoutineStep typed = click;
  typed.agent.action = PlannerStep::Action::kType;
  typed.agent.kind = "input";
  typed.agent.label = "Comment";
  typed.agent.text = "great";
  recorder.Add(typed);
  typed.variable = "comment";
  typed.agent.text.clear();
  recorder.Add(typed);
  RoutineStep scroll = click;
  scroll.agent.action = PlannerStep::Action::kScroll;
  scroll.agent.direction = "down";
  recorder.Add(scroll);
  recorder.Add(*StepFor("pause"));
  ASSERT_EQ(recorder.steps().size(), 6u);
  EXPECT_TRUE(recorder.planned());
  EXPECT_EQ(recorder.refusal(), "");
  std::vector<std::string> said;
  for (const RoutineStep& step : recorder.steps()) {
    EXPECT_EQ(IsAgentStep(step), step.planned);
    said.push_back(DescribeRoutineStep(step));
  }
  EXPECT_EQ(JoinSpokenList(said),
            "open YouTube, click Live Aid \u2013 Queen, type great in Comment, "
            "type what you tell me in Comment, scroll down and pause");
  // A gap: "this" can't be saved, until a new task begins.
  RoutineStep gap;
  gap.planned = true;
  gap.agent.action = PlannerStep::Action::kUnkept;
  gap.agent.why = "the AI used a site's own tool.";
  recorder.Add(gap);
  EXPECT_TRUE(IsUnkeptStep(recorder.steps().back()));
  EXPECT_EQ(recorder.refusal(), "the AI used a site's own tool.");
  EXPECT_EQ(DescribeRoutineStep(gap), "a step the AI took that I can't repeat");
  recorder.Add(*StepFor("open YouTube"));
  EXPECT_EQ(recorder.refusal(), "");
  EXPECT_FALSE(recorder.planned());
  // A voice command the planner chose (round 41's format) isn't an agent
  // step.
  RoutineStep command = *StepFor("pause");
  command.planned = true;
  EXPECT_FALSE(IsAgentStep(command));
  EXPECT_EQ(DescribeRoutineStep(command), "pause");
}

TEST(VoiceRoutinesLogicTest, Names) {
  EXPECT_EQ(RoutineDisplayName(" my  news "), "my news");
  EXPECT_EQ(RoutineDisplayName("yes"), "");
  EXPECT_EQ(RoutineDisplayName("okay"), "");
  EXPECT_EQ(RoutineDisplayName("42"), "");
  EXPECT_EQ(RoutineDisplayName(""), "");
  EXPECT_EQ(RoutineDisplayName("one two three four five six seven eight nine"),
            "");
  EXPECT_EQ(RoutineDisplayName(std::string(150, 'a')), "");
  EXPECT_EQ(RoutineNameKey("My News"), "news");
  EXPECT_EQ(RoutineNameKey("the news"), "news");
  EXPECT_EQ(RoutineNameKey("my"), "my");
  const std::vector<std::string> names = {"my news", "Morning Mail"};
  ASSERT_TRUE(FindRoutineName(names, "news"));
  EXPECT_EQ(*FindRoutineName(names, "News"), "my news");
  EXPECT_EQ(*FindRoutineName(names, "my morning mail"), "Morning Mail");
  EXPECT_FALSE(FindRoutineName(names, "weather"));
  EXPECT_FALSE(FindRoutineName(names, ""));

  // Numbers as digits, written or said (profile-routines).
  EXPECT_EQ(RoutineNameKey("my news (2)"), "news 2");
  EXPECT_EQ(RoutineNameKey("My News two"), "news 2");
  EXPECT_EQ(RoutineNameKey("top twenty one"), "top 21");
  EXPECT_EQ(RoutineNameKey("the first trip"), "first trip");
  EXPECT_EQ(RoutineNameKey("trip to paris"), "trip to paris");
  const std::vector<std::string> copies = {"my news", "my news (2)"};
  EXPECT_EQ(*FindRoutineName(copies, "my news two"), "my news (2)");
  EXPECT_EQ(*FindRoutineName(copies, "news 2"), "my news (2)");
  EXPECT_EQ(*FindRoutineName(copies, "the news"), "my news");
}

TEST(VoiceRoutinesLogicTest, ANameRunsExactlyOneRoutine) {
  // Two routines that are said alike (files edited, or copied in by hand):
  // neither is found by that name, and the count says why.
  const std::vector<std::string> names = {"my news", "The News", "trains"};
  RoutineNameMatch match = MatchRoutineName(names, "news");
  EXPECT_EQ(match.count, 2u);
  EXPECT_FALSE(match.name);
  EXPECT_FALSE(FindRoutineName(names, "news"));
  match = MatchRoutineName(names, "trains");
  EXPECT_EQ(match.count, 1u);
  ASSERT_TRUE(match.name);
  EXPECT_EQ(*match.name, "trains");
  EXPECT_EQ(MatchRoutineName(names, "weather").count, 0u);
  EXPECT_EQ(MatchRoutineName(names, "").count, 0u);

  const PageMap map;
  VoiceEnvironment env = Env();
  env.learning.routines = names;
  for (const char* said : {"run news", "news"}) {
    const VoiceTurn turn = PlanUtterance(said, map, env, ConversationState());
    EXPECT_EQ(Said(turn),
              "You have 2 routines called news. Rename one in Agent mode to "
              "run it by voice.")
        << said;
    EXPECT_FALSE(Find(turn.steps, K::kLearning)) << said;
    EXPECT_FALSE(Find(turn.steps, K::kToPlanner)) << said;
  }
  VoiceTurn turn = PlanUtterance("forget news", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "You have 2 routines called news. Forget the one you mean in "
            "Agent mode.");
  EXPECT_FALSE(turn.state.confirming);
  env.learning.this_steps = {"pause"};
  turn = PlanUtterance("remember this as news", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "You have 2 routines called news. Choose another name, or rename "
            "them in Agent mode.");
  EXPECT_FALSE(turn.state.confirming);
}

TEST(VoiceRoutinesLogicTest, UniqueNames) {
  const std::vector<std::string> names = {"my news", "my news (2)", "Trains"};
  EXPECT_EQ(UniqueRoutineName(names, "weather"), "weather");
  EXPECT_EQ(UniqueRoutineName(names, "My News"), "My News (3)");
  EXPECT_EQ(UniqueRoutineName(names, "my news (2)"), "my news (3)");
  EXPECT_EQ(UniqueRoutineName(names, "news two"), "news two (2)");
  EXPECT_EQ(UniqueRoutineName(names, "the trains"), "the trains (2)");
  EXPECT_EQ(UniqueRoutineName({}, "my news"), "my news");
  // Nothing to say: kept as it is.
  EXPECT_EQ(UniqueRoutineName(names, "!!"), "!!");
  // Every copy taken.
  std::vector<std::string> full = {"x"};
  for (int n = 2; n <= kMaxRoutineNameCopies; ++n) {
    full.push_back("x (" + std::to_string(n) + ")");
  }
  EXPECT_EQ(UniqueRoutineName(full, "x"), "");
  full.pop_back();
  EXPECT_EQ(UniqueRoutineName(full, "x"),
            "x (" + std::to_string(kMaxRoutineNameCopies) + ")");
}

TEST(VoiceRoutinesLogicTest, ANameInUseIsReplacedOnYes) {
  const PageMap map;
  VoiceEnvironment env = Env();
  env.learning.routines = {"my news", "trains"};
  env.learning.this_steps = {"open YouTube", "search for news"};
  env.learning.this_version = 4;
  VoiceTurn turn =
      PlanUtterance("remember this as my news", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "You already have a routine called my news.|I'll remember: open "
            "YouTube and search for news.|Say yes to replace it, or no to "
            "cancel.");
  EXPECT_FALSE(Find(turn.steps, K::kLearning));
  ASSERT_TRUE(turn.state.confirming);
  const ConversationState asked = turn.state;
  // "Yes": replaced (the glue keeps its id).
  turn = PlanUtterance("yes", map, env, asked);
  const VoiceStep* replace = Find(turn.steps, K::kLearning);
  ASSERT_TRUE(replace);
  EXPECT_EQ(replace->index, static_cast<int>(LearningOp::kReplaceRoutine));
  EXPECT_EQ(replace->text, "my news");
  EXPECT_EQ(replace->count, 4);
  // "No": not saved, and another name is asked for.
  turn = PlanUtterance("no", map, env, asked);
  EXPECT_FALSE(Find(turn.steps, K::kLearning));
  EXPECT_EQ(Said(turn),
            "Okay, I won't replace it. Say remember this as, and another "
            "name.");
  EXPECT_FALSE(turn.state.confirming);
  // Another question's "no" is as before.
  turn = PlanUtterance("forget trains", map, env, ConversationState());
  turn = PlanUtterance("no", map, env, turn.state);
  EXPECT_EQ(Said(turn), "Okay, I won't.");
}

TEST(VoiceRoutinesLogicTest, RoutinesFromBeforeProfilesAreOffered) {
  const PageMap map;
  VoiceEnvironment env = Env();
  env.learning.routines = {"trains"};
  env.learning.unclaimed = {"my news", "morning mail"};
  env.learning.unclaimed_version = 9;
  VoiceTurn turn =
      PlanUtterance("what are my routines", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "You have one routine: trains. Say run, then its name.|2 routines "
            "were made before routines belonged to a profile: my news and "
            "morning mail.|Say yes to keep them here, or no to cancel.");
  ASSERT_TRUE(turn.state.confirming);
  const ConversationState asked = turn.state;
  // "Yes": kept here, exactly the ones asked about.
  turn = PlanUtterance("yes", map, env, asked);
  const VoiceStep* claim = Find(turn.steps, K::kLearning);
  ASSERT_TRUE(claim);
  EXPECT_EQ(claim->index, static_cast<int>(LearningOp::kClaimRoutines));
  EXPECT_EQ(claim->count, 9);
  // "No": left for another profile, and not asked again here.
  turn = PlanUtterance("no", map, env, asked);
  const VoiceStep* decline = Find(turn.steps, K::kLearning);
  ASSERT_TRUE(decline);
  EXPECT_EQ(decline->index, static_cast<int>(LearningOp::kDeclineRoutines));
  EXPECT_EQ(decline->count, 9);
  EXPECT_EQ(Said(turn),
            "Okay, I'll leave them for another profile. You can still keep "
            "them here in Agent mode.");

  // Run by name: not this profile's yet, so it isn't run; the question is
  // asked.
  turn = PlanUtterance("run my news", map, env, ConversationState());
  EXPECT_FALSE(Find(turn.steps, K::kLearning));
  EXPECT_EQ(Said(turn),
            "my news was made before routines belonged to a profile.|2 "
            "routines were made before routines belonged to a profile: my "
            "news and morning mail.|Say yes to keep them here, or no to "
            "cancel.");
  // Its name alone isn't a command here: the planner gets it.
  EXPECT_TRUE(
      Find(PlanUtterance("morning mail", map, env, ConversationState()).steps,
           K::kToPlanner));
  // Nor are they forgotten from here.
  EXPECT_EQ(
      Said(PlanUtterance("forget my news", map, env, ConversationState())),
      "I don't have a routine called my news.");

  // No routines of its own: only the question.
  env.learning.routines.clear();
  env.learning.unclaimed = {"my news"};
  turn = PlanUtterance("what are my routines", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "One routine was made before routines belonged to a profile: my "
            "news.|Say yes to keep it here, or no to cancel.");
  // More than can be read: chosen in Agent mode, never by one "yes".
  env.learning.unclaimed.clear();
  for (size_t i = 0; i <= kMaxRoutinesRead; ++i) {
    env.learning.unclaimed.push_back("routine " + std::to_string(i));
  }
  turn = PlanUtterance("what are my routines", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "11 routines were made before routines belonged to a profile. "
            "Choose the ones to keep here in Agent mode.");
  EXPECT_FALSE(turn.state.confirming);
  // Nowhere to keep them: nothing said of them.
  env.learning.routines_available = false;
  turn = PlanUtterance("what are my routines", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "You don't have any routines yet. Do something, then say remember "
            "this as, and a name.");
}

TEST(VoiceRoutinesLogicTest, RememberThisReadsBackAndWaitsForYes) {
  const PageMap map;
  VoiceEnvironment env = Env();
  env.learning.this_steps = {"open YouTube", "search for news",
                             "play the first one"};
  env.learning.this_version = 7;
  VoiceTurn turn =
      PlanUtterance("remember this as my news", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "I'll remember: open YouTube, search for news and play the first "
            "one.|Say yes to save it as my news, or no to cancel.");
  EXPECT_FALSE(Find(turn.steps, K::kLearning));
  ASSERT_TRUE(turn.state.confirming);
  // "Yes" saves exactly what was read back.
  turn = PlanUtterance("yes", map, env, turn.state);
  const VoiceStep* save = Find(turn.steps, K::kLearning);
  ASSERT_TRUE(save);
  EXPECT_EQ(save->index, static_cast<int>(LearningOp::kSaveRoutine));
  EXPECT_EQ(save->text, "my news");
  EXPECT_EQ(save->count, 7);
  // "No": nothing.
  turn = PlanUtterance("remember this as my news", map, env,
                       ConversationState());
  turn = PlanUtterance("no", map, env, turn.state);
  EXPECT_FALSE(Find(turn.steps, K::kLearning));

  // The AI's steps: said, and saved as a draft by the glue.
  env.learning.this_planned = true;
  turn = PlanUtterance("save this routine as trains", map, env,
                       ConversationState());
  EXPECT_NE(Said(turn).find("Some of these steps are the AI's"),
            std::string::npos);

  // A step of the AI's that a routine can't repeat: nothing to save.
  env.learning.this_refusal = "the AI used a site's own tool.";
  turn =
      PlanUtterance("remember this as trains", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "I can't remember this: the AI used a site's own tool.");
  EXPECT_FALSE(turn.state.confirming);
  env.learning.this_refusal.clear();

  // Nothing yet; a name in use; a name that isn't one; nowhere to keep it.
  env.learning.this_steps.clear();
  EXPECT_EQ(Said(PlanUtterance("remember this as my news", map, env,
                               ConversationState())),
            "There's nothing to remember yet. Do it once, then say remember "
            "this as my news.");
  env.learning.this_steps = {"pause"};
  env.learning.routines = {"My News"};
  EXPECT_EQ(Said(PlanUtterance("remember this as news", map, env,
                               ConversationState())),
            "You already have a routine called news.|I'll remember: pause.|"
            "Some of these steps are the AI's, so you'll confirm the sites "
            "they use before it runs.|Say yes to replace it, or no to "
            "cancel.");
  EXPECT_EQ(Said(PlanUtterance("remember this as okay", map, env,
                               ConversationState())),
            "Say remember this as, then a short name for it.");
  env.learning.routines_available = false;
  EXPECT_EQ(Said(PlanUtterance("remember this as mine", map, env,
                               ConversationState())),
            "I can't keep routines here.");
}

TEST(VoiceRoutinesLogicTest, RunListAndForget) {
  const PageMap map;
  VoiceEnvironment env = Env();
  env.learning.routines = {"my news", "morning mail"};
  // "run my news", and the name alone.
  for (const char* said : {"run my news", "run the routine called news",
                           "my news", "News"}) {
    VoiceTurn turn = PlanUtterance(said, map, env, ConversationState());
    const VoiceStep* run = Find(turn.steps, K::kLearning);
    ASSERT_TRUE(run) << said;
    EXPECT_EQ(run->index, static_cast<int>(LearningOp::kRunRoutine));
    EXPECT_EQ(run->text, "my news");
    EXPECT_FALSE(Find(turn.steps, K::kToPlanner));
  }
  EXPECT_EQ(Said(PlanUtterance("run the weather", map, env,
                               ConversationState())),
            "I don't have a routine called the weather. Say what are my "
            "routines.");
  // Anything else still goes to the planner.
  EXPECT_TRUE(Find(PlanUtterance("the weather tomorrow", map, env,
                                 ConversationState())
                       .steps,
                   K::kToPlanner));

  EXPECT_EQ(Said(PlanUtterance("what are my routines", map, env,
                               ConversationState())),
            "You have 2 routines: my news and morning mail. Say run, then its "
            "name.");
  EXPECT_EQ(Said(PlanUtterance("list my routines", map, env,
                               ConversationState())),
            "You have 2 routines: my news and morning mail. Say run, then its "
            "name.");

  // Forgetting waits for "yes".
  VoiceTurn turn =
      PlanUtterance("forget my news", map, env, ConversationState());
  EXPECT_EQ(Said(turn),
            "Say yes to forget the routine my news, or no to cancel.");
  EXPECT_FALSE(Find(turn.steps, K::kLearning));
  turn = PlanUtterance("yes", map, env, turn.state);
  const VoiceStep* forget = Find(turn.steps, K::kLearning);
  ASSERT_TRUE(forget);
  EXPECT_EQ(forget->index, static_cast<int>(LearningOp::kForgetRoutine));
  EXPECT_EQ(forget->text, "my news");
  EXPECT_EQ(Said(PlanUtterance("forget the weather", map, env,
                               ConversationState())),
            "I don't have a routine called the weather.");

  env.learning.routines.clear();
  EXPECT_EQ(Said(PlanUtterance("what are my routines", map, env,
                               ConversationState())),
            "You don't have any routines yet. Do something, then say remember "
            "this as, and a name.");
  EXPECT_EQ(Said(PlanUtterance("run my news", map, env, ConversationState())),
            "You don't have any routines yet. Do something, then say remember "
            "this as, and a name.");
}

TEST(VoiceRoutinesLogicTest, ForgetWordsAndEverything) {
  const PageMap map;
  VoiceEnvironment env = Env();
  env.learning.words = {"queen", "green"};
  VoiceTurn turn =
      PlanUtterance("forget the word Queen", map, env, ConversationState());
  EXPECT_EQ(Said(turn), "Say yes to forget the word Queen, or no to cancel.");
  turn = PlanUtterance("yes", map, env, turn.state);
  const VoiceStep* forget = Find(turn.steps, K::kLearning);
  ASSERT_TRUE(forget);
  EXPECT_EQ(forget->index, static_cast<int>(LearningOp::kForgetWord));
  EXPECT_EQ(forget->text, "Queen");
  EXPECT_EQ(Said(PlanUtterance("forget the word Adele", map, env,
                               ConversationState())),
            "I haven't learned the word Adele.");

  for (const char* said :
       {"forget everything you learned", "delete everything you know about me",
        "forget all you've learned"}) {
    turn = PlanUtterance(said, map, env, ConversationState());
    EXPECT_EQ(turn.kind, VoiceCommandKind::kForgetLearning) << said;
    ASSERT_TRUE(turn.state.confirming) << said;
    turn = PlanUtterance("yes", map, env, turn.state);
    forget = Find(turn.steps, K::kLearning);
    ASSERT_TRUE(forget) << said;
    EXPECT_EQ(forget->index, static_cast<int>(LearningOp::kForgetEverything));
  }
  // "Forget it" still cancels.
  EXPECT_EQ(Parse("forget it").kind, VoiceCommandKind::kCancel);
}

// A site with two search boxes, where the second is the one that works.
PageTree TwoSearchBoxes() {
  TreeBuilder b("Shop");
  const uint32_t root =
      b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea, "Shop", 0);
  b.Add(root, PageRole::kSearchBox, "Search the site", kField);
  b.Add(root, PageRole::kSearchBox, "Search products", kField);
  return b.Take();
}

TEST(VoiceRoutinesLogicTest, SearchUsesTheSitesHint) {
  const PageMap map = BuildPageMap(TwoSearchBoxes());
  VoiceEnvironment env = Env();
  env.host = "shop.example";
  // The generic map: the first box; the turn says what it used.
  VoiceTurn turn = PlanUtterance("search for lamps", map, env,
                                 ConversationState());
  const VoiceStep* typed = Find(turn.steps, K::kSetText);
  ASSERT_TRUE(typed);
  EXPECT_EQ(map.ItemForHandle(typed->target)->label, "Search the site");
  ASSERT_EQ(turn.site_parts.size(), 1u);
  EXPECT_EQ(turn.site_parts[0].part, SitePart::kSearchField);
  EXPECT_EQ(turn.site_parts[0].source, PartSource::kGeneric);
  EXPECT_EQ(turn.site_parts[0].host, "shop.example");
  EXPECT_EQ(turn.site_parts[0].name, "Search the site");
  EXPECT_FALSE(turn.site_parts[0].hint_missed);

  // With the hint: the box that worked before.
  env.learning.hint.host = "shop.example";
  env.learning.hint.search_field = "Search products";
  turn = PlanUtterance("search for lamps", map, env, ConversationState());
  typed = Find(turn.steps, K::kSetText);
  ASSERT_TRUE(typed);
  EXPECT_EQ(map.ItemForHandle(typed->target)->label, "Search products");
  ASSERT_EQ(turn.site_parts.size(), 1u);
  EXPECT_EQ(turn.site_parts[0].source, PartSource::kHint);

  // A hint the page no longer has: the generic map, and the miss noted.
  env.learning.hint.search_field = "Search the archive";
  turn = PlanUtterance("search for lamps", map, env, ConversationState());
  ASSERT_EQ(turn.site_parts.size(), 1u);
  EXPECT_EQ(turn.site_parts[0].source, PartSource::kGeneric);
  EXPECT_TRUE(turn.site_parts[0].hint_missed);

  // Learning off: no hint is used.
  env.learning.hint.search_field = "Search products";
  env.learning.enabled = false;
  turn = PlanUtterance("search for lamps", map, env, ConversationState());
  typed = Find(turn.steps, K::kSetText);
  ASSERT_TRUE(typed);
  EXPECT_EQ(map.ItemForHandle(typed->target)->label, "Search the site");
}

TEST(VoiceRoutinesLogicTest, ChoosingFromAListNotesTheList) {
  const PageMap map = BuildPageMap(test::YouTubeLikeResults());
  const VoiceEnvironment env = Env();
  VoiceTurn turn = PlanUtterance("read the results", map, env,
                                 ConversationState());
  // Reading alone isn't the list working.
  EXPECT_TRUE(turn.site_parts.empty());
  turn = PlanUtterance("play the first one", map, env, turn.state);
  ASSERT_TRUE(Find(turn.steps, K::kPress));
  ASSERT_EQ(turn.site_parts.size(), 1u);
  EXPECT_EQ(turn.site_parts[0].part, SitePart::kResultList);
}

TEST(VoiceRoutinesLogicTest, NothingInMoltNetWindows) {
  const PageMap map;
  VoiceEnvironment env = Env();
  env.moltnet = true;
  env.learning.routines = {"my news"};
  for (const char* said : {"run my news", "my news", "what are my routines",
                           "forget everything you learned"}) {
    const VoiceTurn turn = PlanUtterance(said, map, env, ConversationState());
    EXPECT_EQ(Said(turn), "Voice mode is off in MoltNet windows.") << said;
    EXPECT_FALSE(Find(turn.steps, K::kLearning)) << said;
  }
}

TEST(VoiceRoutinesLogicTest, CommandForStepFillsTheVariable) {
  RoutineStep step = *StepFor("type hello");
  step.variable = "value";
  step.command.text.clear();
  EXPECT_EQ(DescribeRoutineStep(step), "type what you tell me");
  const VoiceCommand command = CommandForStep(step, "secret text");
  EXPECT_EQ(command.kind, VoiceCommandKind::kType);
  EXPECT_EQ(command.text, "secret text");
  EXPECT_EQ(command.heard, "type what you tell me");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
