// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's prompt (docs/design/AGENT_ACTIONS.md §2.1 and §2.6).
//
// Two parts: the rules (MoltBrowser's own text) and the turn (the user's
// goal, then the history and the current page, both page-derived and
// fenced with agent_safety::FenceUntrustedText). Without a screenshot the
// model gets them as one raw prompt, exactly as before. With one, it gets
// them as a chat: the rules as the system prompt, the turn as the user
// message, and the screenshot with the user message only (an image is
// page content: untrusted, like page text, never in the system prompt).
//
// The planner (agent_plan.h, docs/design/AGENT_ACTIONS.md §2.8): with a
// plan, the turn also shows it after the GOAL line, fenced like page text
// (the model wrote it from the goal and from pages), and the rules describe
// "step" and REPLAN; without one the prompt is what it was. The plan itself
// is written from BuildPlanPrompt().
//
// Pure logic (no content/), unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PROMPT_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PROMPT_H_

#include <string>
#include <vector>

namespace molt_ai {
namespace agent_prompt {

// A completed step, as the history shows it (WebAgent::Step).
struct HistoryStep {
  HistoryStep();
  ~HistoryStep();
  HistoryStep(const HistoryStep&);
  HistoryStep& operator=(const HistoryStep&);

  int number = 0;
  std::string action;
  std::string target;
  std::string value;
  bool success = false;
  std::string note;
  std::string observation;
};

struct PromptInput {
  PromptInput();
  ~PromptInput();
  PromptInput(const PromptInput&);
  PromptInput& operator=(const PromptInput&);

  // The user's goal (trusted).
  std::string goal;
  std::vector<HistoryStep> history;
  // The current observation: URL, title, page text and the ELEMENTS list.
  // Page-controlled; fenced here.
  std::string page_state;
  int max_steps = 0;
  int steps_remaining = 0;
  // Why the last reply couldn't be used (agent_actions::InterpretReply),
  // or empty.
  std::string retry_note;
  // A screenshot of the current page goes with this prompt.
  bool with_screenshot = false;
  // The current page offers tools (a TOOLS list in |page_state|): the
  // rules describe CALL_TOOL. Without, the prompt doesn't mention it.
  bool with_tools = false;
  // The run's plan (agent_plan::DescribePlan(): model output, written from
  // the goal and page text), fenced here; empty without a plan, and then
  // the prompt doesn't mention plans.
  std::string plan;
  // The browser's line about the current step
  // (agent_plan::PlanTracker::DescribeProgress()), after the fence.
  std::string plan_progress;
};

struct AgentPrompt {
  AgentPrompt();
  ~AgentPrompt();
  AgentPrompt(const AgentPrompt&);
  AgentPrompt& operator=(const AgentPrompt&);
  AgentPrompt(AgentPrompt&&);
  AgentPrompt& operator=(AgentPrompt&&);

  // The rules: the system prompt of a chat request.
  std::string system;
  // The goal, history and current page: the user message (the screenshot,
  // if any, goes with it).
  std::string user;
};

AgentPrompt BuildAgentPrompt(const PromptInput& input);

// The request for a plan (agent_plan.h): before the first action, and when
// the plan is rewritten.
struct PlanPromptInput {
  PlanPromptInput();
  ~PlanPromptInput();
  PlanPromptInput(const PlanPromptInput&);
  PlanPromptInput& operator=(const PlanPromptInput&);

  // The user's goal (trusted).
  std::string goal;
  std::vector<HistoryStep> history;
  // The current observation, as PromptInput::page_state. Fenced here.
  std::string page_state;
  // When the plan is rewritten: the plan that stopped working
  // (agent_plan::DescribePlan(), model output, fenced here) and the
  // browser's words for why (agent_plan::ReplanReasonText()). Both empty
  // for the first plan.
  std::string earlier_plan;
  std::string replan_reason;
  // Why the last reply couldn't be used (agent_plan::InterpretPlanReply),
  // or empty.
  std::string retry_note;
  // agent_plan::kMaxPlanSteps.
  int max_plan_steps = 0;
};

// The rules (system) and the goal, history, earlier plan and page (user),
// as for an action. Always text-only (ToRawPrompt()).
AgentPrompt BuildPlanPrompt(const PlanPromptInput& input);

// The text-only request: one raw prompt, rules and turn split by "---".
std::string ToRawPrompt(const AgentPrompt& prompt);

}  // namespace agent_prompt
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PROMPT_H_
