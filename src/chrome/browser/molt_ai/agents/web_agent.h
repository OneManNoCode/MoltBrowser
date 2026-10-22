// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebAgent — autonomous web browsing agent.
//
// Implements a ReAct (Reason+Act) loop:
//   IterateOnce → AskLLM → OnLLMDecision → DispatchAction
//   → (DOM/nav callback) → ObservePage → OnPageObserved
//   → OnElementsObserved → OnActionDone → IterateOnce  [loop]
//
// The model picks each step as one JSON object, under grammar-constrained
// decoding (agent_actions.h, docs/design/AGENT_ACTIONS.md): one of the
// actions below, acting on an element id (e1, e2, ...) from the last
// observation rather than a CSS selector of its own. The browser parses
// and validates the reply again; an unusable reply is asked for again a
// bounded number of times, then the run stops.
//
// All methods run on the browser UI thread except RunPrompt() which is
// dispatched via ThreadPool and the reply comes back to the UI thread.
//
// Usage (from chat handler):
//   web_agent_ = std::make_unique<WebAgent>(web_contents, runtime);
//   web_agent_->Start("Find cheapest flight NYC to Tokyo",
//       base::BindRepeating(&OnStep), base::BindOnce(&OnDone),
//       base::BindRepeating(&OnConfirmNeeded));
//   ... later, when the user taps Allow / Deny:
//   web_agent_->ResolveConfirmation(request.id, allowed);
//
// Safety model (all page text and model output is untrusted):
//   - NAVIGATE / SEARCH only ever load http(s) URLs.
//   - A run is scoped to the origin the tab was on when it started, plus
//     origins the goal names explicitly and the built-in search engine.
//     Leaving that set (directly, via a link, or because the page moved),
//     submitting a form, and typing into a payment-card / one-time-code
//     field pause the run and ask the user. Without a confirmation UI the
//     run stops instead (fail closed).
//   - The agent never types into password fields.
//   - Page content reaches the LLM fenced and sanitized
//     (agent_safety::FenceUntrustedText) with rules that it is data only.
//   - The model can't name arbitrary elements: CLICK / TYPE / FILL_FORM
//     take an element id from the observation (TYPE / FILL_FORM only text
//     fields, never a password field), which the browser maps to the
//     selector its own snapshot script built.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/dom/dom_content_bridge.h"
#include "chrome/browser/molt_ai/dom/dom_interpreter.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/origin.h"

