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
// With the planner (#molt-agent-planner, agent_plan.h), IterateOnce first
// asks for a plan (AskPlanner → OnPlanReply) after the first observation
// and whenever the plan is to be rewritten; each action then says which
// plan step it works on.
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
//   - When the loaded model sees images, each observation may also carry a
//     screenshot of the visible viewport (AgentScreenshotter): this tab
//     only, an http(s) page in scope, sensitive fields and frames masked,
//     the element ids tagged. It goes with the user turn as page content
//     and changes nothing above: the reply is still one action on an id.
//   - A page that offers tools (WebMCP, document.modelContext) gets a TOOLS
//     list in its observation and the model may CALL_TOOL one by id
//     (agent_tools.h): the main document's own tools and those of its
//     frames of exactly the same origin (never a frame inside a
//     cross-origin one), listed and called from the agent's isolated world
//     on the page the run is in scope for (SiteToolHost); names,
//     descriptions and answers are fenced page text; arguments are checked
//     against the tool's (converted) schema; a call runs only in the very
//     document that listed the tool, if it still lists the same tool, and
//     one the page doesn't annotate read-only asks the user first (naming
//     the frame when it isn't the main document's).
//   - A frame of another site that the page embeds (with the `tools`
//     policy delegated) has its tools listed only once the user allows
//     that frame's origin on the page's origin (frame_tools.h): the run
//     asks ("FRAME_TOOLS": Not now / This session / Always) when such a
//     frame has tools, and "Not now" only leaves them out. Their tools
//     are named with the frame's origin, every call asks (read-only or
//     not) naming both sites, and a call whose arguments look like a
//     secret is refused. Never in MoltNet.
//   - A plan (agent_plan.h) is model output written from the goal and
//     page text: it reaches the model fenced, never as the GOAL, and the
//     user as text. It grants nothing: the browser never reads it for
//     URLs, origins, elements or tools, and every action goes through the
//     checks above exactly as without a plan.
//
// The page map (#molt-agent-page-map, agent_page_map.h,
// docs/design/AGENT_ACTIONS.md §2.10): outside MoltNet windows the run
// holds the tab's VoicePageController (voice mode's hold on a tab) and
// observes each page through its accessibility page map: the elements are
// the map's numbered items (e12 is item 12), and CLICK, TYPE, FILL_FORM,
// SELECT and SCROLL act through accessibility actions on their nodes, which
// Blink takes as the user's own interaction (a video the agent starts plays
// with sound). Every action first maps the page again and finds its node
// there, of the same kind and name, in a frame still of the page's origin
// (a stale element is observed again, never guessed); the click's
// questions come from the live node as they come from the element probe,
// and no credential field is typed into. The element snapshot stays the
// fallback for a page whose map is empty or whose tree doesn't come, and
// the only source in MoltNet windows (TOUCHLESS_BROWSING.md §7.4: no
// accessibility mode there). The run releases its hold when it ends; voice
// mode's own hold on the tab stays.
//
// The activation guard (molt_ai/security/activation_guard.h,
// docs/design/AGENT_ACTIONS.md §2.10.8): those accessibility actions give
// the page user activation. While the run holds the page it is the tab's
// asker (unless voice mode is on the tab, which asks itself): a new window,
// a download or another app the page tries with that activation is held and
// asked about as a PAGE_REQUEST confirmation, which doesn't pause the run;
// "Deny" leaves it refused and the run goes on. Full screen and clipboard
// writes are refused; permission prompts are shown as usual, for the user.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/agent_page_map.h"
#include "chrome/browser/molt_ai/agents/agent_plan.h"
#include "chrome/browser/molt_ai/agents/agent_prompt.h"
#include "chrome/browser/molt_ai/agents/agent_screenshot.h"
#include "chrome/browser/molt_ai/agents/agent_tools.h"
#include "chrome/browser/molt_ai/agents/frame_tools.h"
#include "chrome/browser/molt_ai/agents/site_tool_host.h"
#include "chrome/browser/molt_ai/automation/automation_locators.h"
#include "chrome/browser/molt_ai/dom/dom_content_bridge.h"
#include "chrome/browser/molt_ai/dom/dom_interpreter.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"
#include "chrome/browser/molt_ai/security/activation_guard.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/origin.h"

class GURL;

namespace content {
class WebContents;
}

