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
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
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
  void Run(Script script,
           StepProgressCallback on_step,
           RunCompleteCallback on_complete);

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

  // Resolve {{variable}} placeholders in |raw| using current variables_.
  std::string Resolve(const std::string& raw) const;

  // Run JS in the active frame and pass the result to |cb|. The expression
  // must produce a JSON-serialisable value.
  void EvalJS(const std::string& script,
              base::OnceCallback<void(base::Value)> cb);

  // Runs the recorded selector + each fallback until one matches; returns
  // the matching selector, or empty string if none found.
  void ResolveSelector(
      const Step& s,
      base::OnceCallback<void(const std::string&)> cb);

  void EmitStepProgress(bool starting, bool succeeded,
                        const std::string& note);
  void Finish(bool success, const std::string& message);

  // ---- State ----
  content::WebContents* target_contents_;       // not owned
  BrowserAIRuntime* ai_runtime_;                // not owned
  AutomationStorage* storage_;                  // not owned

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

  // Stack of bool: did the last IF condition succeed? Used to skip ELSE
  // branches.
  std::vector<bool> if_stack_;

  StepProgressCallback on_step_;
  RunCompleteCallback on_complete_;

  base::WeakPtrFactory<AutomationRunner> weak_factory_{this};
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RUNNER_H_
