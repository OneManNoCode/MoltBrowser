// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_runner.h"

#include <ctime>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {

namespace {

// JS that searches for a selector and returns true if it matches at least
// one visible element, used by WAIT_FOR.
const char kHasSelectorJSTemplate[] =
    R"((() => {
      try {
        const el = document.querySelector(%s);
        if (!el) return false;
        const r = el.getBoundingClientRect();
        return r.width > 0 && r.height > 0;
      } catch (e) { return false; }
    })())";

// Click a selector, returns true on success.
const char kClickSelectorJSTemplate[] =
    R"((() => {
      const el = document.querySelector(%s);
      if (!el) return false;
      el.scrollIntoView({block: 'center'});
      el.click();
      return true;
    })())";

// Set the value of an input/textarea matching selector and dispatch input
// + change events so frameworks pick it up.
const char kTypeIntoJSTemplate[] =
    R"((() => {
      const el = document.querySelector(%s);
      if (!el) return false;
      el.focus();
      const native = Object.getOwnPropertyDescriptor(
          el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype
                                     : HTMLInputElement.prototype, 'value');
      if (native && native.set) native.set.call(el, %s);
      else el.value = %s;
      el.dispatchEvent(new Event('input',  {bubbles: true}));
      el.dispatchEvent(new Event('change', {bubbles: true}));
      return true;
    })())";

const char kScrollByJSTemplate[] =
    "(() => { window.scrollBy(0, %d); return true; })()";

const char kExtractJSTemplate[] =
    R"((() => {
      const els = Array.from(document.querySelectorAll(%s));
      return els.map(e => (e.textContent || '').trim());
    })())";

// JSON-encode a string for safe embedding in JS source.
std::string JSQuote(const std::string& s) {
  std::string out;
  base::JSONWriter::Write(base::Value(s), &out);
  return out;
}

}  // namespace

AutomationRunner::AutomationRunner(content::WebContents* target_contents,
                                    BrowserAIRuntime* ai_runtime,
                                    AutomationStorage* storage)
    : target_contents_(target_contents),
      ai_runtime_(ai_runtime),
      storage_(storage) {}

AutomationRunner::~AutomationRunner() = default;

void AutomationRunner::Run(Script script,
                            StepProgressCallback on_step,
                            RunCompleteCallback on_complete,
                            size_t start_index) {
  if (is_running_) {
    if (on_complete) {
      std::move(on_complete).Run(
          {false, "Runner busy", 0, 0, 0});
    }
    return;
  }
  script_ = std::move(script);
  // Day 6: support resume-at-step. Clamp to a valid index — out-of-range
  // start values fall back to 0 to preserve the existing behavior.
  current_index_ = (start_index < script_.steps.size()) ? start_index : 0;
  is_running_ = true;
  cancel_requested_ = false;
  current_run_tokens_ = 0;
  variables_.clear();
  // Seed user-supplied default variables so {{name}} substitution
  // works without an upstream EXTRACT step.
  for (const auto& kv : script_.default_variables)
    variables_[kv.first] = base::Value(kv.second);
  loop_stack_.clear();
  if_stack_.clear();
  start_time_ = base::TimeTicks::Now();
  on_step_ = std::move(on_step);
  on_complete_ = std::move(on_complete);

  if (storage_) {
    std::string extra =
        (current_index_ > 0)
            ? "from_step=" + base::NumberToString(current_index_)
            : "";
    storage_->AppendAudit(script_.id, "run_started", extra);
  }

  ExecuteNextStep();
}

void AutomationRunner::Cancel() {
  cancel_requested_ = true;
}