class GURL;

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
    std::string target;      // URL, search query, the element acted on
                             // (`button "Search"`), or the DONE / ERROR
                             // result
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

  // A sensitive action the run is paused on until the user allows or
  // denies it (see the safety model above).
  struct ConfirmRequest {
    int id = 0;
    std::string action;   // NAVIGATE, CLICK, TYPE, FILL_FORM or CONTINUE
    std::string message;  // e.g. "Open https://example.com"
  };
  // Called on the UI thread when the run pauses for approval. Answer with
  // ResolveConfirmation(request.id, allowed).
  using ConfirmCallback = base::RepeatingCallback<void(const ConfirmRequest&)>;

  WebAgent(content::WebContents* web_contents, BrowserAIRuntime* runtime);
  ~WebAgent() override;

  // Start the agent. Idempotent — calling twice cancels the first run.
  // A null |on_confirm| makes every sensitive action stop the run.
  void Start(const std::string& goal,
             StepCallback on_step,
             DoneCallback on_done,
             ConfirmCallback on_confirm);

  // Cancel in-progress execution. Safe to call at any time.
  void Cancel();

  // The user's answer to the pending ConfirmRequest |request_id|. Allow
  // resumes the paused action; deny stops the run. Stale or unknown ids
  // are ignored.
  void ResolveConfirmation(int request_id, bool allowed);

  // content::WebContentsObserver:
  void DidStopLoading() override;

 private:
  // -- ReAct loop --------------------------------------------------
  void IterateOnce();     // Entry: kick off one LLM round-trip
  void AskLLM();         // Post RunPrompt to ThreadPool
  // Validates the reply (agent_actions::InterpretReply) → pending_action_,
  // asks again, or stops the run.
  void OnLLMDecision(GenerationResult result);
  void DispatchAction(); // Origin check, then RunAction()
  void AllowOriginAndDispatch(const url::Origin& origin);
  void RunAction();      // Route pending_action_ to the right executor

  // -- Executors ----------------------------------------------------
  void DoNavigate(const std::string& spec);  // http(s) + origin check
  void DoSearch();            // Navigate to the search URL for the query
  void DoClick();             // probe, confirm if needed, then click()
  void DoType();              // probe, confirm if needed, then set value
  void DoFillForm();          // Several text fields
  void DoScroll();            // window.scrollBy, then observe
  void DoObserve();           // Just ObservePage (explicit OBSERVE action)

  // Second halves of the executors, run once the element probe (and any
  // user confirmation) is done.
  using FieldList = std::vector<std::pair<std::string, std::string>>;
  void LoadAndWait(const GURL& url);
  void AllowOriginAndLoad(const url::Origin& origin, const GURL& url);
  void OnClickProbed(std::vector<agent_safety::ElementProbe> probes);
  void AllowOriginAndClick(std::optional<url::Origin> origin);
  void ClickNow();
  void OnTypeProbed(std::vector<agent_safety::ElementProbe> probes);
  void TypeNow();
  void OnFillFormProbed(FieldList fields,
                        std::vector<agent_safety::ElementProbe> probes);
  void FillNow(FieldList fields, std::string note);

  // Probes |selectors| in the page (see agent_safety::ElementProbe).
  void ProbeElements(
      const std::vector<std::string>& selectors,
      base::OnceCallback<void(std::vector<agent_safety::ElementProbe>)> cb);

  // -- Page observation --------------------------------------------
  void ObservePage();  // Get structured page → OnPageObserved
  void OnPageObserved(const StructuredPage& page);
  void AllowOriginAndContinue(const url::Origin& origin,
                              const StructuredPage& page);
  // Lists the page's interactive elements (the ids the model may use).
  void ContinueAfterObserve(const StructuredPage& page);
  void OnElementsObserved(const StructuredPage& page, base::Value snapshot);

  // -- Origin scope / user confirmation ------------------------------
  bool IsOriginAllowed(const url::Origin& origin) const;
  void AllowOrigin(const url::Origin& origin);  // no-op for non-http(s)
  std::string CurrentOriginText() const;
  // Pauses the run on |message|; runs |on_allow| if the user allows it and
  // stops the run if they deny it (or if there is no ConfirmCallback).
  void RequestConfirmation(const std::string& action,
                           const std::string& message,
                           base::OnceClosure on_allow);

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
  // pending_step_ for the trace and the UI: the action's name, and the
  // elements it acts on described rather than as ids or selectors.
  Step DescribeAction(const agent_actions::AgentAction& action) const;

  // -- State -------------------------------------------------------
  raw_ptr<content::WebContents> web_contents_;  // not owned
  raw_ptr<BrowserAIRuntime> runtime_;           // not owned
  std::unique_ptr<DOMContentBridge> dom_bridge_;

  std::string goal_;
  StepCallback step_cb_;
  DoneCallback done_cb_;
  ConfirmCallback confirm_cb_;

  // Origins this run may act on without asking (see the safety model).
  std::vector<url::Origin> allowed_origins_;
  // Origin of the page the current CLICK / TYPE / FILL_FORM was approved
  // for; the write scripts refuse to act if location.origin differs.
  url::Origin action_origin_;

  // The action the run is paused on, if any.
  base::OnceClosure pending_confirm_;
  std::string pending_confirm_message_;
  int pending_confirm_id_ = 0;
  int next_confirm_id_ = 0;

  std::vector<Step> history_;
  Step pending_step_;
  std::string current_page_state_;  // compact text from last ObservePage
  // The elements of the last observation: the ids the schema offers and
  // ParseAction accepts, and the selectors they stand for.
  agent_actions::Observation observation_;
  // The validated action being carried out.
  agent_actions::AgentAction pending_action_;
  // The selector of the CLICK / TYPE target, resolved from observation_
  // when the action starts.
  std::string action_selector_;
  // Unusable model replies (see agent_actions::InterpretReply): in a row
  // for the current step, and in the whole run; and the note the next
  // prompt carries about the last one.
  int bad_replies_in_a_row_ = 0;
  int bad_replies_total_ = 0;
  std::string retry_note_;

  int iteration_ = 0;
  static constexpr int kMaxIterations = 20;

  bool cancelled_ = false;
  bool waiting_for_nav_ = false;  // Expecting DidStopLoading

  base::WeakPtrFactory<WebAgent> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_
