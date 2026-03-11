// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "src/molt_ai/agents/agent_engine.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace molt_ai {

AgentEngine::AgentEngine(BrowserAIRuntime* runtime,
                         DOMInterpreter* dom_interpreter)
    : runtime_(runtime),
      dom_interpreter_(dom_interpreter),
      current_status_(TaskStatus::PENDING),
      cancel_requested_(false) {}

AgentEngine::~AgentEngine() {
  CancelTask();
}

AgentTaskResult AgentEngine::ExecuteTask(const std::string& goal,
                                          const AgentTaskOptions& options,
                                          StepCallback step_callback) {
  AgentTaskResult result;
  result.goal = goal;
  result.status = TaskStatus::RUNNING;
  current_status_ = TaskStatus::RUNNING;
  cancel_requested_ = false;

  auto task_start = std::chrono::steady_clock::now();

  // Step 1: Observe current page
  StructuredPage current_page = dom_interpreter_->ParseTab(0);

  // Step 2: Generate plan
  auto plan_steps = GeneratePlan(goal, current_page);
  std::string plan;
  for (size_t i = 0; i < plan_steps.size(); ++i) {
    plan += std::to_string(i + 1) + ". " + plan_steps[i] + "\n";
  }

  // Step 3: Execute ReAct loop
  // GOAL → PLAN → ACT → OBSERVE → REASON → LOOP/DONE
  for (int iteration = 0; iteration < options.max_iterations; ++iteration) {
    if (cancel_requested_) {
      result.status = TaskStatus::CANCELLED;
      break;
    }

    // Check timeout
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - task_start).count();
    if (elapsed > options.timeout_ms) {
      result.status = TaskStatus::MAX_ITERATIONS_REACHED;
      result.error_message = "Task timed out";
      break;
    }

    StepResult step;
    step.step_number = iteration + 1;
    auto step_start = std::chrono::steady_clock::now();

    // REASON: Decide next action
    AgentAction action = DecideNextAction(goal, plan, result.steps, current_page);
    step.action_taken = action;

    // Check if agent decided it's done
    if (action.type == ActionType::DONE) {
      step.success = true;
      step.observation = "Task completed";
      step.reasoning = action.description;
      result.final_answer = action.value;
      result.status = TaskStatus::COMPLETED;

      auto step_end = std::chrono::steady_clock::now();
      step.time_ms = std::chrono::duration<float, std::milli>(
          step_end - step_start).count();
      result.steps.push_back(step);

      if (step_callback) step_callback(step);
      break;
    }

    if (action.type == ActionType::ERROR) {
      step.success = false;
      step.observation = action.description;
      result.status = TaskStatus::FAILED;
      result.error_message = action.description;

      auto step_end = std::chrono::steady_clock::now();
      step.time_ms = std::chrono::duration<float, std::milli>(
          step_end - step_start).count();
      result.steps.push_back(step);

      if (step_callback) step_callback(step);
      break;
    }

    // ACT: Execute the action
    step.success = ExecuteAction(action, 0);

    // OBSERVE: Get new page state
    current_page = dom_interpreter_->ParseTab(0);
    step.page_state = current_page;
    step.observation = ObservePage(0);
    step.reasoning = action.description;

    auto step_end = std::chrono::steady_clock::now();
    step.time_ms = std::chrono::duration<float, std::milli>(
        step_end - step_start).count();

    result.steps.push_back(step);
    if (step_callback) step_callback(step);

    // Check if max iterations reached
    if (iteration == options.max_iterations - 1) {
      result.status = TaskStatus::MAX_ITERATIONS_REACHED;
      result.error_message = "Maximum iterations reached";
    }
  }

  auto task_end = std::chrono::steady_clock::now();
  result.total_time_ms = std::chrono::duration<float, std::milli>(
      task_end - task_start).count();
  result.total_steps = static_cast<int>(result.steps.size());

  current_status_ = result.status;
  return result;
}

void AgentEngine::CancelTask() {
  cancel_requested_ = true;
}

TaskStatus AgentEngine::GetTaskStatus() const {
  return current_status_;
}

bool AgentEngine::ExecuteAction(const AgentAction& action, int tab_id) {
  // TODO: Integration with Chromium's automation APIs
  // Each action type maps to browser automation commands:
  //
  // CLICK      → SimulateMouseClick(x, y) or element.click()
  // SCROLL     → window.scrollBy() or SimulateMouseWheel()
  // NAVIGATE   → LoadURL(action.target)
  // FILL_FORM  → element.value = action.value
  // EXTRACT    → DOM query and data extraction
  // OPEN_TAB   → Browser::CreateTab(action.target)
  // CLOSE_TAB  → TabStripModel::CloseTabAt(tab_id)
  // SCREENSHOT → RenderWidgetHost::CopyFromSurface()
  // WAIT       → base::ThreadTaskRunnerHandle::PostDelayedTask()
  // TYPE_TEXT  → SimulateKeyboardInput(action.value)

  switch (action.type) {
    case ActionType::CLICK:
      // TODO: Implement click action
      return true;

    case ActionType::SCROLL:
      // TODO: Implement scroll action
      return true;

    case ActionType::NAVIGATE:
      // TODO: Implement navigation
      return true;

    case ActionType::FILL_FORM:
      // TODO: Implement form filling
      return true;

    case ActionType::EXTRACT_DATA:
      // TODO: Implement data extraction
      return true;

    case ActionType::OPEN_TAB:
      // TODO: Implement tab opening
      return true;

    case ActionType::CLOSE_TAB:
      // TODO: Implement tab closing
      return true;

    case ActionType::SCREENSHOT:
      // TODO: Implement screenshot capture
      return true;

    case ActionType::WAIT:
      // Simple wait implementation
      if (action.wait_ms > 0 && action.wait_ms <= 10000) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(action.wait_ms));
      }
      return true;

    case ActionType::TYPE_TEXT:
      // TODO: Implement keyboard input
      return true;

    default:
      return false;
  }
}