void AutomationRunner::ExecuteNextStep() {
  if (cancel_requested_) {
    Finish(false, "Cancelled by user");
    return;
  }
  if (current_index_ >= script_.steps.size()) {
    Finish(true, "Completed");
    return;
  }
  // Per-script overall timeout.
  int elapsed_s = (base::TimeTicks::Now() - start_time_).InSeconds();
  if (elapsed_s > script_.security.max_runtime_seconds) {
    Finish(false, "Max runtime exceeded");
    return;
  }

  const Step& s = script_.steps[current_index_];
  bool execute = ShouldExecuteCurrentStep();

  // Control-flow steps always run for stack management. Their effect
  // when in a not-taken branch is adjusted (e.g. nested IF pushes
  // {false} so its body is also skipped, LOOP pushes 0 iterations).
  switch (s.type) {
    case StepType::IF: {
      bool cond = execute ? EvaluateIfCondition(s) : false;
      if_stack_.push_back({cond, /*in_else=*/false});
      OnStepFinished(true, cond ? "If: true" : "If: false");
      return;
    }
    case StepType::ELSE: {
      if (!if_stack_.empty()) if_stack_.back().in_else = true;
      OnStepFinished(true, "Else");
      return;
    }
    case StepType::END_IF: {
      if (!if_stack_.empty()) if_stack_.pop_back();
      OnStepFinished(true, "End if");
      return;
    }
    case StepType::LOOP: {
      // Push frame; if outer skip is on, push 0 iterations so we exit fast.
      int iters = execute ? std::max(1, s.max_iterations) : 0;
      loop_stack_.push_back({current_index_, iters});
      OnStepFinished(true, "Loop start (" + base::NumberToString(iters) + ")");
      return;
    }
    case StepType::END_LOOP: {
      if (loop_stack_.empty()) {
        OnStepFinished(false, "END_LOOP without LOOP");
        return;
      }
      auto& top = loop_stack_.back();
      top.remaining -= 1;
      if (top.remaining > 0 && execute) {
        // Jump back; OnStepFinished increments past the LOOP step.
        current_index_ = top.start_index;
        OnStepFinished(true, "Loop continue");
      } else {
        loop_stack_.pop_back();
        OnStepFinished(true, "Loop done");
      }
      return;
    }
    default:
      break;
  }

  // Non-control-flow step: skip if any IF frame is in not-taken branch.
  if (!execute) {
    EmitStepProgress(/*starting=*/true, /*succeeded=*/false, "(skipped)");
    EmitStepProgress(/*starting=*/false, /*succeeded=*/true, "skipped");
    ++current_index_;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&AutomationRunner::ExecuteNextStep,
                       weak_factory_.GetWeakPtr()));
    return;
  }

  EmitStepProgress(/*starting=*/true, /*succeeded=*/false, "");
  ArmStepTimeout(s);

  switch (s.type) {
    case StepType::NAVIGATE:    DoNavigate(s); break;
    case StepType::CLICK:       DoClick(s); break;
    case StepType::TYPE:        DoType(s); break;
    case StepType::SCROLL:      DoScroll(s); break;
    case StepType::KEYPRESS:    DoKeypress(s); break;
    case StepType::WAIT:        DoWait(s); break;
    case StepType::WAIT_FOR:    DoWaitFor(s); break;
    case StepType::EXTRACT:     DoExtract(s); break;
    case StepType::AI_DECIDE:   DoAIDecide(s); break;
    case StepType::AI_EXTRACT:  DoAIExtract(s); break;
    case StepType::NOTIFY:      DoNotify(s); break;
    case StepType::SCREENSHOT:  DoScreenshot(s); break;
    case StepType::ASSERT:      DoAssert(s); break;
    case StepType::RUN_JS:      DoRunJS(s); break;
    case StepType::RUN_SCRIPT:  DoRunScript(s); break;
    default:
      OnStepFinished(false, "Unknown step type");
      break;
  }
}

bool AutomationRunner::ShouldExecuteCurrentStep() const {
  for (const auto& f : if_stack_) {
    bool active = (f.condition && !f.in_else) ||
                  (!f.condition && f.in_else);
    if (!active) return false;
  }
  return true;
}

bool AutomationRunner::EvaluateIfCondition(const Step& s) const {
  std::string expr = Resolve(s.value);
  if (expr.empty()) {
    // Empty: take the most recent AI_DECIDE outcome.
    auto it = variables_.find("__ai_decision");
    if (it != variables_.end() && it->second.is_bool())
      return it->second.GetBool();
    return false;
  }
  // Variable lookup: if expr matches a key in variables_, use that value.
  auto it = variables_.find(expr);
  if (it != variables_.end()) {
    if (it->second.is_bool()) return it->second.GetBool();
    if (it->second.is_int()) return it->second.GetInt() != 0;
    if (it->second.is_double()) return it->second.GetDouble() != 0.0;
    if (it->second.is_string()) return !it->second.GetString().empty();
    return !it->second.is_none();
  }
  // Literal: "false"/"0"/"" → false, anything else → true.
  std::string lower = base::ToLowerASCII(expr);
  return lower != "false" && lower != "0" && lower != "no";
}

void AutomationRunner::ArmStepTimeout(const Step& s) {
  step_timeout_timer_.Stop();
  int ms = s.timeout_ms > 0 ? s.timeout_ms : 5000;
  step_timeout_timer_.Start(
      FROM_HERE, base::Milliseconds(ms),
      base::BindOnce(&AutomationRunner::OnStepTimeout,
                     weak_factory_.GetWeakPtr()));
}

void AutomationRunner::OnStepTimeout() {
  // The dispatched step never called OnStepFinished. Fail the step,
  // letting the retry path kick in if Step::retries > 0.
  OnStepFinished(false, "Step timed out");
}

