// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_AGENTS_AGENT_ENGINE_H_
#define MOLT_AI_AGENTS_AGENT_ENGINE_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "src/molt_ai/dom/dom_interpreter.h"
#include "src/molt_ai/runtime/browser_ai_runtime.h"

namespace molt_ai {

// Agent action types (from V4 spec)
enum class ActionType {
  CLICK,
  SCROLL,
  NAVIGATE,
  FILL_FORM,
  EXTRACT_DATA,
  OPEN_TAB,
  CLOSE_TAB,
  SCREENSHOT,
  WAIT,
  TYPE_TEXT,
  DONE,
  ERROR
};

// Single agent action
struct AgentAction {
  ActionType type;
  std::string target;        // CSS selector or URL
  std::string value;         // Text to type, data to fill, etc.
  int x = 0;                 // Click coordinates
  int y = 0;
  int wait_ms = 0;           // Wait duration
  std::string description;   // Human-readable description
};

// Agent step result
struct StepResult {
  int step_number;
  AgentAction action_taken;
  bool success;
  std::string observation;   // What the agent observed after the action
  std::string reasoning;     // Why the agent chose this action
  StructuredPage page_state; // Page state after action
  float time_ms;
};

// Agent task status
enum class TaskStatus {
  PENDING,
  RUNNING,
  COMPLETED,
  FAILED,
  CANCELLED,
  MAX_ITERATIONS_REACHED
};

// Agent task result
struct AgentTaskResult {
  std::string goal;
  TaskStatus status;
  std::string final_answer;
  std::vector<StepResult> steps;
  int total_steps;
  float total_time_ms;
  std::string error_message;
};

// Agent task options
struct AgentTaskOptions {
  int max_iterations = 20;     // From V4 spec: max 20 iterations
  int timeout_ms = 120000;     // 2 minute timeout
  bool allow_navigation = true;
  bool allow_form_fill = true;
  bool allow_new_tabs = true;
  std::string model_id;        // Model to use for reasoning
  std::vector<std::string> allowed_domains;  // Domain whitelist
  bool verbose = false;        // Log detailed reasoning
};

// Step callback for progress monitoring
using StepCallback = std::function<void(const StepResult& step)>;

// ============================================================
// AgentEngine — Autonomous browser agent
// ============================================================
// Implements the ReAct (Reason + Act) agent pattern:
//   GOAL → PLAN → ACT → OBSERVE → REASON → LOOP/DONE
//
// The agent operates inside a sandboxed environment, isolated
// from human browsing sessions.
//
// Security constraints:
//   - All actions pass through ActionValidator
//   - Max 20 iterations per task
//   - Domain whitelist enforcement
//   - No credential access
//   - No hidden UI manipulation
// ============================================================
class AgentEngine {
 public:
  AgentEngine(BrowserAIRuntime* runtime, DOMInterpreter* dom_interpreter);
  ~AgentEngine();

  // Disallow copy/assign
  AgentEngine(const AgentEngine&) = delete;
  AgentEngine& operator=(const AgentEngine&) = delete;

  // ---- Task Execution ----

  // Execute an autonomous task with a natural language goal
  AgentTaskResult ExecuteTask(const std::string& goal,
                               const AgentTaskOptions& options = {},
                               StepCallback step_callback = nullptr);

  // Cancel a running task
  void CancelTask();

  // Get current task status
  TaskStatus GetTaskStatus() const;

  // ---- Action Execution ----

  // Execute a single agent action in the browser
  bool ExecuteAction(const AgentAction& action, int tab_id);

  // ---- Planning ----

  // Generate a plan for achieving a goal
  std::vector<std::string> GeneratePlan(const std::string& goal,
                                         const StructuredPage& current_page);

  // Decide next action based on current state and goal
  AgentAction DecideNextAction(const std::string& goal,
                                const std::string& plan,
                                const std::vector<StepResult>& history,
                                const StructuredPage& current_page);

 private:
  BrowserAIRuntime* runtime_;       // Not owned
  DOMInterpreter* dom_interpreter_; // Not owned
  TaskStatus current_status_;
  bool cancel_requested_;

  // Build the reasoning prompt for the LLM
  std::string BuildReasoningPrompt(const std::string& goal,
                                    const std::string& plan,
                                    const std::vector<StepResult>& history,
                                    const StructuredPage& current_page) const;

  // Parse LLM output into an AgentAction
  AgentAction ParseActionFromLLM(const std::string& llm_output) const;

  // Observe the page state after an action
  std::string ObservePage(int tab_id) const;
};

}  // namespace molt_ai

#endif  // MOLT_AI_AGENTS_AGENT_ENGINE_H_
