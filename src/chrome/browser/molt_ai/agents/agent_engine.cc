// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_engine.h"

#include <chrono>
#include <ratio>
#include <sstream>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/molt_ai/security/action_validator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

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

  LOG(INFO) << "[MoltAI Agent] Starting task: " << goal;

  auto task_start = std::chrono::steady_clock::now();

  // Step 1: Observe current page
  StructuredPage current_page = dom_interpreter_->ParseTab(0);

  // Step 2: Generate plan
  auto plan_steps = GeneratePlan(goal, current_page);
  std::string plan;
  for (size_t i = 0; i < plan_steps.size(); ++i) {
    plan += std::to_string(i + 1) + ". " + plan_steps[i] + "\n";
  }

  LOG(INFO) << "[MoltAI Agent] Plan generated with " << plan_steps.size()
            << " steps";

  // Step 3: Execute ReAct loop
  // GOAL → PLAN → ACT → OBSERVE → REASON → LOOP/DONE
  for (int iteration = 0; iteration < options.max_iterations; ++iteration) {
    if (cancel_requested_) {
      result.status = TaskStatus::CANCELLED;
      LOG(INFO) << "[MoltAI Agent] Task cancelled at step " << iteration + 1;
      break;
    }

    // Check timeout
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - task_start).count();
    if (elapsed > options.timeout_ms) {
      result.status = TaskStatus::MAX_ITERATIONS_REACHED;
      result.error_message = "Task timed out after " +
          std::to_string(elapsed / 1000) + " seconds";
      break;
    }

    StepResult step;
    step.step_number = iteration + 1;
    auto step_start = std::chrono::steady_clock::now();

    // REASON: Decide next action
    AgentAction action = DecideNextAction(goal, plan, result.steps, current_page);
    step.action_taken = action;

    LOG(INFO) << "[MoltAI Agent] Step " << step.step_number << ": "
              << action.description;

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

    // VALIDATE: Security check before execution
    ActionValidator validator;
    if (!validator.ValidateAction(action, options)) {
      step.success = false;
      step.observation = "Action blocked by security validator";
      step.reasoning = "Action violated security constraints";
      result.steps.push_back(step);
      if (step_callback) step_callback(step);
      continue;
    }

    // ACT: Execute the action in the browser
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
  LOG(INFO) << "[MoltAI Agent] Task finished: "
            << (result.status == TaskStatus::COMPLETED ? "SUCCESS" : "FAILED")
            << " in " << result.total_steps << " steps";
  return result;
}

void AgentEngine::CancelTask() {
  cancel_requested_ = true;
}

TaskStatus AgentEngine::GetTaskStatus() const {
  return current_status_;
}