void AutomationRunner::OnStepFinished(bool succeeded, const std::string& note) {
  step_timeout_timer_.Stop();
  EmitStepProgress(/*starting=*/false, succeeded, note);
  if (!succeeded) {
    // Retry-with-backoff: if the failed step asked for retries and we
    // haven't exhausted them, re-dispatch after exponential backoff.
    if (current_index_ < script_.steps.size()) {
      const Step& s = script_.steps[current_index_];
      if (step_attempt_ < s.retries) {
        step_attempt_++;
        int backoff_ms = 250 * (1 << (step_attempt_ - 1));  // 250, 500, 1000...
        if (backoff_ms > 5000) backoff_ms = 5000;
        LOG(INFO) << "[Automation] step " << current_index_
                  << " failed (" << note << "); retry "
                  << step_attempt_ << "/" << s.retries
                  << " in " << backoff_ms << "ms";
        base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&AutomationRunner::ExecuteNextStep,
                           weak_factory_.GetWeakPtr()),
            base::Milliseconds(backoff_ms));
        return;
      }
    }
    // No retries left: take a failure snapshot then finish.
    TakeSnapshot(
        "step_" + base::NumberToString(current_index_) + "_failed",
        base::BindOnce(
            [](base::WeakPtr<AutomationRunner> self, std::string note,
               bool /*ok*/) {
              if (!self) return;
              self->Finish(false, note);
            },
            weak_factory_.GetWeakPtr(), note));
    return;
  }
  // Reset retry counter on successful step.
  step_attempt_ = 0;
  ++current_index_;
  // Yield to message loop so the UI updates.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&AutomationRunner::ExecuteNextStep,
                     weak_factory_.GetWeakPtr()));
}

void AutomationRunner::Finish(bool success, const std::string& message) {
  is_running_ = false;
  int duration_ms = (base::TimeTicks::Now() - start_time_).InMilliseconds();
  RunResult result;
  result.success = success;
  result.message = message;
  result.steps_executed = static_cast<int>(current_index_);
  result.total_steps = static_cast<int>(script_.steps.size());
  result.duration_ms = duration_ms;

  // Persist stats.
  script_.stats.runs += 1;
  if (success) script_.stats.successes += 1;
  script_.stats.last_run_unix = static_cast<int64_t>(std::time(nullptr));
  script_.stats.last_run_duration_ms = duration_ms;
  script_.stats.last_result = message;
  // Day 6: track which step failed so the manager UI can offer a
  // "retry from step N" button. -1 = run succeeded / no failure.
  script_.stats.last_failed_step_index =
      success ? -1 : static_cast<int>(current_index_);
  // Day 6: cumulative + per-run AI token usage.
  script_.stats.ai_tokens_total += current_run_tokens_;
  RunRecord rec;
  rec.timestamp_unix = script_.stats.last_run_unix;
  rec.success = success;
  rec.duration_ms = duration_ms;
  rec.steps_executed = static_cast<int>(current_index_);
  rec.total_steps = static_cast<int>(script_.steps.size());
  rec.ai_tokens = current_run_tokens_;
  rec.message = message;
  // Newest first; cap to kRunHistoryCap so .molt files don't bloat.
  script_.stats.run_history.insert(script_.stats.run_history.begin(),
                                     std::move(rec));
  if (script_.stats.run_history.size() > kRunHistoryCap)
    script_.stats.run_history.resize(kRunHistoryCap);
  if (storage_) {
    storage_->Save(script_);
    storage_->AppendAudit(script_.id, success ? "run_succeeded" : "run_failed",
                          message);
  }

  if (on_complete_)
    std::move(on_complete_).Run(result);
}

void AutomationRunner::EmitStepProgress(bool starting, bool succeeded,
                                         const std::string& note) {
  if (!on_step_)
    return;
  if (current_index_ >= script_.steps.size())
    return;
  StepProgress p;
  p.index = static_cast<int>(current_index_);
  p.type = script_.steps[current_index_].type;
  p.description = Resolve(script_.steps[current_index_].description);
  p.starting = starting;
  p.succeeded = succeeded;
  p.note = note;
  on_step_.Run(p);
}

// -----------------------------------------------------------------------------
// Variable resolution
// -----------------------------------------------------------------------------

std::string AutomationRunner::Resolve(const std::string& raw) const {
  std::string out = raw;
  // Replace {{name}} with variable contents (string repr).
  for (const auto& kv : variables_) {
    std::string token = "{{" + kv.first + "}}";
    std::string val;
    if (kv.second.is_string()) {
      val = kv.second.GetString();
    } else if (kv.second.is_int()) {
      val = base::NumberToString(kv.second.GetInt());
    } else if (kv.second.is_double()) {
      val = base::NumberToString(kv.second.GetDouble());
    } else if (kv.second.is_list()) {
      // Render lists as JSON-y for the prompt context.
      base::JSONWriter::Write(kv.second, &val);
    } else if (kv.second.is_dict()) {
      base::JSONWriter::Write(kv.second, &val);
    }
    base::ReplaceSubstringsAfterOffset(&out, 0, token, val);
  }
  return out;
}

