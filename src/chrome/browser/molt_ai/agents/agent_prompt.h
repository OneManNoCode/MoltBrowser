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

// The text-only request: one raw prompt, rules and turn split by "---".
std::string ToRawPrompt(const AgentPrompt& prompt);

}  // namespace agent_prompt
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_PROMPT_H_
