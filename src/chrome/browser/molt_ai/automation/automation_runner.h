// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationRunner — orchestrates execution of a single Script.
//
// One Runner = one in-flight script. The runner walks the steps list,
// substitutes {{variables}}, dispatches each action to the right
// executor (DOM action via WebContents IPC, AI step via BrowserAIRuntime),
// updates Stats, and writes audit lines.
//
// Threading: methods must be called on the UI thread; any blocking work
// (LLM inference, screenshot serialization) is dispatched off-thread and
// completion bounces back.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RUNNER_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RUNNER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {

class BrowserAIRuntime;

namespace automation {

// Result reported when a run finishes (success or failure).
struct RunResult {
  bool success = false;
  std::string message;            // human-readable result/error
  int steps_executed = 0;
  int total_steps = 0;
  int duration_ms = 0;
};

// Per-step progress callback — fired before and after each step so the UI
// can highlight the active step and animate spinners.
struct StepProgress {
  int index = 0;             // 0-based step number
  StepType type = StepType::UNKNOWN;
  std::string description;   // resolved with {{vars}} substituted
  bool starting = true;      // false = step finished
  bool succeeded = false;    // only meaningful when starting=false
  std::string note;          // e.g. "selector matched", "AI returned SUCCESS"
};

using RunCompleteCallback = base::OnceCallback<void(RunResult)>;
using StepProgressCallback = base::RepeatingCallback<void(const StepProgress&)>;

class AutomationRunner {
 public:
  // |target_contents| is the tab the script operates on. The runner does
  // not own it; caller must keep it alive. Per user spec, the runner uses
  // the user's main profile so logins/cookies persist.
  AutomationRunner(content::WebContents* target_contents,
                   BrowserAIRuntime* ai_runtime,
                   AutomationStorage* storage);
  ~AutomationRunner();

  AutomationRunner(const AutomationRunner&) = delete;
  AutomationRunner& operator=(const AutomationRunner&) = delete;

  // Begin executing |script|. |on_complete| fires once at the end.
  // |on_step| fires for every step transition (start + end). May be null.
  // |start_index| picks up at a specific step (for the manager UI's
  // "retry from failed step" button); 0 = run from the beginning.
  void Run(Script script,
           StepProgressCallback on_step,
           RunCompleteCallback on_complete,
           size_t start_index = 0);

  // Cooperative cancel. Stops at the next step boundary.
  void Cancel();

  bool is_running() const { return is_running_; }

 private:
  // Step dispatch — each returns true to continue, false to abort the run.
  void ExecuteNextStep();
  void OnStepFinished(bool succeeded, const std::string& note);

  // Action executors. Each posts work and calls OnStepFinished when done.
  void DoNavigate(const Step& s);
  void DoClick(const Step& s);
  void DoType(const Step& s);
  void DoScroll(const Step& s);
  void DoKeypress(const Step& s);
  void DoWait(const Step& s);
  void DoWaitFor(const Step& s);
  void DoExtract(const Step& s);
  void DoAIDecide(const Step& s);
  void DoAIExtract(const Step& s);
  void DoNotify(const Step& s);
  void DoScreenshot(const Step& s);
  void DoIf(const Step& s);
  void DoElse(const Step& s);
  void DoEndIf(const Step& s);
  void DoLoop(const Step& s);
  void DoEndLoop(const Step& s);
  void DoAssert(const Step& s);
  void DoRunJS(const Step& s);
  // RUN_SCRIPT: inline-expand a child script's steps into the current
  // script_.steps vector at current_index_ + 1, then continue.
  void DoRunScript(const Step& s);

  // Returns true iff every IF frame currently allows execution. Used by
  // ExecuteNextStep to skip steps inside a not-taken branch.
  bool ShouldExecuteCurrentStep() const;
  // Evaluate s.value as a truthy expression for IF. Looks up the variable
  // by name first, falls back to literal-string truthiness.
  bool EvaluateIfCondition(const Step& s) const;