// -----------------------------------------------------------------------------
// JS evaluation helpers
// -----------------------------------------------------------------------------

void AutomationRunner::EvalJS(
    const std::string& script,
    base::OnceCallback<void(base::Value)> cb) {
  if (!target_contents_ || !target_contents_->GetPrimaryMainFrame()) {
    std::move(cb).Run(base::Value());
    return;
  }
  target_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(script),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb, base::Value v) {
            std::move(cb).Run(std::move(v));
          },
          std::move(cb)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void AutomationRunner::TryNextSelector(
    std::vector<std::string> candidates,
    size_t index,
    std::string description,
    base::OnceCallback<void(const std::string&)> cb) {
  if (index >= candidates.size()) {
    // All recorded selectors failed. Try LLM-based recovery if we have a
    // runtime and a description to ground the request in.
    if (ai_runtime_ && !description.empty() && target_contents_) {
      AskLLMForSelector(description, std::move(cb));
      return;
    }
    std::move(cb).Run("");
    return;
  }

  std::string sel = candidates[index];
  std::string js = base::StringPrintf(kHasSelectorJSTemplate,
                                       JSQuote(sel).c_str());
  auto self = weak_factory_.GetWeakPtr();
  EvalJS(js, base::BindOnce(
                  [](base::WeakPtr<AutomationRunner> self,
                     std::vector<std::string> cands, size_t idx,
                     std::string desc, std::string current,
                     base::OnceCallback<void(const std::string&)> cb,
                     base::Value v) {
                    if (!self) return;
                    if (v.is_bool() && v.GetBool()) {
                      std::move(cb).Run(current);
                      return;
                    }
                    self->TryNextSelector(std::move(cands), idx + 1,
                                          std::move(desc), std::move(cb));
                  },
                  self, std::move(candidates), index, std::move(description),
                  sel, std::move(cb)));
}

void AutomationRunner::ResolveSelector(
    const Step& s,
    base::OnceCallback<void(const std::string&)> cb) {
  // Build the candidate ladder: primary target first, then recorded
  // fallbacks. Each is variable-substituted via Resolve().
  std::vector<std::string> candidates;
  if (!s.target.empty())
    candidates.push_back(Resolve(s.target));
  for (const auto& f : s.selector_fallbacks)
    candidates.push_back(Resolve(f));

  if (candidates.empty()) {
    std::move(cb).Run("");
    return;
  }

  TryNextSelector(std::move(candidates), 0, Resolve(s.description),
                  std::move(cb));
}

namespace {

// Snippet of page innerText to give the LLM enough context to pick a new
// selector. Capped to keep prompts under TinyLlama's context window.
constexpr char kPageSnippetJS[] =
    "(function(){"
    "  const t = document.body ? document.body.innerText : '';"
    "  return t.slice(0, 2000);"
    "})()";

}  // namespace

void AutomationRunner::AskLLMForSelector(
    const std::string& description,
    base::OnceCallback<void(const std::string&)> cb) {
  // First grab a small text snippet from the page, then ask the LLM.
  auto self = weak_factory_.GetWeakPtr();
  EvalJS(kPageSnippetJS,
         base::BindOnce(
             [](base::WeakPtr<AutomationRunner> self,
                std::string desc,
                base::OnceCallback<void(const std::string&)> cb,
                base::Value v) {
               if (!self || !self->ai_runtime_) {
                 std::move(cb).Run("");
                 return;
               }
               std::string snippet = v.is_string() ? v.GetString() : "";
               std::string prompt =
                   "Page text snippet (truncated):\n```\n" + snippet +
                   "\n```\n\n"
                   "The user wants to interact with: " + desc + "\n"
                   "Return a single CSS selector that targets that element. "
                   "Reply with ONLY the selector, no quotes, no commentary.";
               molt_ai::PromptOptions opts;
               opts.max_tokens = 64;
               molt_ai::GenerationResult r =
                   self->ai_runtime_->RunPrompt(prompt, opts);
               self->current_run_tokens_ +=
                   r.tokens_prompt + r.tokens_generated;
               std::string s = r.success ? r.text : std::string();
               // Trim whitespace + leading/trailing punctuation the model
               // loves to add.
               while (!s.empty() &&
                      (s.front() == ' ' || s.front() == '\n' ||
                       s.front() == '`' || s.front() == '"'))
                 s.erase(0, 1);
               while (!s.empty() &&
                      (s.back() == ' ' || s.back() == '\n' ||
                       s.back() == '`' || s.back() == '"'))
                 s.pop_back();
               // Reject obviously-wrong replies (multi-line / very long).
               if (s.find('\n') != std::string::npos || s.size() > 200)
                 s.clear();
               std::move(cb).Run(s);
             },
             self, description, std::move(cb)));
}

