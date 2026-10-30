// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The planner's steps for routines (voice_agent_replay.h): what a routine
// keeps of each of the agent's steps, and a step replayed: the element
// found again by kind and name, exactly once, as a reply the agent's own
// parser takes.

#include "chrome/browser/molt_ai/agents/voice_agent_replay.h"

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice_agent {
namespace {

using agent_actions::ActionType;
using agent_actions::Observation;
using agent_actions::ObservedElement;
using automation::agent_draft::RunElement;
using automation::agent_draft::RunStep;
using Action = voice::PlannerStep::Action;
using Outcome = ReplayPick::Outcome;

RunElement Element(std::string kind, std::string label, std::string text = "") {
  RunElement element;
  element.kind = std::move(kind);
  element.label = std::move(label);
  element.text = std::move(text);
  return element;
}

RunStep Ran(std::string action,
            std::vector<RunElement> elements = {},
            bool success = true) {
  RunStep step;
  step.action = std::move(action);
  step.success = success;
  step.origin = "https://trains.example";
  step.elements = std::move(elements);
  return step;
}

ObservedElement Observed(std::string ref,
                         std::string kind,
                         std::string label,
                         bool typeable = false) {
  ObservedElement element;
  element.ref = std::move(ref);
  element.kind = std::move(kind);
  element.label = std::move(label);
  element.typeable = typeable;
  return element;
}

// The booking page, as the page map shows it.
Observation BookingPage() {
  Observation observation;
  observation.from_page_map = true;
  observation.elements.push_back(Observed("e3", "input", "To", true));
  observation.elements.push_back(Observed("e4", "button", "Search"));
  ObservedElement select = Observed("e7", "select", "Class");
  select.selectable = true;
  select.options = {"Standard", "First class"};
  observation.elements.push_back(select);
  observation.elements.push_back(Observed("e9", "link", "More"));
  observation.elements.push_back(Observed("e12", "link", "More"));
  observation.elements.push_back(Observed("e14", "input", "Promo"));
  return observation;
}

voice::PlannerStep Step(Action action, std::string kind, std::string label) {
  voice::PlannerStep step;
  step.action = action;
  step.kind = std::move(kind);
  step.label = std::move(label);
  return step;
}

// The reply, through the agent's own parser.
agent_actions::AgentAction Parsed(const ReplayPick& pick,
                                  const Observation& observation) {
  agent_actions::ParsedAction parsed =
      agent_actions::ParseAction(pick.reply, observation);
  EXPECT_TRUE(parsed.action)
      << pick.reply << ": " << agent_actions::ActionErrorText(parsed.error);
  return parsed.action.value_or(agent_actions::AgentAction());
}

TEST(VoiceAgentReplayTest, WhatARoutineKeepsOfEachStep) {
  RunStep navigate = Ran("NAVIGATE");
  navigate.url = "https://trains.example/book";
  RunStep search = Ran("SEARCH");
  search.url = "https://search.brave.com/search?q=trains";
  search.query = "trains";
  RunStep scroll = Ran("SCROLL");
  scroll.direction = "down";
  RunElement card = Element("input", "Card number", "4111");
  card.field_kind = agent_safety::FieldKind::kPaymentCard;

  std::vector<voice::PlannerStep> got = PlannerStepsFor(navigate);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].action, Action::kNavigate);
  EXPECT_EQ(got[0].url, "https://trains.example/book");

  got = PlannerStepsFor(search);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].action, Action::kSearch);
  EXPECT_EQ(got[0].query, "trains");

  got = PlannerStepsFor(Ran("CLICK", {Element("button", "Search")}));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].action, Action::kClick);
  EXPECT_EQ(got[0].host, "trains.example");
  EXPECT_EQ(got[0].kind, "button");
  EXPECT_EQ(got[0].label, "Search");
  EXPECT_FALSE(got[0].credential_field);

  got = PlannerStepsFor(Ran("TYPE", {Element("input", "To", "Lyon")}));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].action, Action::kType);
  EXPECT_EQ(got[0].text, "Lyon");

  got = PlannerStepsFor(Ran("TYPE", {card}));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_TRUE(got[0].credential_field);

  got = PlannerStepsFor(
      Ran("SELECT", {Element("select", "Class", "First class")}));
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].action, Action::kSelect);
  EXPECT_EQ(got[0].option, "First class");
  EXPECT_TRUE(got[0].text.empty());

  scroll.origin.clear();
  got = PlannerStepsFor(scroll);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].action, Action::kScroll);
  EXPECT_EQ(got[0].direction, "down");
  EXPECT_EQ(got[0].host, "");

  // A form: one step per field, in order.
  got = PlannerStepsFor(Ran("FILL_FORM", {Element("input", "From", "Paris"),
                                          Element("input", "To", "Lyon")}));
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].label, "From");
  EXPECT_EQ(got[1].text, "Lyon");

  // Gaps: a site's tool, a form filled in part, an element step without
  // its element.
  for (const RunStep& gap : {Ran("CALL_TOOL"), Ran("FILL_FORM"), Ran("CLICK"),
                             Ran("TYPE", {Element("input", "A", "x"),
                                          Element("input", "B", "y")})}) {
    got = PlannerStepsFor(gap);
    ASSERT_EQ(got.size(), 1u) << gap.action;
    EXPECT_EQ(got[0].action, Action::kUnkept) << gap.action;
    EXPECT_FALSE(got[0].why.empty()) << gap.action;
  }
  EXPECT_NE(PlannerStepsFor(Ran("CALL_TOOL"))[0].why.find("site's own tool"),
            std::string::npos);

  // Nothing for what changed no page, or failed.
  for (const char* none : {"OBSERVE", "REPLAN", "DONE", "ERROR"}) {
    EXPECT_TRUE(PlannerStepsFor(Ran(none)).empty()) << none;
  }
  EXPECT_TRUE(
      PlannerStepsFor(Ran("CLICK", {Element("button", "Go")}, false)).empty());
  EXPECT_TRUE(PlannerStepsFor(Ran("CALL_TOOL", {}, false)).empty());
  // Not a web page: no host.
  RunStep local = Ran("CLICK", {Element("button", "Go")});
  local.origin = "chrome://settings";
  EXPECT_EQ(PlannerStepsFor(local)[0].host, "");
}