  // Save a JSON snapshot {url, html_snippet, text_snippet, timestamp} to
  // ~/.moltbrowser/automations/artifacts/<script_id>/<tag>_<unix>.json.
  // |on_done| fires on the UI thread after the file is written.
  void TakeSnapshot(const std::string& tag,
                    base::OnceCallback<void(bool)> on_done);

  // Per-step timeout: if the dispatched step doesn't call OnStepFinished
  // within |s.timeout_ms|, this fires false. Cleared on every legit
  // OnStepFinished.
  void ArmStepTimeout(const Step& s);
  void OnStepTimeout();

  // Resolve {{variable}} placeholders in |raw| using current variables_.
  std::string Resolve(const std::string& raw) const;

  // Run JS in the active frame and pass the result to |cb|. The expression
  // must produce a JSON-serialisable value.
  void EvalJS(const std::string& script,
              base::OnceCallback<void(base::Value)> cb);

  // Runs the recorded selector + each fallback until one matches; returns
  // the matching selector, or empty string if none found. If all recorded
  // candidates fail, falls back to AskLLMForSelector() which queries the
  // BrowserAIRuntime for a recovery selector.
  void ResolveSelector(
      const Step& s,
      base::OnceCallback<void(const std::string&)> cb);

  // Internal helper: walks the candidate ladder one at a time, then
  // delegates to AskLLMForSelector if all fail.
  void TryNextSelector(
      std::vector<std::string> candidates,
      size_t index,
      std::string description,
      std::string text_hint,
      base::OnceCallback<void(const std::string&)> cb);

  // Text-anchor recovery: find the visible clickable element whose text matches
  // the recorded hint (e.g. "Kids"), mark it, and return a unique selector.
  // Falls through to AskLLMForSelector if no text match.
  void ResolveByText(const std::string& text_hint,
                     const std::string& description,
                     base::OnceCallback<void(const std::string&)> cb);

  // Last-resort recovery: send page snippet + step description to the LLM
  // and ask for a CSS selector. Returns "" if the model fails.
  void AskLLMForSelector(
      const std::string& description,
      base::OnceCallback<void(const std::string&)> cb);

  void EmitStepProgress(bool starting, bool succeeded,
                        const std::string& note);
  void Finish(bool success, const std::string& message);

  // ---- State ----
  raw_ptr<content::WebContents> target_contents_;  // not owned
  raw_ptr<BrowserAIRuntime> ai_runtime_;           // not owned
  raw_ptr<AutomationStorage> storage_;             // not owned

  Script script_;
  size_t current_index_ = 0;
  bool is_running_ = false;
  bool cancel_requested_ = false;
  base::TimeTicks start_time_;

  // Variables filled by EXTRACT / AI_EXTRACT and substituted via {{name}}.
  std::map<std::string, base::Value> variables_;

  // Stack of (loop_start_index, iterations_left) for nested LOOP/END_LOOP.
  struct LoopFrame {
    size_t start_index;
    int remaining;
  };
  std::vector<LoopFrame> loop_stack_;

  // Per-IF state: condition value at evaluation time + whether we've
  // crossed the ELSE marker yet. A step executes iff for every frame
  // (condition && !in_else) || (!condition && in_else).
  struct IfFrame {
    bool condition = false;
    bool in_else = false;
  };
  std::vector<IfFrame> if_stack_;

  // Per-step retry counter. Reset on step transition; incremented when
  // a step fails and Step::retries > attempts_so_far_.
  int step_attempt_ = 0;

  // Day 6: AI tokens consumed by the current run. Accumulated from
  // GenerationResult.tokens_prompt + tokens_generated on every
  // AI_DECIDE / AI_EXTRACT call. Folded into Stats::ai_tokens_total
  // and the new RunRecord at Finish().
  int current_run_tokens_ = 0;

  // Per-step timeout — fires OnStepTimeout() if the dispatched step
  // doesn't call OnStepFinished in time.
  base::OneShotTimer step_timeout_timer_;

  StepProgressCallback on_step_;
  RunCompleteCallback on_complete_;

  base::WeakPtrFactory<AutomationRunner> weak_factory_{this};
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RUNNER_H_