// -----------------------------------------------------------------------------
// Action implementations
// -----------------------------------------------------------------------------

void AutomationRunner::DoNavigate(const Step& s) {
  if (!target_contents_) {
    OnStepFinished(false, "No target tab");
    return;
  }
  GURL url(Resolve(s.target));
  if (!url.is_valid()) {
    OnStepFinished(false, "Invalid URL");
    return;
  }
  target_contents_->GetController().LoadURL(
      url, content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
  OnStepFinished(true, "Navigated to " + url.spec());
}

void AutomationRunner::DoClick(const Step& s) {
  ResolveSelector(s, base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self,
         base::OnceCallback<void()> retry,
         const std::string& sel) {
        if (!self) return;
        if (sel.empty()) {
          self->OnStepFinished(false, "Selector did not match");
          return;
        }
        std::string js = base::StringPrintf(
            kClickSelectorJSTemplate, JSQuote(sel).c_str());
        self->EvalJS(js, base::BindOnce(
            [](base::WeakPtr<AutomationRunner> self, base::Value v) {
              if (!self) return;
              if (v.is_bool() && v.GetBool())
                self->OnStepFinished(true, "Clicked");
              else
                self->OnStepFinished(false, "Click failed");
            },
            self));
      },
      weak_factory_.GetWeakPtr(),
      base::OnceCallback<void()>()));
}

void AutomationRunner::DoType(const Step& s) {
  ResolveSelector(s, base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, std::string value,
         const std::string& sel) {
        if (!self) return;
        if (sel.empty()) {
          self->OnStepFinished(false, "Input not found");
          return;
        }
        std::string val_js = JSQuote(value);
        std::string js = base::StringPrintf(
            kTypeIntoJSTemplate, JSQuote(sel).c_str(),
            val_js.c_str(), val_js.c_str());
        self->EvalJS(js, base::BindOnce(
            [](base::WeakPtr<AutomationRunner> self, base::Value v) {
              if (!self) return;
              self->OnStepFinished(v.is_bool() && v.GetBool(),
                                    "Typed text");
            },
            self));
      },
      weak_factory_.GetWeakPtr(),
      Resolve(s.value)));
}

void AutomationRunner::DoScroll(const Step& s) {
  int px = 600;
  if (!s.value.empty()) {
    int parsed = 0;
    if (base::StringToInt(s.value, &parsed))
      px = parsed;
  }
  std::string js = base::StringPrintf(kScrollByJSTemplate, px);
  EvalJS(js, base::BindOnce(
                  [](base::WeakPtr<AutomationRunner> self, base::Value) {
                    if (!self) return;
                    self->OnStepFinished(true, "Scrolled");
                  },
                  weak_factory_.GetWeakPtr()));
}

void AutomationRunner::DoWait(const Step& s) {
  int ms = s.timeout_ms > 0 ? s.timeout_ms : 1000;
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AutomationRunner::OnStepFinished,
                     weak_factory_.GetWeakPtr(),
                     true, "Waited"),
      base::Milliseconds(ms));
}

void AutomationRunner::DoWaitFor(const Step& s) {
  // Poll every 250ms up to s.timeout_ms.
  std::string sel = Resolve(s.target);
  if (sel.empty()) {
    OnStepFinished(false, "Empty selector");
    return;
  }
  // Capture deadline by reference via copy into the lambda.
  auto deadline = base::TimeTicks::Now() + base::Milliseconds(s.timeout_ms);
  auto poll = std::make_shared<base::OnceClosure>();
  *poll = base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, std::string sel,
         base::TimeTicks deadline,
         std::shared_ptr<base::OnceClosure> poll_holder) {
        if (!self) return;
        std::string js = base::StringPrintf(
            kHasSelectorJSTemplate, JSQuote(sel).c_str());
        self->EvalJS(js, base::BindOnce(
            [](base::WeakPtr<AutomationRunner> self, std::string sel,
               base::TimeTicks deadline,
               std::shared_ptr<base::OnceClosure> poll_holder,
               base::Value v) {
              if (!self) return;
              bool matched = v.is_bool() && v.GetBool();
              if (matched) {
                self->OnStepFinished(true, "Selector appeared");
                return;
              }
              if (base::TimeTicks::Now() >= deadline) {
                self->OnStepFinished(false, "Timed out waiting for selector");
                return;
              }
              // Re-arm.
              base::SequencedTaskRunner::GetCurrentDefault()
                  ->PostDelayedTask(
                      FROM_HERE,
                      base::BindOnce(
                          [](std::shared_ptr<base::OnceClosure> p) {
                            if (p && *p) std::move(*p).Run();
                          },
                          poll_holder),
                      base::Milliseconds(250));
            },
            self, sel, deadline, poll_holder));
      },
      weak_factory_.GetWeakPtr(), sel, deadline, poll);
  std::move(*poll).Run();
}