std::vector<std::string> AgentEngine::GeneratePlan(
    const std::string& goal,
    const StructuredPage& current_page) {
  std::string prompt =
      "You are a browser automation agent. Given the following goal and "
      "current page state, generate a step-by-step plan to achieve the goal.\n\n"
      "GOAL: " + goal + "\n\n"
      "CURRENT PAGE:\n" + current_page.ToCompactText() + "\n\n"
      "Generate a numbered plan with 3-8 steps. Each step should be a "
      "concrete browser action (click, navigate, type, extract, etc.).\n\n"
      "PLAN:";

  PromptOptions opts;
  opts.max_tokens = 256;
  opts.temperature = 0.3f;

  auto result = runtime_->RunPrompt(prompt, opts);

  // Parse plan steps from LLM output
  std::vector<std::string> steps;
  std::istringstream stream(result.text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && (line[0] >= '1' && line[0] <= '9')) {
      // Remove number prefix
      size_t dot_pos = line.find('.');
      if (dot_pos != std::string::npos && dot_pos < 3) {
        line = line.substr(dot_pos + 1);
        // Trim leading space
        if (!line.empty() && line[0] == ' ') {
          line = line.substr(1);
        }
      }
      steps.push_back(line);
    }
  }

  return steps;
}

AgentAction AgentEngine::DecideNextAction(
    const std::string& goal,
    const std::string& plan,
    const std::vector<StepResult>& history,
    const StructuredPage& current_page) {
  std::string prompt = BuildReasoningPrompt(goal, plan, history, current_page);

  PromptOptions opts;
  opts.max_tokens = 200;
  opts.temperature = 0.2f;

  auto result = runtime_->RunPrompt(prompt, opts);

  return ParseActionFromLLM(result.text);
}

std::string AgentEngine::BuildReasoningPrompt(
    const std::string& goal,
    const std::string& plan,
    const std::vector<StepResult>& history,
    const StructuredPage& current_page) const {
  std::ostringstream prompt;

  prompt << "You are an autonomous browser agent. Your task is to achieve "
         << "the given goal by taking browser actions.\n\n";

  prompt << "GOAL: " << goal << "\n\n";
  prompt << "PLAN:\n" << plan << "\n\n";

  // Include recent history (last 5 steps to fit in context)
  if (!history.empty()) {
    prompt << "HISTORY (last " << std::min(history.size(), size_t(5))
           << " steps):\n";
    size_t start = history.size() > 5 ? history.size() - 5 : 0;
    for (size_t i = start; i < history.size(); ++i) {
      const auto& step = history[i];
      prompt << "Step " << step.step_number << ": "
             << step.action_taken.description
             << " → " << (step.success ? "Success" : "Failed")
             << " | " << step.observation << "\n";
    }
    prompt << "\n";
  }

  prompt << "CURRENT PAGE:\n" << current_page.ToCompactText() << "\n\n";

  prompt << "Decide the next action. Respond in this exact format:\n"
         << "ACTION: <click|scroll|navigate|fill_form|extract_data|"
         << "open_tab|close_tab|wait|type_text|done>\n"
         << "TARGET: <css_selector or url>\n"
         << "VALUE: <text or data, if applicable>\n"
         << "REASON: <why this action>\n";

  return prompt.str();
}

AgentAction AgentEngine::ParseActionFromLLM(
    const std::string& llm_output) const {
  AgentAction action;

  // Parse the structured output from the LLM
  auto get_field = [&llm_output](const std::string& field) -> std::string {
    std::string prefix = field + ": ";
    size_t pos = llm_output.find(prefix);
    if (pos == std::string::npos) return "";
    pos += prefix.length();
    size_t end = llm_output.find('\n', pos);
    if (end == std::string::npos) end = llm_output.length();
    return llm_output.substr(pos, end - pos);
  };

  std::string action_str = get_field("ACTION");
  action.target = get_field("TARGET");
  action.value = get_field("VALUE");
  action.description = get_field("REASON");

  // Map action string to ActionType
  if (action_str == "click") action.type = ActionType::CLICK;
  else if (action_str == "scroll") action.type = ActionType::SCROLL;
  else if (action_str == "navigate") action.type = ActionType::NAVIGATE;
  else if (action_str == "fill_form") action.type = ActionType::FILL_FORM;
  else if (action_str == "extract_data") action.type = ActionType::EXTRACT_DATA;
  else if (action_str == "open_tab") action.type = ActionType::OPEN_TAB;
  else if (action_str == "close_tab") action.type = ActionType::CLOSE_TAB;
  else if (action_str == "wait") {
    action.type = ActionType::WAIT;
    try { action.wait_ms = std::stoi(action.value); }
    catch (...) { action.wait_ms = 1000; }
  }
  else if (action_str == "type_text") action.type = ActionType::TYPE_TEXT;
  else if (action_str == "done") action.type = ActionType::DONE;
  else action.type = ActionType::ERROR;

  return action;
}

std::string AgentEngine::ObservePage(int tab_id) const {
  auto page = dom_interpreter_->ParseTab(tab_id);
  return "Page: " + page.page_title + " | URL: " + page.page_url +
         " | Buttons: " + std::to_string(page.buttons.size()) +
         " | Links: " + std::to_string(page.links.size()) +
         " | Forms: " + std::to_string(page.forms.size());
}

}  // namespace molt_ai
