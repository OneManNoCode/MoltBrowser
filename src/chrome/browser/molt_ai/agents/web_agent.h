// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebAgent — autonomous web browsing agent.
//
// Implements a ReAct (Reason+Act) loop:
//   IterateOnce → AskLLM → OnLLMDecision → DispatchAction
//   → (DOM/nav callback) → ObservePage → OnPageObserved
//   → OnActionDone → IterateOnce  [loop]
//
// All methods run on the browser UI thread except RunPrompt() which is
// dispatched via ThreadPool and the reply comes back to the UI thread.
//
// Usage (from chat handler):
//   web_agent_ = std::make_unique<WebAgent>(web_contents, runtime);
//   web_agent_->Start("Find cheapest flight NYC to Tokyo",
//       base::BindRepeating(&OnStep), base::BindOnce(&OnDone));

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/dom/dom_content_bridge.h"
#include "chrome/browser/molt_ai/dom/dom_interpreter.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class WebContents;
}

namespace molt_ai {

// ============================================================
// WebAgent — ReAct-based autonomous browsing agent
// ============================================================
class WebAgent : public content::WebContentsObserver {
 public:
  // One step in the agent's execution trace.
  struct Step {
    int number = 0;
    std::string action;      // NAVIGATE, SEARCH, CLICK, TYPE, FILL_FORM,
                             // SCROLL, OBSERVE, DONE, ERROR
    std::string target;      // URL, CSS selector, or search query
    std::string value;       // Text to type, scroll direction, etc.
    std::string reason;      // LLM's stated rationale
    std::string observation; // Page snapshot taken after the action
    bool success = false;
    std::string note;        // Short result note (e.g., "Navigated to …")
  };

  // Called on the UI thread once per completed step.
  using StepCallback = base::RepeatingCallback<void(const Step&)>;

  // Called when the agent finishes (success or failure).
  using DoneCallback =
      base::OnceCallback<void(bool success, std::string result)>;

  WebAgent(content::WebContents* web_contents, BrowserAIRuntime* runtime);
  ~WebAgent() override;

  // Start the agent. Idempotent — calling twice cancels the first run.
  void Start(const std::string& goal,
             StepCallback on_step,
             DoneCallback on_done);

  // Cancel in-progress execution. Safe to call at any time.
  void Cancel();

  // content::WebContentsObserver:
  void DidStopLoading() override;

 private:
  // -- ReAct loop --------------------------------------------------
  void IterateOnce();     // Entry: kick off one LLM round-trip
  void AskLLM();         // Post RunPrompt to ThreadPool
  void OnLLMDecision(std::string response); // Parse LLM output → pending_step_
  void DispatchAction(); // Route pending_step_ to the right executor

  // -- Executors ----------------------------------------------------
  void DoNavigate();          // LoadURL + wait DidStopLoading + ObservePage
  void DoSearch();            // Navigate to search URL (same as DoNavigate)
  void DoClick();             // querySelector + click()
  void DoType();              // querySelector + set value + events
  void DoFillForm();          // Multiple selector=value pairs
  void DoScroll();            // window.scrollBy
  void DoObserve();           // Just ObservePage (explicit OBSERVE action)

  // -- Page observation --------------------------------------------
  void ObservePage();  // Get structured page → OnPageObserved
  void OnPageObserved(const StructuredPage& page);

  // -- Navigation wait helpers ------------------------------------
  void MaybeNavDone();  // Called by DidStopLoading + 12 s safety timeout

  // -- Action completion ------------------------------------------
  void OnActionDone(bool ok, std::string note);
  void Finish(bool success, const std::string& result);

  // -- JS execution -----------------------------------------------
  // Executes |script| in isolated world; calls |cb| with the result.
  void EvalJS(const std::string& script,
              base::OnceCallback<void(base::Value)> cb);

  // -- LLM context ------------------------------------------------
  std::string BuildPrompt() const;
  std::string HistoryText() const;
  bool ParseLLMResponse(const std::string& text);

  // -- State -------------------------------------------------------
  raw_ptr<content::WebContents> web_contents_;  // not owned
  raw_ptr<BrowserAIRuntime> runtime_;           // not owned
  std::unique_ptr<DOMContentBridge> dom_bridge_;

  std::string goal_;
  StepCallback step_cb_;
  DoneCallback done_cb_;

  std::vector<Step> history_;
  Step pending_step_;
  std::string current_page_state_;  // compact text from last ObservePage

  int iteration_ = 0;
  static constexpr int kMaxIterations = 20;

  bool cancelled_ = false;
  bool waiting_for_nav_ = false;  // Expecting DidStopLoading

  base::WeakPtrFactory<WebAgent> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_