void AutomationRunner::DoExtract(const Step& s) {
  ResolveSelector(s, base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, std::string store_as,
         const std::string& sel) {
        if (!self) return;
        if (sel.empty()) {
          self->OnStepFinished(false, "Selector did not match");
          return;
        }
        std::string js = base::StringPrintf(
            kExtractJSTemplate, JSQuote(sel).c_str());
        self->EvalJS(js, base::BindOnce(
            [](base::WeakPtr<AutomationRunner> self, std::string store_as,
               base::Value v) {
              if (!self) return;
              if (!store_as.empty())
                self->variables_[store_as] = std::move(v);
              self->OnStepFinished(true, "Extracted into " + store_as);
            },
            self, store_as));
      },
      weak_factory_.GetWeakPtr(), s.store_as));
}

void AutomationRunner::DoAIDecide(const Step& s) {
  // Local LLM decision step. The prompt is resolved with current
  // variables, sent to BrowserAIRuntime, and the response is parsed
  // for SUCCESS / FAIL / true / false / yes / no.
  std::string prompt = Resolve(s.value);
  if (!ai_runtime_ || prompt.empty()) {
    LOG(WARNING) << "[Automation] ai_decide without runtime/prompt";
    OnStepFinished(true, "AI: skipped (no runtime)");
    return;
  }

  // Force the model to give a one-word answer so parsing is reliable.
  std::string framed =
      prompt + "\n\nAnswer with exactly one word: SUCCESS or FAIL.";

  PromptOptions opts;
  opts.model_id = script_.ai_model;
  opts.max_tokens = 8;
  opts.temperature = 0.1f;
  opts.stream = false;
  GenerationResult r = ai_runtime_->RunPrompt(framed, opts);
  current_run_tokens_ += r.tokens_prompt + r.tokens_generated;

  std::string ans = base::ToLowerASCII(r.text);
  bool succeeded = ans.find("success") != std::string::npos ||
                    ans.find("yes") != std::string::npos ||
                    ans.find("true") != std::string::npos;
  // The result is stored so subsequent steps can branch on the outcome.
  variables_["__ai_decision"] = base::Value(succeeded);
  OnStepFinished(true, std::string("AI: ") +
                            (succeeded ? "SUCCESS" : "FAIL") +
                            " (" + r.text.substr(0, 32) + ")");
}

void AutomationRunner::DoAIExtract(const Step& s) {
  // The extra dict is expected to contain a "schema" describing the
  // shape we want, e.g. {"price": "int", "airline": "string"}. We pass
  // the schema and the visible page text to the LLM and parse the
  // returned JSON.
  if (!ai_runtime_) {
    if (!s.store_as.empty())
      variables_[s.store_as] = base::Value(base::DictValue());
    OnStepFinished(true, "AI extract: no runtime");
    return;
  }

  // Pull the visible body text first, then prompt the model.
  EvalJS("(document.body && document.body.innerText) || ''",
         base::BindOnce(
            [](base::WeakPtr<AutomationRunner> self,
               std::string store_as, std::string user_prompt,
               base::Value page_text) {
              if (!self) return;
              std::string text = page_text.is_string()
                  ? page_text.GetString().substr(0, 6000)
                  : std::string();
              std::string framed =
                  "You are extracting structured data from a web page.\n"
                  "Page text follows between <PAGE> markers. Respond with "
                  "ONLY a single JSON object, no commentary.\n\n"
                  "Task: " + user_prompt + "\n\n"
                  "<PAGE>\n" + text + "\n</PAGE>\n\nJSON:";
              PromptOptions opts;
              opts.model_id = self->script_.ai_model;
              opts.max_tokens = 256;
              opts.temperature = 0.1f;
              opts.stream = false;
              GenerationResult r = self->ai_runtime_->RunPrompt(framed, opts);
              self->current_run_tokens_ +=
                  r.tokens_prompt + r.tokens_generated;
              // Try to parse JSON; if it fails, store as raw string.
              auto parsed = base::JSONReader::Read(
                  r.text, base::JSON_ALLOW_TRAILING_COMMAS);
              if (!store_as.empty()) {
                if (parsed)
                  self->variables_[store_as] = std::move(*parsed);
                else
                  self->variables_[store_as] = base::Value(r.text);
              }
              self->OnStepFinished(true, "AI extract -> " + store_as);
            },
            weak_factory_.GetWeakPtr(),
            s.store_as,
            Resolve(s.value)));
}

void AutomationRunner::DoNotify(const Step& s) {
  // Per user spec: top-right macOS-style notification. The actual
  // platform notification is fired by the manager UI when it observes
  // the StepProgress callback — for the runner we just store the message
  // as the run's last_result so the manager picks it up.
  std::string body = Resolve(s.value);
  if (!body.empty())
    script_.stats.last_result = body;
  OnStepFinished(true, body.empty() ? "Notify" : body);
}

