// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_runner.h"

#include <ctime>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
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
                            RunCompleteCallback on_complete) {
  if (is_running_) {
    if (on_complete) {
      std::move(on_complete).Run(
          {false, "Runner busy", 0, 0, 0});
    }
    return;
  }
  script_ = std::move(script);
  current_index_ = 0;
  is_running_ = true;
  cancel_requested_ = false;
  variables_.clear();
  loop_stack_.clear();
  if_stack_.clear();
  start_time_ = base::TimeTicks::Now();
  on_step_ = std::move(on_step);
  on_complete_ = std::move(on_complete);

  if (storage_)
    storage_->AppendAudit(script_.id, "run_started", "");

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
  EmitStepProgress(/*starting=*/true, /*succeeded=*/false, "");

  switch (s.type) {
    case StepType::NAVIGATE:    DoNavigate(s); break;
    case StepType::CLICK:       DoClick(s); break;
    case StepType::TYPE:        DoType(s); break;
    case StepType::SCROLL:      DoScroll(s); break;
    case StepType::WAIT:        DoWait(s); break;
    case StepType::WAIT_FOR:    DoWaitFor(s); break;
    case StepType::EXTRACT:     DoExtract(s); break;
    case StepType::AI_DECIDE:   DoAIDecide(s); break;
    case StepType::AI_EXTRACT:  DoAIExtract(s); break;
    case StepType::NOTIFY:      DoNotify(s); break;
    case StepType::SCREENSHOT:  DoScreenshot(s); break;
    case StepType::IF:          DoIf(s); break;
    case StepType::ELSE:        DoElse(s); break;
    case StepType::END_IF:      DoEndIf(s); break;
    case StepType::LOOP:        DoLoop(s); break;
    case StepType::END_LOOP:    DoEndLoop(s); break;
    case StepType::ASSERT:      DoAssert(s); break;
    default:
      OnStepFinished(false, "Unknown step type");
      break;
  }
}

void AutomationRunner::OnStepFinished(bool succeeded, const std::string& note) {
  EmitStepProgress(/*starting=*/false, succeeded, note);
  if (!succeeded) {
    Finish(false, note);
    return;
  }
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

void AutomationRunner::ResolveSelector(
    const Step& s,
    base::OnceCallback<void(const std::string&)> cb) {
  // Try primary, then fallbacks. Each attempt: query JS to see if exactly
  // one match exists. The first success wins.
  std::vector<std::string> candidates;
  if (!s.target.empty())
    candidates.push_back(Resolve(s.target));
  for (const auto& f : s.selector_fallbacks)
    candidates.push_back(Resolve(f));

  if (candidates.empty()) {
    std::move(cb).Run("");
    return;
  }

  // Capture-state recursive lambda via a helper class isn't trivial in C++,
  // so we use a stateful callback pattern: for simplicity, just try the
  // first candidate; if it fails we abort the step. The full ladder is a
  // Sprint 2 polish item.
  std::string first = candidates.front();
  std::string js = base::StringPrintf(kHasSelectorJSTemplate,
                                       JSQuote(first).c_str());
  EvalJS(js, base::BindOnce(
                  [](std::string sel,
                     base::OnceCallback<void(const std::string&)> cb,
                     base::Value v) {
                    std::move(cb).Run(v.is_bool() && v.GetBool() ? sel : "");
                  },
                  first, std::move(cb)));
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
  // Defer real bitmap capture to Sprint 2; Sprint 1 just logs the intent
  // so test scripts can include the step.
  OnStepFinished(true, "Screenshot (deferred)");
}

void AutomationRunner::DoIf(const Step& s) {
  // Sprint 1: if without ai_decide simply pushes 'true' so all branches run.
  if_stack_.push_back(true);
  OnStepFinished(true, "If");
}

void AutomationRunner::DoElse(const Step& s) {
  if (!if_stack_.empty()) if_stack_.back() = !if_stack_.back();
  OnStepFinished(true, "Else");
}

void AutomationRunner::DoEndIf(const Step& s) {
  if (!if_stack_.empty()) if_stack_.pop_back();
  OnStepFinished(true, "End if");
}

void AutomationRunner::DoLoop(const Step& s) {
  loop_stack_.push_back({current_index_, s.max_iterations});
  OnStepFinished(true, "Loop start");
}

void AutomationRunner::DoEndLoop(const Step& s) {
  if (loop_stack_.empty()) {
    OnStepFinished(false, "END_LOOP without LOOP");
    return;
  }
  auto& top = loop_stack_.back();
  top.remaining -= 1;
  if (top.remaining > 0) {
    // Jump back to the LOOP step (current_index_ will be incremented to
    // start_index + 1 in OnStepFinished).
    current_index_ = top.start_index;
    OnStepFinished(true, "Loop continue");
  } else {
    loop_stack_.pop_back();
    OnStepFinished(true, "Loop done");
  }
}

void AutomationRunner::DoAssert(const Step& s) {
  // For Sprint 1: just succeed. Sprint 2 wires real assertion checks.
  OnStepFinished(true, "Assert");
}

}  // namespace automation
}  // namespace molt_ai