namespace molt_ai {

namespace voice {
class VoicePageController;
}  // namespace voice

// ============================================================
// WebAgent — ReAct-based autonomous browsing agent
// ============================================================
class WebAgent : public content::WebContentsObserver,
                 public ActivationGuard::Asker {
 public:
  // Where an observation came from: the accessibility page map
  // (agent_page_map.h) or the element snapshot (agent_actions.h).
  enum class ObservationSource { kNone, kPageMap, kElementSnapshot };
  // "page_map", "element_snapshot" or "" (none).
  static std::string_view ObservationSourceName(ObservationSource source);

  // One step in the agent's execution trace.
  struct Step {
    int number = 0;
    std::string action;      // NAVIGATE, SEARCH, CLICK, TYPE, FILL_FORM,
                             // SCROLL, OBSERVE, DONE, ERROR, CALL_TOOL,
                             // REPLAN
    std::string target;      // URL, search query, the element acted on
                             // (`button "Search"`), the tool
                             // (`tool "search"`), or the DONE / ERROR
                             // result
    std::string value;       // Text to type, scroll direction, a tool's
                             // arguments, etc.
    std::string reason;      // LLM's stated rationale
    std::string observation; // Page snapshot taken after the action
    bool success = false;
    std::string note;        // Short result note (e.g., "Navigated to …")
    int plan_step = 0;       // The plan step it worked on (1-based); 0
                             // without a plan
    // What the observation the action was chosen on came from.
    ObservationSource observed_with = ObservationSource::kNone;

    // What the step acted on, so a successful run can be saved as a draft
    // workflow (automation/agent_run_draft.h). The browser's own record,
    // never shown to the model.
    struct Element {
      std::string selector;    // the observation's selector
      std::string kind;        // ObservedElement::kind
      std::string input_type;  // ObservedElement::input_type
      std::string label;       // ObservedElement::label (page text)
      // What the probe found right before the action.
      agent_safety::FieldKind field_kind = agent_safety::FieldKind::kOrdinary;
      std::string text;        // TYPE / FILL_FORM: what was typed
      // Its fallback locators, captured right before the action on the
      // page acted on (automation_locators.h); none in MoltNet, where runs
      // are never saved.
      std::optional<automation::locators::StepLocators> locators;
    };
    std::string url;     // NAVIGATE / SEARCH: the URL loaded
    std::string origin;  // CLICK / TYPE / FILL_FORM: the page acted on
    // CLICK / TYPE: the element. FILL_FORM: the fields filled, or none when
    // only some of them were.
    std::vector<Element> elements;
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
    std::string action;   // NAVIGATE, CLICK, TYPE, FILL_FORM, CALL_TOOL,
                          // CONTINUE, FRAME_TOOLS or PAGE_REQUEST (the
                          // activation guard's: the run doesn't wait for
                          // it, and "Deny" doesn't stop it)
    std::string message;  // e.g. "Open https://example.com"
    // FRAME_TOOLS: whether "Always" may be offered (a profile that keeps
    // answers).
    bool can_remember = false;
  };
  // Called on the UI thread when the run pauses for approval. Answer with
  // ResolveConfirmation(request.id, allowed), or for FRAME_TOOLS with
  // ResolveFrameTools(request.id, grant).
  using ConfirmCallback = base::RepeatingCallback<void(const ConfirmRequest&)>;

  // The run's plan (agent_plan.h), for the UI. Its steps are model text:
  // show them as text only.
  enum class PlanEvent {
    kPlanned,      // the first plan
    kReplanned,    // a new plan replaced the old one
    kAdvanced,     // the current step changed
    kWithoutPlan,  // no usable plan: the run goes on one action at a time
  };
  struct PlanUpdate {
    PlanUpdate();
    ~PlanUpdate();
    PlanUpdate(const PlanUpdate&);
    PlanUpdate& operator=(const PlanUpdate&);

    PlanEvent event = PlanEvent::kPlanned;
    std::vector<agent_plan::PlanStep> steps;  // empty without a plan
    int current = 0;                          // 1-based; 0 without a plan
    int replans = 0;                          // rewrites so far
    std::string note;                         // the browser's words
  };
  // Called on the UI thread when the plan is written or rewritten, its
  // current step changes, or the run goes on without one.
  using PlanCallback = base::RepeatingCallback<void(const PlanUpdate&)>;

  // How the run observed pages: the observations through the page map and
  // through the element snapshot.
  struct ObservationCounts {
    int page_map = 0;
    int element_snapshot = 0;
  };

  // A model in place of the on-device runtime, for browser tests: it gets
  // the raw prompt (ToRawPrompt()) and the options (its json_schema says
  // whether a plan or an action is asked for) and answers at once.
  using ModelForTesting = base::RepeatingCallback<GenerationResult(
      const std::string& prompt,
      const PromptOptions& options)>;

  WebAgent(content::WebContents* web_contents, BrowserAIRuntime* runtime);
  ~WebAgent() override;

  // Start the agent. Idempotent — calling twice cancels the first run.
  // A null |on_confirm| makes every sensitive action stop the run. |on_plan|
  // may be null (the plan is then not shown).
  void Start(const std::string& goal,
             StepCallback on_step,
             DoneCallback on_done,
             ConfirmCallback on_confirm,
             PlanCallback on_plan);

  // Cancel in-progress execution. Safe to call at any time.
  void Cancel();

  // The user's answer to the pending ConfirmRequest |request_id|. Allow
  // resumes the paused action; deny stops the run. Stale or unknown ids
  // are ignored.
  void ResolveConfirmation(int request_id, bool allowed);

  // The user's answer to a pending FRAME_TOOLS question: kNone ("Not now")
  // leaves that frame's tools out for the rest of the run; kSession and
  // kAlways allow its origin on the page's (FrameToolPermissions; the
  // caller checked the click). The run goes on either way. Stale or
  // unknown ids are ignored.
  void ResolveFrameTools(int request_id, frame_tools::Grant grant);

  // Called with each action, as the trace will describe it, right before
  // it runs (voice mode says what the agent is about to do). Set before
  // Start(); may be null.
  void SetActionCallback(StepCallback on_action);

  // The run uses the page map (outside MoltNet windows) even while
  // #molt-agent-page-map is off: voice mode's runs, which act on the page
  // voice mode already reads through it. Set before Start().
  void SetUsePageMap(bool use) { use_page_map_ = use; }

  // How this run has observed pages so far.
  const ObservationCounts& observation_counts() const {
    return observation_counts_;
  }

  // Every WebAgent asks |model| instead of the runtime while it is set;
  // null restores the runtime.
  static void SetModelForTesting(ModelForTesting model);
  static bool HasModelForTesting();

  // content::WebContentsObserver:
  void DidStopLoading() override;

  // ActivationGuard::Asker:
  bool AskAboutPageRequest(int id,
                           const activation::Question& question) override;
  void TellAboutPageRequest(const std::string& text) override;
  void PageRequestQuestionWithdrawn(int id) override;

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
  void DoCallTool();          // a site tool: ask if needed, then call it
  void DoReplan();            // REPLAN: the next request rewrites the plan

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
  void CallToolNow();

  // -- Site tools (WebMCP) ------------------------------------------
  // SiteToolHost lists them (the main document's and its same-origin
  // frames') and calls one, polling the isolated world for the answers.
  // |document| is still the tab's primary main document.
  bool IsCurrentDocument(const content::WeakDocumentPtr& document) const;
  void OnToolCallSettled(SiteToolHost::CallEnd end,
                         agent_tools::CallOutcome outcome);
  // Observes once the page that a tool loaded has loaded.
  void WaitForLoadThenObserve();

  // Captures the fallback locators of |selectors| (the step's elements,
  // in order) into pending_step_, then runs |then|. Not in MoltNet.
  void CaptureLocators(std::vector<std::string> selectors,
                       base::OnceClosure then);

  // Probes |selectors| in the page (see agent_safety::ElementProbe).
  void ProbeElements(
      const std::vector<std::string>& selectors,
      base::OnceCallback<void(std::vector<agent_safety::ElementProbe>)> cb);

  // -- Page observation --------------------------------------------
  void ObservePage();  // Get structured page → OnPageObserved
  void OnPageObserved(const StructuredPage& page);
  void AllowOriginAndContinue(const url::Origin& origin,
                              const StructuredPage& page);
  // Lists the page's interactive elements (the ids the model may use):
  // through the page map when the run uses it, else the element snapshot.
  void ContinueAfterObserve(const StructuredPage& page);
  void ObserveThroughSnapshot(const StructuredPage& page,
                              content::WeakDocumentPtr document);
  // |document| is the one the snapshot ran in.
  void OnElementsObserved(const StructuredPage& page,
                          content::WeakDocumentPtr document,
                          base::Value snapshot);
  // The page's own tools, if any, then the rest of the observation
  // (ContinueObservation()). |observed_origin| is the origin the elements
  // were read at; |document_is_current|: they were read in the tab's
  // primary main document.
  void ListToolsThenContinue(const StructuredPage& page,
                             content::WeakDocumentPtr document,
                             std::string observed_origin,
                             bool origin_allowed,
                             bool document_is_current);
  // The page's own tools (its main document's and its same-origin
  // frames'), if the switch is on and it offers any
  // (agent_tools::ShouldListTools), added to observation_.
  void OnSiteToolsListed(const StructuredPage& page,
                         content::WeakDocumentPtr document,
                         std::string observed_origin,
                         bool origin_allowed,
                         SiteToolHost::Listing listing);
  // The frames of other sites a listing may cover on |main_origin|: those
  // the user allowed there, and the others counted (to ask) unless this
  // run already asked about them. None in MoltNet.
  SiteToolHost::OtherSites OtherSitesFor(const url::Origin& main_origin) const;
  // Asks about the next frame of frame_question_, or (none left) goes on
  // with the observation, listing again if the user allowed one.
  void AskNextFrameQuestion();
  void OnFrameToolsRelisted(SiteToolHost::Listing listing);
  // The page state for the prompt, and the screenshot.
  void ContinueObservation(const StructuredPage& page,
                           content::WeakDocumentPtr document,
                           std::string observed_origin,
                           bool origin_allowed);
  // The observation's screenshot, if any (AgentScreenshotter).
  void OnScreenshot(const StructuredPage& page,
                    std::optional<ModelImage> image);
  // Completes the step the observation was for (or starts the run).
  void FinishObservation(const StructuredPage& page);

  // -- The page map (agent_page_map.h) --------------------------------
  // The tab's controller while this run holds it (null otherwise).
  voice::VoicePageController* PageController();
  void HoldPage();
  void ReleasePage();
  // Whether this observation goes through the page map: the run uses it
  // and the tab shows an http(s) page.
  bool UsePageMapNow();
  // Waits until the page's tree has been quiet a moment (kMapQuietMs) or a
  // deadline (kMapWaitMs) has passed, then maps it.
  void ObserveThroughPageMap(const StructuredPage& page,
                             content::WeakDocumentPtr document);
  void OnPageTreeChanged();
  void OnPageMapSettled();
  // Maps the page and makes the observation from the map, or falls back
  // to the element snapshot when the map is empty or the tree didn't come.
  void FinishPageMapObservation(const StructuredPage& page,
                                content::WeakDocumentPtr document);

  // The node an element of the page map names, found again right before
  // acting (agent_page_map::FindLiveItem()), and what acting on it touches.
  struct LiveTarget {
    voice::PageNodeHandle handle;
    voice::PageItem item;
    agent_safety::ElementProbe probe;
  };
  enum class LiveResult { kFound, kStale, kBlocked };
  LiveResult FindLiveTarget(const agent_actions::ObservedElement& element,
                            LiveTarget* target);
  // The element is gone or changed: the page is observed again and the
  // step fails with |note| (nothing is acted on).
  void ReobserveStale(std::string note);
  // A live result that isn't kFound ends the step.
  void FailLive(LiveResult result, std::string_view what);
  // Acting through the page controller.
  void DoPageMapClick(const agent_actions::ObservedElement& element);
  void AllowOriginAndPress(std::optional<url::Origin> origin);
  void PressNow();
  void DoPageMapType(const agent_actions::ObservedElement& element);
  void DoPageMapFillForm();
  void DoSelect();
  // After an accessibility action (it reaches the page apart from scripts):
  // a moment for its effects, then the page is observed once it has
  // loaded.
  void AfterPageAction();

  // -- Planner (agent_plan.h) ----------------------------------------
  // Asks for a plan (the first, or a new one) before the next action.
  void AskPlanner();
  // Validates the plan (agent_plan::InterpretPlanReply), then asks for the
  // next action; or asks again, goes on without a plan, or stops the run.
  void OnPlanReply(GenerationResult result);
  // The plan is to be rewritten for |reason| before the next action. False
  // when it has been rewritten agent_plan::kMaxReplans times already: the
  // run has stopped.
  bool ScheduleReplan(agent_plan::ReplanReason reason);
  void ReportPlan(PlanEvent event, const std::string& note);
  // What the next action may be and say, with or without a plan.
  agent_actions::ActionSet ActionsForNextStep() const;
  std::optional<agent_actions::PlanSteps> PlanStepsForNextStep() const;

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
  // The goal, history, plan and current page for agent_prompt.
  agent_prompt::PromptInput BuildPromptInput() const;
  std::vector<agent_prompt::HistoryStep> HistoryForPrompt() const;
  // pending_step_ for the trace and the UI: the action's name, and the
  // elements it acts on described rather than as ids or selectors.
  Step DescribeAction(const agent_actions::AgentAction& action) const;

  // -- State -------------------------------------------------------
  raw_ptr<content::WebContents> web_contents_;  // not owned
  raw_ptr<BrowserAIRuntime> runtime_;           // not owned
  std::unique_ptr<DOMContentBridge> dom_bridge_;
  std::unique_ptr<AgentScreenshotter> screenshotter_;

  std::string goal_;
  StepCallback step_cb_;
  DoneCallback done_cb_;
  ConfirmCallback confirm_cb_;
  PlanCallback plan_cb_;
  StepCallback action_cb_;

  // The page map (#molt-agent-page-map, read at Start(); never in MoltNet):
  // whether the run uses it, holds the tab's controller, the observation
  // being made (waiting for the tree), and the current observation's text.
  bool page_map_on_ = false;
  bool use_page_map_ = false;
  bool holds_page_ = false;
  struct PendingMap {
    PendingMap();
    ~PendingMap();

    StructuredPage page;
    content::WeakDocumentPtr document;
    base::TimeTicks started;
  };
  std::unique_ptr<PendingMap> pending_map_;
  base::CallbackListSubscription trees_subscription_;
  base::OneShotTimer map_quiet_timer_;
  base::OneShotTimer map_deadline_timer_;
  std::string page_map_text_;
  ObservationSource observation_source_ = ObservationSource::kNone;
  ObservationCounts observation_counts_;
  // Set when an action found its element stale: the observation that
  // follows ends the step with it.
  std::string stale_note_;

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
  // The activation guard's questions asked in this run: the confirmation's
  // id, the guard's id.
  std::map<int, int> page_request_questions_;

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

  // The current observation's screenshot (masked and tagged), sent with
  // the next request; dropped at the next observation, and by TYPE and
  // FILL_FORM, which change the page without observing it again.
  std::optional<ModelImage> screenshot_;
  // The last request carried |screenshot_|.
  bool sent_screenshot_ = false;
  // A request with a screenshot failed: the rest of the run is text-only.
  bool screenshots_failed_ = false;

  // Lists and calls site tools (null after Cancel()).
  std::unique_ptr<SiteToolHost> site_tools_;
  // The documents the observation's tools were listed in: the main
  // document observed, and each frame by number (agent_tools::SiteTool::
  // frame; [0] is the main document).
  content::WeakDocumentPtr tools_main_document_;
  std::vector<content::WeakDocumentPtr> tool_documents_;
  // The site tool being called (a copy: the observation is replaced when
  // the page is observed again).
  agent_tools::SiteTool action_tool_;
  // The finished call's answer (agent_tools::DescribeToolResult), put
  // into the next observation; and the step's note.
  std::string tool_result_;
  std::string tool_call_note_;

  // An observation paused on FRAME_TOOLS questions: what it goes on with,
  // and the frames still to ask about.
  struct FrameQuestion {
    FrameQuestion();
    ~FrameQuestion();

    StructuredPage page;
    content::WeakDocumentPtr document;
    std::string observed_origin;
    bool origin_allowed = false;
    url::Origin main_origin;
    // The listing before the questions (used as is if nothing is allowed).
    SiteToolHost::Listing listing;
    std::vector<SiteToolHost::Listing::Counted> to_ask;
    // The frame origin of the question pending, and whether one was allowed.
    std::string asking;
    bool any_allowed = false;
  };
  std::unique_ptr<FrameQuestion> frame_question_;
  int pending_frame_question_id_ = 0;
  // "<main origin> <frame origin>" pairs this run asked about.
  std::vector<std::string> frame_pairs_asked_;

  // The planner (#molt-agent-planner, read at Start()), the run's plan and
  // how far it got. |plan_needed_|: a plan is to be written before the next
  // action, the first or a new one (then |earlier_plan_| is the old one and
  // |replan_reason_| the browser's words for why). Unusable plan replies
  // for the plan being written, and the note the next request carries.
  bool planner_on_ = false;
  agent_plan::PlanTracker plan_;
  bool plan_needed_ = false;
  std::string earlier_plan_;
  std::string replan_reason_;
  int bad_plan_replies_ = 0;
  std::string plan_retry_note_;

  int iteration_ = 0;
  static constexpr int kMaxIterations = 20;

  bool cancelled_ = false;
  bool waiting_for_nav_ = false;  // Expecting DidStopLoading

  base::WeakPtrFactory<WebAgent> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_WEB_AGENT_H_