void AutomationRunner::DoScreenshot(const Step& s) {
  // No bitmap yet — write a JSON snapshot {url, html_snippet, text_snippet}
  // into ~/.moltbrowser/automations/artifacts/<id>/<tag>_<unix>.json.
  std::string tag = Resolve(s.value);
  if (tag.empty()) tag = "screenshot";
  TakeSnapshot(tag, base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, bool ok) {
        if (!self) return;
        self->OnStepFinished(ok, ok ? "Snapshot saved" : "Snapshot failed");
      },
      weak_factory_.GetWeakPtr()));
}

// IF / ELSE / END_IF / LOOP / END_LOOP have moved to ExecuteNextStep so
// they can run even when we're in a not-taken branch (for stack
// management).  These methods are kept as stubs to satisfy the header.
void AutomationRunner::DoIf(const Step& s) {
  OnStepFinished(true, "If");
}
void AutomationRunner::DoElse(const Step& s) {
  OnStepFinished(true, "Else");
}
void AutomationRunner::DoEndIf(const Step& s) {
  OnStepFinished(true, "End if");
}
void AutomationRunner::DoLoop(const Step& s) {
  OnStepFinished(true, "Loop");
}
void AutomationRunner::DoEndLoop(const Step& s) {
  OnStepFinished(true, "End loop");
}

void AutomationRunner::DoKeypress(const Step& s) {
  // We dispatch a synthetic KeyboardEvent on the focused element (or
  // body). Note: synthetic events have isTrusted=false, so heavily
  // sandboxed SPAs may ignore them — for those, prefer CLICK on a real
  // submit button or use ENTER via a form.requestSubmit() call in JS.
  std::string key = Resolve(s.value);
  if (key.empty()) {
    OnStepFinished(false, "Empty keypress value");
    return;
  }
  std::string js = base::StringPrintf(R"JS(
    (function() {
      var t = document.activeElement || document.body;
      if (!t) return false;
      var k = %s;
      // Map "Enter" to also call form.requestSubmit() if the target is
      // inside a form, since that's what users usually mean.
      ['keydown','keypress','keyup'].forEach(function(type) {
        try {
          t.dispatchEvent(new KeyboardEvent(type, {
            key: k, code: k, bubbles: true, cancelable: true
          }));
        } catch(e) {}
      });
      if (k === 'Enter' && t.form) {
        try { t.form.requestSubmit ? t.form.requestSubmit() : t.form.submit(); }
        catch(e) {}
      }
      return true;
    })()
  )JS", JSQuote(key).c_str());
  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, std::string key,
         base::Value v) {
        if (!self) return;
        bool ok = v.is_bool() && v.GetBool();
        self->OnStepFinished(ok, ok ? "Pressed " + key : "Keypress failed");
      },
      weak_factory_.GetWeakPtr(), key));
}

void AutomationRunner::DoRunJS(const Step& s) {
  // Trust gate: RUN_JS is power-user-only. Below TRUSTED, fail hard.
  if (script_.security.trust < TrustLevel::TRUSTED) {
    OnStepFinished(false, "RUN_JS requires Trusted script");
    return;
  }
  std::string js = Resolve(s.value);
  if (js.empty()) {
    OnStepFinished(false, "Empty JS");
    return;
  }
  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, std::string store_as,
         base::Value v) {
        if (!self) return;
        if (!store_as.empty())
          self->variables_[store_as] = std::move(v);
        self->OnStepFinished(true, store_as.empty() ? "Ran JS"
                                                     : "Ran JS -> " + store_as);
      },
      weak_factory_.GetWeakPtr(), s.store_as));
}

// RUN_SCRIPT: chain agents. The step's `target` field is the id of a
// saved Script to invoke. We load the child script from storage and
// splice its steps into the parent's script_.steps vector at the
// position immediately after the current step, so when ExecuteNextStep
// advances it picks up the child's first step. The child shares the
// parent's WebContents, ai_runtime, storage, and variables_ map, so
// {{vars}} flow through. The parent's stats/security policy continue
// to apply.
//
// Cycle protection: if the child id matches script_.id we refuse,
// since the user could chain a script into itself ad infinitum.
void AutomationRunner::DoRunScript(const Step& s) {
  std::string child_id = Resolve(s.target);
  if (child_id.empty()) {
    OnStepFinished(false, "RUN_SCRIPT: missing target script id");
    return;
  }
  if (child_id == script_.id) {
    OnStepFinished(false, "RUN_SCRIPT: refusing to recurse into self");
    return;
  }
  if (!storage_) {
    OnStepFinished(false, "RUN_SCRIPT: no storage available");
    return;
  }
  auto child = storage_->Load(child_id);
  if (!child) {
    OnStepFinished(false, "RUN_SCRIPT: child not found: " + child_id);
    return;
  }
  if (child->steps.empty()) {
    OnStepFinished(true, "RUN_SCRIPT: " + child_id + " (empty, skipped)");
    return;
  }
  // Splice the child's steps after the current index. ExecuteNextStep
  // will advance current_index_ by 1 and pick up the first child step.
  // Step contains a base::Value (move-only), so we move-iterate.
  const size_t inserted = child->steps.size();
  auto insert_pos = script_.steps.begin() + current_index_ + 1;
  script_.steps.insert(insert_pos,
                       std::make_move_iterator(child->steps.begin()),
                       std::make_move_iterator(child->steps.end()));
  OnStepFinished(true,
                 "RUN_SCRIPT: expanded " + child_id +
                 " (" + std::to_string(inserted) + " steps)");
}