TEST(VoiceAgentReplayTest, FindsTheElementAgainByKindAndName) {
  const Observation page = BookingPage();

  ReplayPick pick =
      PickReplayAction(Step(Action::kClick, "button", "Search"), page);
  EXPECT_EQ(pick.outcome, Outcome::kAct);
  agent_actions::AgentAction action = Parsed(pick, page);
  EXPECT_EQ(action.type, ActionType::kClick);
  EXPECT_EQ(action.ref, "e4");

  voice::PlannerStep type = Step(Action::kType, "input", "To");
  type.text = "Lyon\nand back";
  pick = PickReplayAction(type, page);
  ASSERT_EQ(pick.outcome, Outcome::kAct);
  action = Parsed(pick, page);
  EXPECT_EQ(action.type, ActionType::kType);
  EXPECT_EQ(action.ref, "e3");
  EXPECT_EQ(action.text, "Lyon\nand back");

  voice::PlannerStep select = Step(Action::kSelect, "select", "Class");
  select.option = "First class";
  pick = PickReplayAction(select, page);
  ASSERT_EQ(pick.outcome, Outcome::kAct);
  action = Parsed(pick, page);
  EXPECT_EQ(action.type, ActionType::kSelect);
  EXPECT_EQ(action.ref, "e7");
  EXPECT_EQ(action.option, "First class");

  // Nothing is guessed: another name, another kind, the name's case, a
  // field that isn't a text field, an option the select doesn't list.
  select.option = "Sleeper";
  for (const voice::PlannerStep& missing :
       {Step(Action::kClick, "button", "Book"),
        Step(Action::kClick, "link", "Search"),
        Step(Action::kClick, "button", "search"),
        Step(Action::kType, "input", "Promo"), select}) {
    pick = PickReplayAction(missing, page);
    EXPECT_EQ(pick.outcome, Outcome::kNotFound) << missing.label;
    EXPECT_EQ(Parsed(pick, page).type, ActionType::kError);
  }
  // Two links named "More": which one is the step's can't be told.
  pick = PickReplayAction(Step(Action::kClick, "link", "More"), page);
  EXPECT_EQ(pick.outcome, Outcome::kAmbiguous);
  EXPECT_EQ(Parsed(pick, page).type, ActionType::kError);
  // A gap is never replayed.
  pick = PickReplayAction(Step(Action::kUnkept, "", ""), page);
  EXPECT_EQ(pick.outcome, Outcome::kNotFound);
}

TEST(VoiceAgentReplayTest, PagesAndScrolls) {
  const Observation page = BookingPage();
  voice::PlannerStep navigate;
  navigate.action = Action::kSearch;
  navigate.url = "https://search.brave.com/search?q=trains+to+lyon";
  navigate.query = "trains to lyon";
  ReplayPick pick = PickReplayAction(navigate, page);
  ASSERT_EQ(pick.outcome, Outcome::kAct);
  agent_actions::AgentAction action = Parsed(pick, page);
  EXPECT_EQ(action.type, ActionType::kNavigate);
  EXPECT_EQ(action.url, navigate.url);

  voice::PlannerStep scroll;
  scroll.action = Action::kScroll;
  scroll.direction = "up";
  action = Parsed(PickReplayAction(scroll, page), page);
  EXPECT_EQ(action.type, ActionType::kScroll);
  EXPECT_EQ(action.direction, "up");
  scroll.direction = "sideways";
  EXPECT_EQ(Parsed(PickReplayAction(scroll, page), page).direction, "down");

  // The replay ends: DONE once the step ran, else ERROR.
  ReplayPick end;
  end.reply = EndReplayReply(true);
  EXPECT_EQ(Parsed(end, page).type, ActionType::kDone);
  end.reply = EndReplayReply(false);
  EXPECT_EQ(Parsed(end, page).type, ActionType::kError);
}

}  // namespace
}  // namespace voice_agent
}  // namespace molt_ai
