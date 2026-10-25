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
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/automation/automation_ai_steps.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {

class BrowserAIRuntime;
struct GenerationResult;
struct PromptOptions;

namespace automation {

// Waits for a click-triggered navigation to settle before advancing the run
// (defined in the .cc). Forward-declared so it can be held as a member.
class NavigationSettleWaiter;

// A RunPromptAsync request waiting for the in-flight one (defined in .cc).
struct QueuedPrompt;

// Step::extra key. When true, a CLICK that would follow a link to another
// origin, or a TYPE into a password / payment-card / one-time-code field,
// is not performed; the run stops with a message starting with
// kNeedsConfirmationPrefix instead. The chat action path sets it until the
// user has confirmed the action.
inline constexpr char kAskBeforeSensitiveKey[] = "ask_before_sensitive";
inline constexpr char kNeedsConfirmationPrefix[] = "needs_confirmation: ";

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

  // Workflow security policy (automation_security.h). Checks |s| against
  // policy_ before it touches the page; false + |why| if it must not run.
  // |needs_trust| is set when trusting the workflow (APPROVED) would allow
  // the step.
  bool PreflightStepPolicy(const Step& s,
                           std::string* why,
                           bool* needs_trust) const;
  // Ends the run on the current step without retries (a policy block or a
  // step that needs the user's confirmation), recording |reason|.
  // |needs_trust| marks a block that trusting the workflow lifts; it is
  // saved as Stats::last_run_needs_trust.
  void BlockStep(const std::string& reason, bool needs_trust = false);

  // After a step reaches a terminal outcome (success, or failure with no
  // retries left), record a StepResult for the Runs timeline — including a
  // screenshot thumbnail captured on the current page — then either advance
  // to the next step or Finish() the run.
  void CaptureStepAndThen(bool succeeded, std::string note,
                          bool terminal_failure);
  void FinalizeStep(StepResult sr, std::string filename, std::string note,
                    bool terminal_failure, bool screenshot_ok);

  // Capture the tab's current pixels to |path| as a downscaled PNG. |done|
  // runs on the UI thread with true iff a file was written. Best-effort:
  // a missing live surface (minimized without a capturer, occluded, not yet
  // composited) resolves false without failing the step.
  void CapturePng(const base::FilePath& path,
                  base::OnceCallback<void(bool)> done);

  // A click may trigger a navigation. Wait briefly for the resulting load to
  // settle (like DoNavigate) before advancing, so the next step doesn't run
  // against a torn-down / mid-navigation page. If no navigation starts within
  // a short grace window, finishes immediately.
  void BeginClickSettle(const std::string& note);

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

  // Second halves of DoClick / DoType: once the selector is resolved, probe
  // what the element is (submit control, link target, field kind) and
  // apply the policy before acting.
  void ProbeElement(const std::string& sel,
                    base::OnceCallback<void(agent_safety::ElementProbe)> cb);
  void OnClickTargetResolved(bool ask_before_sensitive, const std::string& sel);
  void OnClickProbed(const std::string& sel,
                     bool ask_before_sensitive,
                     agent_safety::ElementProbe probe);
  void ClickNow(const std::string& sel);
  void OnTypeTargetResolved(std::string value,
                            bool ask_before_sensitive,
                            const std::string& sel);
  void OnTypeProbed(const std::string& sel,
                    std::string value,
                    bool ask_before_sensitive,
                    agent_safety::ElementProbe probe);
  void TypeInto(const std::string& sel, const std::string& value);

  // Secret (vault) TYPE steps — see kSecretKey in automation_security.h.
  // The password is looked up off the UI thread by the EXACT host of the
  // page being filled and only ever written into a type=password field on
  // that same host.
  void OnSecretTargetResolved(const std::string& sel);
  void OnSecretTargetProbed(const std::string& sel,
                            agent_safety::ElementProbe probe);
  void OnVaultPassword(const std::string& sel,
                       const std::string& host,
                       std::optional<std::string> password);
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
  // Re-arms the current step's timeout to |budget| from now, for steps that
  // legitimately wait longer (an OS keychain prompt, LLM inference).
  void ExtendStepTimeout(base::TimeDelta budget);

  // The step's text anchor (extra.text, or the description's first quoted
  // token for old recordings), with {{variables}} substituted.
  std::string TextHintOf(const Step& s) const;

  // Resolve {{variable}} placeholders in |raw| using current variables_.
  // With |fence_values|, each substituted value is wrapped in
  // agent_safety::FenceUntrustedText — for LLM prompts, where a value
  // captured from a page (EXTRACT / AI_EXTRACT) must read as data.
  std::string Resolve(const std::string& raw, bool fence_values = false) const;

  // Run JS in the active frame and pass the result to |cb|. The expression
  // must produce a JSON-serialisable value.
  void EvalJS(const std::string& script,
              base::OnceCallback<void(base::Value)> cb);

  // Runs the recorded selector + each fallback until one matches; returns
  // the matching selector, or empty string if none found. If all recorded
  // candidates fail, falls back to AskLLMForSelector() which queries the
  // BrowserAIRuntime for a recovery selector. A step with fallback
  // locators (Step::locators) goes through ResolveWithLocators() instead.
  // Either way found_by_ says how the element was found.
  void ResolveSelector(
      const Step& s,
      base::OnceCallback<void(const std::string&)> cb);

  // Steps with Step::locators (automation_locators.h): one probe of the
  // recorded selector and every locator, then locators::Decide(): a
  // strong match is pinned and acted on; a weak or conflicting one only
  // once the user allows it (AskUser); none, the on-device model's
  // suggestion, checked as a locator and asked about too.
  void ResolveWithLocators(const Step& s,
                           base::OnceCallback<void(const std::string&)> cb);
  void OnLocatorProbe(std::string probe_id,
                      std::vector<locators::Locator> order,
                      locators::StepLocators recorded,
                      std::string description,
                      bool from_model,
                      base::OnceCallback<void(const std::string&)> cb,
                      base::Value report);
  void OnLocatorModelSelector(std::string description,
                              locators::StepLocators recorded,
                              base::OnceCallback<void(const std::string&)> cb,
                              const std::string& selector);
  void OnWeakMatchAnswer(std::string probe_id,
                         size_t candidate,
                         std::string found_by,
                         base::OnceCallback<void(const std::string&)> cb,
                         bool allowed);
  // Marks the probe's candidate in the page (an unguessable attribute,
  // and the element kept in the isolated world) and answers its selector;
  // the click / type scripts then act only on that very element.
  void PinLocated(std::string probe_id,
                  size_t candidate,
                  std::string found_by,
                  base::OnceCallback<void(const std::string&)> cb);
  // The statement the action scripts run on the element they found: for a
  // pinned element, that it is the one pinned (else they do nothing).
  std::string PinStatement() const;

  // Asks the user (RunQuestions, answered in the Agent studio) and waits
  // at most kQuestionTimeoutSeconds; no answer stops the run on this step
  // (|needs_trust|: trusting the workflow would have let it through).
  void AskUser(const std::string& kind,
               const std::string& text,
               bool needs_trust,
               base::OnceCallback<void(bool)> on_answer);
  void OnUserAnswer(base::OnceCallback<void(bool)> on_answer, bool allowed);
  void OnQuestionTimeout(bool needs_trust);
  void WithdrawQuestion();

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
  // and ask for a CSS selector ({"selector": ...}, read with
  // ai_steps::ParseSelectorReply). Returns "" if the model fails.
  void AskLLMForSelector(
      const std::string& description,
      base::OnceCallback<void(const std::string&)> cb);

  // BrowserAIRuntime::RunPrompt blocks for the whole generation — and, while
  // a chat reply is streaming, waits on the runtime's generation lock — so
  // it never runs on the UI thread. RunPromptAsync posts it to a MayBlock
  // ThreadPool worker and replies here; |reply| should be bound to
  // weak_factory_ so a step that ended meanwhile drops the result.
  void RunPromptAsync(std::string prompt,
                      PromptOptions opts,
                      base::OnceCallback<void(GenerationResult)> reply);
  void OnPromptDone(base::OnceCallback<void(GenerationResult)> reply,
                    GenerationResult result);
  void OnSelectorLLMReply(base::OnceCallback<void(const std::string&)> cb,
                          GenerationResult result);
  // AI_DECIDE / AI_EXTRACT answers (ai_steps): stored only when they parse
  // and validate; otherwise the step fails and nothing is stored.
  void OnAIDecideReply(std::string store_as, GenerationResult result);
  void OnAIExtractReply(std::string store_as,
                        ai_steps::ExtractSpec spec,
                        GenerationResult result);

  void EmitStepProgress(bool starting, bool succeeded,
                        const std::string& note);
  void Finish(bool success, const std::string& message);

  // ---- State ----
  raw_ptr<content::WebContents> target_contents_;  // not owned
  raw_ptr<BrowserAIRuntime> ai_runtime_;           // not owned
  raw_ptr<AutomationStorage> storage_;             // not owned

  Script script_;
  // AutomationSecurity::EffectivePolicy(script_), fixed at Run(); RUN_SCRIPT
  // widens its whitelist with the child script's hosts.
  AutomationSecurity security_;
  SecurityPolicy policy_;
  // The run was stopped by a block that trusting the workflow would lift.
  bool blocked_needs_trust_ = false;
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

  // Per-step trace for the Runs review timeline. Accumulated as steps reach a
  // terminal outcome; moved into the RunRecord at Finish().
  std::vector<StepResult> current_run_steps_;
  // When the current step began — for per-step timing.
  base::TimeTicks step_start_;
  // Stable per-run id (unix seconds at Run() start), used to name the
  // per-step screenshots (run_<ts>_step_<n>.png) in the artifacts dir.
  int64_t run_ts_ = 0;
  // Keeps the tab compositing (and reporting visible) for the whole run so
  // per-step screenshots capture pixels even when the run window is minimized
  // (Auto mode). Released when the run object is destroyed.
  base::ScopedClosureRunner capture_keepalive_;
  // Live during a click's post-navigation settle; reset before the next step.
  std::unique_ptr<NavigationSettleWaiter> nav_waiter_;

  // Per-step timeout — fires OnStepTimeout() if the dispatched step
  // doesn't call OnStepFinished in time.
  base::OneShotTimer step_timeout_timer_;

  // How the current step's element was found (StepResult::found_by), and
  // the token of the element pinned for it (automation_locators.h), if
  // any. Reset at every step.
  std::string found_by_;
  std::string pin_token_;
  // The question the current step waits on (RunQuestions), or 0.
  int64_t question_id_ = 0;

  StepProgressCallback on_step_;
  RunCompleteCallback on_complete_;

  // True while a RunPrompt task is on a worker. A request made meanwhile (a
  // step retried after its timeout abandoned the first one) waits in
  // queued_prompt_ instead of stacking a second inference behind the
  // runtime's generation lock.
  bool llm_in_flight_ = false;
  std::unique_ptr<QueuedPrompt> queued_prompt_;

  // Only for OnPromptDone (clears llm_in_flight_, counts tokens); unlike
  // weak_factory_ it is not invalidated at step boundaries.
  base::WeakPtrFactory<AutomationRunner> llm_weak_factory_{this};
  base::WeakPtrFactory<AutomationRunner> weak_factory_{this};
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RUNNER_H_