void AutomationRunner::DoAssert(const Step& s) {
  // Two modes:
  //   target only   -> selector must exist
  //   target+value  -> matched element's innerText/value must contain value
  std::string sel = Resolve(s.target);
  std::string expected = Resolve(s.value);
  if (sel.empty() && expected.empty()) {
    OnStepFinished(false, "Assert needs target or value");
    return;
  }
  if (!sel.empty() && expected.empty()) {
    std::string js = base::StringPrintf(kHasSelectorJSTemplate,
                                         JSQuote(sel).c_str());
    EvalJS(js, base::BindOnce(
        [](base::WeakPtr<AutomationRunner> self, std::string sel,
           base::Value v) {
          if (!self) return;
          bool ok = v.is_bool() && v.GetBool();
          self->OnStepFinished(ok,
              ok ? "Assert: present" : "Assert: missing " + sel);
        },
        weak_factory_.GetWeakPtr(), sel));
    return;
  }
  std::string js = base::StringPrintf(R"JS(
    (function() {
      var el = document.querySelector(%s);
      if (!el) return null;
      return (el.innerText || el.value || el.textContent || '');
    })()
  )JS", JSQuote(sel).c_str());
  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, std::string sel,
         std::string expected, base::Value v) {
        if (!self) return;
        if (!v.is_string()) {
          self->OnStepFinished(false, "Assert: " + sel + " missing");
          return;
        }
        std::string text = v.GetString();
        bool ok = text.find(expected) != std::string::npos;
        self->OnStepFinished(ok,
            ok ? "Assert: matched"
               : "Assert: '" + expected + "' not in '" +
                 text.substr(0, 80) + "'");
      },
      weak_factory_.GetWeakPtr(), sel, expected));
}

void AutomationRunner::TakeSnapshot(const std::string& tag,
                                     base::OnceCallback<void(bool)> on_done) {
  if (!storage_ || !target_contents_) {
    std::move(on_done).Run(false);
    return;
  }
  // Capture URL + visible text + a small outerHTML snippet via JS.
  EvalJS(R"JS(
    (function() {
      var body = document.body;
      var html = body ? body.outerHTML.slice(0, 8000) : '';
      var text = body ? body.innerText.slice(0, 4000) : '';
      return {url: location.href, title: document.title,
              text: text, html: html, ts: Date.now()};
    })()
  )JS", base::BindOnce(
      [](base::WeakPtr<AutomationRunner> self, std::string tag,
         base::OnceCallback<void(bool)> on_done, base::Value v) {
        if (!self || !self->storage_) {
          std::move(on_done).Run(false);
          return;
        }
        base::FilePath dir =
            self->storage_->EnsureArtifactsDir(self->script_.id);
        if (dir.empty()) {
          std::move(on_done).Run(false);
          return;
        }
        std::string filename = tag + "_" +
            base::NumberToString(static_cast<int64_t>(std::time(nullptr))) +
            ".json";
        base::FilePath path = dir.Append(filename);
        std::string serialized;
        base::JSONWriter::WriteWithOptions(
            v, base::JSONWriter::OPTIONS_PRETTY_PRINT, &serialized);
        // EnsureArtifactsDir already opened a ScopedAllowBlocking scope
        // for itself, but WriteFile here is independent — open a local
        // one. Since AutomationStorage is the gate, we go through a
        // tiny helper that uses the same wrapper.
        bool ok = false;
        {
          ScopedAllowBlockingForMolt allow_blocking;
          ok = base::WriteFile(path, serialized);
        }
        if (ok) {
          self->storage_->AppendAudit(self->script_.id, "snapshot",
                                       filename);
        } else {
          LOG(WARNING) << "[Automation] snapshot write failed: "
                       << path.value();
        }
        std::move(on_done).Run(ok);
      },
      weak_factory_.GetWeakPtr(), tag, std::move(on_done)));
}

}  // namespace automation
}  // namespace molt_ai