bool AgentEngine::ExecuteAction(const AgentAction& action, int tab_id) {
  content::WebContents* web_contents = GetWebContentsForTab(tab_id);
  if (!web_contents && action.type != ActionType::WAIT) {
    LOG(ERROR) << "[MoltAI Agent] No web contents for tab " << tab_id;
    return false;
  }

  content::RenderFrameHost* rfh = web_contents
      ? web_contents->GetPrimaryMainFrame()
      : nullptr;

  switch (action.type) {
    case ActionType::CLICK: {
      if (!rfh) return false;
      // Click via JavaScript on CSS selector or coordinates
      std::string script;
      if (!action.target.empty()) {
        // CSS selector click
        script =
            "(() => {"
            "  const el = document.querySelector('" + action.target + "');"
            "  if (!el) return 'NOT_FOUND';"
            "  el.scrollIntoView({block: 'center'});"
            "  el.click();"
            "  return 'CLICKED';"
            "})()";
      } else if (action.x > 0 || action.y > 0) {
        // Coordinate-based click
        script =
            "(() => {"
            "  const el = document.elementFromPoint(" +
            std::to_string(action.x) + "," + std::to_string(action.y) + ");"
            "  if (!el) return 'NOT_FOUND';"
            "  el.click();"
            "  return 'CLICKED';"
            "})()";
      } else {
        return false;
      }
      rfh->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::DoNothing(),
          content::ISOLATED_WORLD_ID_CONTENT_END);
      return true;
    }

    case ActionType::SCROLL: {
      if (!rfh) return false;
      std::string direction = action.value.empty() ? "down" : action.value;
      int amount = 400;
      std::string script;
      if (direction == "up") {
        script = "window.scrollBy(0, -" + std::to_string(amount) + ")";
      } else if (direction == "down") {
        script = "window.scrollBy(0, " + std::to_string(amount) + ")";
      } else if (direction == "top") {
        script = "window.scrollTo(0, 0)";
      } else if (direction == "bottom") {
        script = "window.scrollTo(0, document.body.scrollHeight)";
      } else {
        script = "window.scrollBy(0, " + std::to_string(amount) + ")";
      }
      rfh->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::DoNothing(),
          content::ISOLATED_WORLD_ID_CONTENT_END);
      return true;
    }

    case ActionType::NAVIGATE: {
      if (!action.target.empty()) {
        GURL url(action.target);
        if (url.is_valid()) {
          content::NavigationController::LoadURLParams params(url);
          params.transition_type = ui::PAGE_TRANSITION_TYPED;
          web_contents->GetController().LoadURLWithParams(params);
          return true;
        }
      }
      return false;
    }

    case ActionType::FILL_FORM: {
      if (!rfh || action.target.empty()) return false;
      // Set value on form element identified by CSS selector
      std::string escaped_value = action.value;
      // Escape single quotes in value
      size_t pos = 0;
      while ((pos = escaped_value.find('\'', pos)) != std::string::npos) {
        escaped_value.replace(pos, 1, "\\'");
        pos += 2;
      }
      std::string script =
          "(() => {"
          "  const el = document.querySelector('" + action.target + "');"
          "  if (!el) return 'NOT_FOUND';"
          "  el.focus();"
          "  el.value = '" + escaped_value + "';"
          "  el.dispatchEvent(new Event('input', {bubbles: true}));"
          "  el.dispatchEvent(new Event('change', {bubbles: true}));"
          "  return 'FILLED';"
          "})()";
      rfh->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::DoNothing(),
          content::ISOLATED_WORLD_ID_CONTENT_END);
      return true;
    }

    case ActionType::EXTRACT_DATA: {
      if (!rfh) return false;
      // Extract text content from CSS selector
      std::string script =
          "(() => {"
          "  const el = document.querySelector('" + action.target + "');"
          "  if (!el) return '';"
          "  return el.innerText || el.textContent || '';"
          "})()";
      rfh->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::DoNothing(),
          content::ISOLATED_WORLD_ID_CONTENT_END);
      return true;
    }

    case ActionType::OPEN_TAB: {
      if (!rfh) return false;
      // Open URL in new tab via window.open
      std::string script = "window.open('" + action.target + "', '_blank')";
      rfh->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::DoNothing(),
          content::ISOLATED_WORLD_ID_CONTENT_END);
      return true;
    }

    case ActionType::CLOSE_TAB: {
      if (!rfh) return false;
      std::string script = "window.close()";
      rfh->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::DoNothing(),
          content::ISOLATED_WORLD_ID_CONTENT_END);
      return true;
    }

    case ActionType::SCREENSHOT: {
      // Screenshot capture via RenderWidgetHost::CopyFromSurface
      // Stored in agent step result for the LLM to reference
      LOG(INFO) << "[MoltAI Agent] Screenshot captured (placeholder)";
      return true;
    }

    case ActionType::WAIT: {
      int wait_ms = action.wait_ms > 0 ? action.wait_ms : 1000;
      if (wait_ms > 10000) wait_ms = 10000;  // Max 10 seconds
      // Use base::PlatformThread::Sleep instead of std::this_thread
      base::PlatformThread::Sleep(base::Milliseconds(wait_ms));
      return true;
    }

    case ActionType::TYPE_TEXT: {
      if (!rfh || action.target.empty()) return false;
      // Focus element and type text character by character with input events
      std::string escaped_value = action.value;
      size_t pos = 0;
      while ((pos = escaped_value.find('\'', pos)) != std::string::npos) {
        escaped_value.replace(pos, 1, "\\'");
        pos += 2;
      }
      std::string script =
          "(() => {"
          "  const el = document.querySelector('" + action.target + "');"
          "  if (!el) return 'NOT_FOUND';"
          "  el.focus();"
          "  const text = '" + escaped_value + "';"
          "  for (let i = 0; i < text.length; i++) {"
          "    el.value += text[i];"
          "    el.dispatchEvent(new InputEvent('input', {"
          "      bubbles: true, data: text[i], inputType: 'insertText'"
          "    }));"
          "  }"
          "  el.dispatchEvent(new Event('change', {bubbles: true}));"
          "  return 'TYPED';"
          "})()";
      rfh->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::DoNothing(),
          content::ISOLATED_WORLD_ID_CONTENT_END);
      return true;
    }

    default:
      return false;
  }
}

content::WebContents* AgentEngine::GetWebContentsForTab(int tab_id) const {
  // The WebContents is obtained from the Browser's TabStripModel.
  // The caller (typically MoltAIChatHandler) should set this via
  // a method or constructor parameter. For now, we use the
  // DOMInterpreter's cached reference if available.
  return dom_interpreter_->GetWebContents(tab_id);
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
      size_t dot_pos = line.find('.');
      if (dot_pos != std::string::npos && dot_pos < 3) {
        line = line.substr(dot_pos + 1);
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

  if (!history.empty()) {
    prompt << "HISTORY (last " << std::min(history.size(), size_t(5))
           << " steps):\n";
    size_t start = history.size() > 5 ? history.size() - 5 : 0;
    for (size_t i = start; i < history.size(); ++i) {
      const auto& step = history[i];
      prompt << "Step " << step.step_number << ": "
             << step.action_taken.description
             << " -> " << (step.success ? "Success" : "Failed")
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

  if (action_str == "click") action.type = ActionType::CLICK;
  else if (action_str == "scroll") action.type = ActionType::SCROLL;
  else if (action_str == "navigate") action.type = ActionType::NAVIGATE;
  else if (action_str == "fill_form") action.type = ActionType::FILL_FORM;
  else if (action_str == "extract_data") action.type = ActionType::EXTRACT_DATA;
  else if (action_str == "open_tab") action.type = ActionType::OPEN_TAB;
  else if (action_str == "close_tab") action.type = ActionType::CLOSE_TAB;
  else if (action_str == "wait") {
    action.type = ActionType::WAIT;
    char* end_ptr = nullptr;
    long val = std::strtol(action.value.c_str(), &end_ptr, 10);
    action.wait_ms = (end_ptr != action.value.c_str() && val > 0)
                         ? static_cast<int>(val) : 1000;
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
