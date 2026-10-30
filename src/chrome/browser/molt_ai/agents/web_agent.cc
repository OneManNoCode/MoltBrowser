// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/web_agent.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/agents/agent_features.h"
#include "chrome/browser/molt_ai/agents/agent_vision.h"
#include "chrome/browser/molt_ai/agents/frame_tool_permissions.h"
#include "chrome/browser/molt_ai/agents/frame_tools.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace molt_ai {

namespace {

using agent_actions::ActionType;
using agent_actions::ObservedElement;
using agent_safety::ElementProbe;
using agent_safety::FieldKind;

// http(s) origins are the only ones the agent can act on; anything else
// (opaque, chrome://, molt://, file://) never joins the allowed set.
bool IsWebOrigin(const url::Origin& origin) {
  return !origin.opaque() && (origin.scheme() == url::kHttpScheme ||
                              origin.scheme() == url::kHttpsScheme);
}

// Seconds to wait for page load before proceeding.
constexpr int kNavTimeoutSeconds = 12;

// Max page content chars sent to LLM per iteration.
constexpr int kMaxPageChars = 3000;

// Step note when the model asks to type into a password field.
constexpr char kRefusePasswordNote[] =
    "Refused: the agent never types into password fields. Ask the user to "
    "sign in themselves, then continue.";

// The page map (agent_page_map.h): how long the page's tree must be quiet
// before it is mapped, and how long an observation waits for it at most.
constexpr base::TimeDelta kMapQuiet = base::Milliseconds(300);
constexpr base::TimeDelta kMapWait = base::Seconds(2);
// After an accessibility action, before the page is observed: the action
// reaches the renderer apart from the observation's scripts.
constexpr base::TimeDelta kAfterPageAction = base::Milliseconds(600);
// A map's selects whose options are read, per observation.
constexpr size_t kMaxSelectsRead = 40;

// Step note when an element of the page map is gone or changed.
constexpr char kStaleNote[] =
    "Not done: the page changed before the agent could act (the element "
    "moved, changed or went away), so it read the page again. Choose from "
    "the new ELEMENTS.";

using VoicePage = voice::VoicePageController;
using PageActionResult = voice::VoicePageController::ActionResult;

// The model browser tests use instead of the runtime.
WebAgent::ModelForTesting& TestingModel() {
  static base::NoDestructor<WebAgent::ModelForTesting> model;
  return *model;
}

// The parts of a GenerationResult the reply decisions need
// (agent_actions::InterpretReply, agent_plan::InterpretPlanReply).
agent_actions::ModelReply ToModelReply(GenerationResult& result) {
  agent_actions::ModelReply reply;
  reply.success = result.success;
  reply.cancelled = result.was_cancelled;
  reply.constraint_unsupported = result.constraint_unsupported;
  reply.invalid_output = result.invalid_output;
  reply.truncated = result.truncated;
  reply.text = std::move(result.text);
  reply.error_message = std::move(result.error_message);
  return reply;
}

// What a step acted on, for saving the run as a draft workflow. Its
// locators are captured separately (CaptureLocators).
WebAgent::Step::Element StepElement(const ObservedElement& element,
                                    std::string text) {
  WebAgent::Step::Element out;
  out.selector = element.selector;
  out.kind = element.kind;
  out.input_type = element.input_type;
  out.label = element.label;
  out.field_kind = element.field_kind;
  out.text = std::move(text);
  return out;
}

}  // namespace

WebAgent::PendingMap::PendingMap() = default;
WebAgent::PendingMap::~PendingMap() = default;

// static
std::string_view WebAgent::ObservationSourceName(ObservationSource source) {
  switch (source) {
    case ObservationSource::kNone:
      return "";
    case ObservationSource::kPageMap:
      return "page_map";
    case ObservationSource::kElementSnapshot:
      return "element_snapshot";
  }
  return "";
}

// static
void WebAgent::SetModelForTesting(ModelForTesting model) {
  TestingModel() = std::move(model);
}

// static
bool WebAgent::HasModelForTesting() {
  return !TestingModel().is_null();
}

WebAgent::PlanUpdate::PlanUpdate() = default;
WebAgent::PlanUpdate::~PlanUpdate() = default;
WebAgent::PlanUpdate::PlanUpdate(const PlanUpdate&) = default;
WebAgent::PlanUpdate& WebAgent::PlanUpdate::operator=(const PlanUpdate&) =
    default;

// ----------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------

WebAgent::WebAgent(content::WebContents* web_contents,
                   BrowserAIRuntime* runtime)
    : content::WebContentsObserver(web_contents),
      web_contents_(web_contents),
      runtime_(runtime),
      dom_bridge_(std::make_unique<DOMContentBridge>(web_contents)),
      screenshotter_(std::make_unique<AgentScreenshotter>(web_contents)),
      site_tools_(std::make_unique<SiteToolHost>()) {}

WebAgent::~WebAgent() {
  ReleasePage();
}

void WebAgent::SetActionCallback(StepCallback on_action) {
  action_cb_ = std::move(on_action);
}

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

void WebAgent::Start(const std::string& goal,
                     StepCallback on_step,
                     DoneCallback on_done,
                     ConfirmCallback on_confirm,
                     PlanCallback on_plan) {
  cancelled_  = false;
  iteration_  = 0;
  history_.clear();
  current_page_state_.clear();
  pending_step_ = Step();
  pending_action_ = agent_actions::AgentAction();
  action_selector_.clear();
  observation_ = agent_actions::Observation();
  bad_replies_in_a_row_ = 0;
  bad_replies_total_ = 0;
  retry_note_.clear();
  screenshotter_->Cancel();
  screenshot_.reset();
  sent_screenshot_ = false;
  screenshots_failed_ = false;
  action_tool_ = agent_tools::SiteTool();
  site_tools_->Cancel();
  tools_main_document_ = content::WeakDocumentPtr();
  tool_documents_.clear();
  tool_result_.clear();
  tool_call_note_.clear();
  frame_question_.reset();
  pending_frame_question_id_ = 0;
  frame_pairs_asked_.clear();
  ReleasePage();
  page_map_text_.clear();
  observation_source_ = ObservationSource::kNone;
  observation_counts_ = ObservationCounts();
  stale_note_.clear();
  goal_       = goal;
  step_cb_    = std::move(on_step);
  done_cb_    = std::move(on_done);
  confirm_cb_ = std::move(on_confirm);
  plan_cb_    = std::move(on_plan);
  pending_confirm_.Reset();
  pending_confirm_message_.clear();

  // The planner: a plan is written after the first observation, before the
  // first action. Without it the run is what it always was.
  planner_on_ = agent_features::AgentPlannerEnabled();
  plan_ = agent_plan::PlanTracker();
  plan_needed_ = planner_on_;
  earlier_plan_.clear();
  replan_reason_.clear();
  bad_plan_replies_ = 0;
  plan_retry_note_.clear();

  // Origin scope for this run: where the user started, origins the goal
  // names explicitly (the user typed them), and the built-in search engine
  // (SEARCH builds that URL itself; only the query is model-chosen).
  allowed_origins_.clear();
  if (web_contents_ && web_contents_->GetPrimaryMainFrame()) {
    AllowOrigin(web_contents_->GetPrimaryMainFrame()->GetLastCommittedOrigin());
  }
  AllowOrigin(url::Origin::Create(GURL(agent_actions::kSearchURLPrefix)));
  for (const std::string& spec : agent_safety::ExtractExplicitURLs(goal_)) {
    GURL url(spec);
    if (agent_safety::IsAllowedNavigationURL(url))
      AllowOrigin(url::Origin::Create(url));
  }

  DVLOG(1) << "[WebAgent] Start (" << allowed_origins_.size()
           << " allowed origins)";

  // The page map: the tab's accessibility tree, held for the run. Never in
  // MoltNet windows, where a page could notice the accessibility mode
  // (TOUCHLESS_BROWSING.md §7.4): there the element snapshot is the only
  // source, as it was.
  content::WebContents* contents = web_contents();
  page_map_on_ =
      agent_features::AgentPageMapEnabled() && contents &&
      !tor::IsMoltNetProfile(
          Profile::FromBrowserContext(contents->GetBrowserContext()));
  if (page_map_on_) {
    HoldPage();
  }

  // Bootstrap: observe wherever the user is before the first LLM call.
  ObservePage();
}

void WebAgent::Cancel() {
  cancelled_ = true;
  ReleasePage();
  pending_confirm_.Reset();
  frame_question_.reset();
  screenshotter_->Cancel();
  site_tools_->Cancel();
  weak_factory_.InvalidateWeakPtrs();
  DVLOG(1) << "[WebAgent] Cancelled";
}

void WebAgent::ResolveConfirmation(int request_id, bool allowed) {
  if (cancelled_ || !pending_confirm_ || request_id != pending_confirm_id_)
    return;
  base::OnceClosure on_allow = std::move(pending_confirm_);
  if (!allowed) {
    Finish(false, "Stopped — you declined: " + pending_confirm_message_);
    return;
  }
  std::move(on_allow).Run();
}

void WebAgent::ResolveFrameTools(int request_id, frame_tools::Grant grant) {
  if (cancelled_ || !frame_question_ || frame_question_->asking.empty() ||
      request_id != pending_frame_question_id_) {
    return;
  }
  pending_frame_question_id_ = 0;
  const url::Origin frame_origin =
      url::Origin::Create(GURL(frame_question_->asking));
  frame_question_->asking.clear();
  if (grant != frame_tools::Grant::kNone && web_contents_ &&
      !tor::IsMoltNetProfile(Profile::FromBrowserContext(
          web_contents_->GetBrowserContext()))) {
    FrameToolPermissions* permissions =
        FrameToolPermissions::Get(web_contents_->GetBrowserContext());
    if (permissions &&
        permissions->Allow(frame_question_->main_origin, frame_origin, grant)) {
      frame_question_->any_allowed = true;
    }
  }
  AskNextFrameQuestion();
}

// ----------------------------------------------------------------
// WebContentsObserver
// ----------------------------------------------------------------

void WebAgent::DidStopLoading() {
  MaybeNavDone();
}

// ----------------------------------------------------------------
// ReAct loop
// ----------------------------------------------------------------

void WebAgent::IterateOnce() {
  if (cancelled_) return;
  if (iteration_ >= kMaxIterations) {
    Finish(false, "Reached the maximum of " +
                      base::NumberToString(kMaxIterations) + " steps.");
    return;
  }
  if (plan_needed_) {
    AskPlanner();
    return;
  }
  AskLLM();
}

void WebAgent::AskLLM() {
  if (cancelled_) return;

  agent_prompt::AgentPrompt prompt =
      agent_prompt::BuildAgentPrompt(BuildPromptInput());
  BrowserAIRuntime* rt = runtime_;
  auto self = weak_factory_.GetWeakPtr();

  PromptOptions opts;
  opts.max_tokens = agent_actions::kMaxReplyTokens;
  opts.temperature = 0.2f;
  opts.stream = false;
  // The reply must be one action on the elements the model is shown, with
  // or without a screenshot; with a plan, it also names the plan step.
  const std::optional<agent_actions::PlanSteps> plan = PlanStepsForNextStep();
  opts.json_schema = agent_actions::BuildActionSchema(
      observation_, ActionsForNextStep(), plan ? &*plan : nullptr);

  // At most one image, the current observation's; older ones are never
  // sent again.
  sent_screenshot_ = screenshot_.has_value();

  if (HasModelForTesting()) {
    sent_screenshot_ = false;
    GenerationResult result =
        TestingModel().Run(agent_prompt::ToRawPrompt(prompt), opts);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&WebAgent::OnLLMDecision,
                                  weak_factory_.GetWeakPtr(),
                                  std::move(result)));
    return;
  }

  // Generation is blocking. Post it to a background thread; reply on UI.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](BrowserAIRuntime* rt, agent_prompt::AgentPrompt p,
             std::optional<ModelImage> image,
             PromptOptions opts) -> GenerationResult {
            if (!image) {
              // Text-only: one raw prompt, as without vision.
              return rt->RunPrompt(agent_prompt::ToRawPrompt(p), opts);
            }
            // With a screenshot: a chat request, the rules as the system
            // prompt and the image with the (untrusted) user turn.
            ChatPrompt chat;
            chat.system_prompt = std::move(p.system);
            chat.user_message = std::move(p.user);
            chat.user_images.push_back(std::move(*image));
            return rt->StreamChat(chat, TokenCallback(), opts);
          },
          rt, std::move(prompt), screenshot_, std::move(opts)),
      base::BindOnce(
          [](base::WeakPtr<WebAgent> self, GenerationResult result) {
            if (self) self->OnLLMDecision(std::move(result));
          },
          self));
}

void WebAgent::OnLLMDecision(GenerationResult result) {
  if (cancelled_) return;

  const bool images_unsupported = result.images_unsupported;
  agent_actions::ModelReply reply = ToModelReply(result);

  // A request with a screenshot that failed (the model can't take images
  // after all, or the image couldn't be encoded) is asked again without
  // it, and the rest of the run is text-only. Not counted as a bad reply.
  if (sent_screenshot_ &&
      agent_vision::ShouldRetryWithoutImage(reply, images_unsupported)) {
    LOG(WARNING) << "[WebAgent] A request with a screenshot failed; "
                    "continuing without screenshots";
    screenshots_failed_ = true;
    screenshot_.reset();
    AskLLM();
    return;
  }

  // Read against what the request offered: the same observation, action
  // set and plan steps (none of which changes while a request runs).
  const std::optional<agent_actions::PlanSteps> plan = PlanStepsForNextStep();
  agent_actions::ReplyOutcome outcome = agent_actions::InterpretReply(
      reply, observation_, bad_replies_in_a_row_, bad_replies_total_,
      ActionsForNextStep(), plan ? &*plan : nullptr);
  // Model output can carry typed text — log only its shape, never its
  // content.
  DVLOG(1) << "[WebAgent] LLM replied (" << reply.text.size()
           << " chars) decision=" << static_cast<int>(outcome.decision);

  switch (outcome.decision) {
    case agent_actions::ReplyDecision::kStop:
      Finish(false, outcome.message);
      return;
    case agent_actions::ReplyDecision::kRetry:
      // No step is taken or counted; the next prompt says what was wrong.
      LOG(WARNING) << "[WebAgent] Unusable model reply; asking again";
      ++bad_replies_in_a_row_;
      ++bad_replies_total_;
      retry_note_ = std::move(outcome.message);
      AskLLM();
      return;
    case agent_actions::ReplyDecision::kAct:
      break;
  }
  bad_replies_in_a_row_ = 0;
  retry_note_.clear();
  pending_action_ = std::move(outcome.action);
  // The next plan step: the current one is done. This only moves the
  // marker the prompt and the user see; it allows nothing.
  if (plan_.Report(pending_action_.plan_step))
    ReportPlan(PlanEvent::kAdvanced, std::string());
  pending_step_ = DescribeAction(pending_action_);
  if (action_cb_)
    action_cb_.Run(pending_step_);
  if (cancelled_) return;
  DispatchAction();
}

void WebAgent::DispatchAction() {
  if (cancelled_) return;

  const ActionType a = pending_action_.type;
  if (a == ActionType::kClick || a == ActionType::kType ||
      a == ActionType::kFillForm || a == ActionType::kCallTool ||
      a == ActionType::kSelect) {
    // The page may have changed origin since the last observation (a
    // redirect or script navigation after a click), so check again right
    // before acting on it. A site tool runs only on the page it was listed
    // on, which this keeps in the run's scope.
    url::Origin current;
    if (web_contents_ && web_contents_->GetPrimaryMainFrame())
      current = web_contents_->GetPrimaryMainFrame()->GetLastCommittedOrigin();
    if (!IsWebOrigin(current)) {
      // Never drive molt:// / chrome:// pages (settings, vault, ...).
      OnActionDone(false,
                   "Blocked: the agent only acts on http(s) web pages");
      return;
    }
    if (!IsOriginAllowed(current)) {
      RequestConfirmation(
          "CONTINUE",
          "Keep working on " + current.Serialize() + " (the page moved there)",
          base::BindOnce(&WebAgent::AllowOriginAndDispatch,
                         weak_factory_.GetWeakPtr(), current));
      return;
    }
    action_origin_ = current;
  }
  RunAction();
}

void WebAgent::AllowOriginAndDispatch(const url::Origin& origin) {
  AllowOrigin(origin);
  action_origin_ = origin;
  RunAction();
}

void WebAgent::RunAction() {
  if (cancelled_) return;

  DVLOG(1) << "[WebAgent] step=" << (iteration_ + 1)
           << " action=" << pending_step_.action;

  // The page an element action acts on (DispatchAction checked it).
  const ActionType a = pending_action_.type;
  if (a == ActionType::kClick || a == ActionType::kType ||
      a == ActionType::kFillForm || a == ActionType::kSelect) {
    pending_step_.origin = action_origin_.Serialize();
  }

  switch (pending_action_.type) {
    case ActionType::kNavigate: DoNavigate(pending_action_.url); break;
    case ActionType::kSearch:   DoSearch(); break;
    case ActionType::kClick:    DoClick(); break;
    case ActionType::kType:     DoType(); break;
    case ActionType::kFillForm: DoFillForm(); break;
    case ActionType::kScroll:   DoScroll(); break;
    case ActionType::kObserve:  DoObserve(); break;
    case ActionType::kDone:     Finish(true, pending_action_.result); break;
    case ActionType::kError:    Finish(false, pending_action_.result); break;
    case ActionType::kCallTool: DoCallTool(); break;
    case ActionType::kReplan:   DoReplan(); break;
    case ActionType::kSelect:   DoSelect(); break;
  }
}

// ----------------------------------------------------------------
// Action executors
// ----------------------------------------------------------------

void WebAgent::DoNavigate(const std::string& spec) {
  if (!web_contents_) { OnActionDone(false, "No active tab"); return; }
  GURL url(spec);
  if (!url.is_valid()) {
    OnActionDone(false, "Invalid URL: " + spec);
    return;
  }
  // Model output never reaches javascript:, data:, file:, chrome:, molt: …
  if (!agent_safety::IsAllowedNavigationURL(url)) {
    OnActionDone(false,
                 "Blocked: the agent can only open http:// or https:// pages");
    return;
  }
  const url::Origin origin = url::Origin::Create(url);
  if (IsOriginAllowed(origin)) {
    LoadAndWait(url);
    return;
  }
  RequestConfirmation("NAVIGATE", "Open " + origin.Serialize(),
                      base::BindOnce(&WebAgent::AllowOriginAndLoad,
                                     weak_factory_.GetWeakPtr(), origin, url));
}

void WebAgent::AllowOriginAndLoad(const url::Origin& origin, const GURL& url) {
  AllowOrigin(origin);
  LoadAndWait(url);
}

void WebAgent::LoadAndWait(const GURL& url) {
  if (cancelled_) return;
  if (!web_contents_) { OnActionDone(false, "No active tab"); return; }
  waiting_for_nav_ = true;
  web_contents_->GetController().LoadURL(
      url, content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());

  // Safety: proceed after timeout even if DidStopLoading never fires.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::MaybeNavDone, weak_factory_.GetWeakPtr()),
      base::Seconds(kNavTimeoutSeconds));
}

void WebAgent::DoSearch() {
  DoNavigate(agent_actions::BuildSearchURL(pending_action_.query));
}

void WebAgent::ProbeElements(
    const std::vector<std::string>& selectors,
    base::OnceCallback<void(std::vector<ElementProbe>)> cb) {
  EvalJS(agent_safety::BuildElementProbeScript(selectors),
         base::BindOnce(
             [](size_t n,
                base::OnceCallback<void(std::vector<ElementProbe>)> cb,
                base::Value v) {
               std::move(cb).Run(agent_safety::ParseElementProbes(v, n));
             },
             selectors.size(), std::move(cb)));
}

void WebAgent::CaptureLocators(std::vector<std::string> selectors,
                               base::OnceClosure then) {
  // Only for "Save as automation": never in MoltNet, where saved
  // workflows are off.
  content::BrowserContext* context =
      web_contents_ ? web_contents_->GetBrowserContext() : nullptr;
  if (!context || tor::IsMoltNetProfile(Profile::FromBrowserContext(context))) {
    std::move(then).Run();
    return;
  }
  const size_t n = selectors.size();
  EvalJS(automation::locators::BuildCaptureListScript(selectors),
         base::BindOnce(
             [](base::WeakPtr<WebAgent> self, size_t n, base::OnceClosure then,
                base::Value v) {
               if (!self || self->cancelled_)
                 return;
               // Kept only if captured on the page the step acts on.
               std::vector<std::optional<automation::locators::StepLocators>>
                   got = automation::locators::ParseCaptureList(
                       v, n, self->action_origin_.Serialize());
               for (size_t i = 0;
                    i < n && i < self->pending_step_.elements.size(); ++i) {
                 self->pending_step_.elements[i].locators = std::move(got[i]);
               }
               std::move(then).Run();
             },
             weak_factory_.GetWeakPtr(), n, std::move(then)));
}

void WebAgent::DoClick() {
  // ParseAction checked the id against observation_, which hasn't changed
  // since the model was shown it.
  const ObservedElement* element = observation_.Find(pending_action_.ref);
  if (!element) {
    OnActionDone(false, "Click failed: not_found");
    return;
  }
  if (element->from_page_map()) {
    DoPageMapClick(*element);
    return;
  }
  action_selector_ = element->selector;
  pending_step_.elements = {StepElement(*element, std::string())};
  CaptureLocators(
      {action_selector_},
      base::BindOnce(&WebAgent::ProbeElements, weak_factory_.GetWeakPtr(),
                     std::vector<std::string>{action_selector_},
                     base::BindOnce(&WebAgent::OnClickProbed,
                                    weak_factory_.GetWeakPtr())));
}

void WebAgent::OnClickProbed(std::vector<ElementProbe> probes) {
  if (cancelled_) return;
  const ElementProbe& p = probes[0];
  if (!p.found) {
    OnActionDone(false, "Click failed: not_found");
    return;
  }
  // A click that follows a link (or submits a form) to a new origin is a
  // navigation off the run's allowed set; a form submission is a
  // state-changing action. Both need the user's approval (the same
  // question on the page map's path: agent_page_map::CheckClick()).
  agent_page_map::ClickQuestion question = agent_page_map::CheckClick(
      p, allowed_origins_, CurrentOriginText());
  if (question.effects.empty()) {
    ClickNow();
    return;
  }
  std::string what =
      p.label.empty() ? pending_step_.target : "\"" + p.label + "\"";
  RequestConfirmation(
      "CLICK",
      "Click " + what + " — it " + base::JoinString(question.effects, " and "),
      base::BindOnce(&WebAgent::AllowOriginAndClick,
                     weak_factory_.GetWeakPtr(),
                     std::move(question.new_origin)));
}

void WebAgent::AllowOriginAndClick(std::optional<url::Origin> origin) {
  if (origin)
    AllowOrigin(*origin);
  ClickNow();
}

void WebAgent::ClickNow() {
  std::string js = agent_actions::BuildClickScript(action_origin_.Serialize(),
                                                   action_selector_);

  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, base::Value v) {
        if (!self) return;
        bool ok = v.is_string() && v.GetString() == "clicked";
        // After a click, observe the page to capture any changes.
        if (ok) {
          self->ObservePage();
        } else {
          std::string note = v.is_string() ? v.GetString() : "no result";
          self->OnActionDone(false, "Click failed: " + note);
        }
      },
      weak_factory_.GetWeakPtr()));
}

void WebAgent::DoType() {
  const ObservedElement* element = observation_.Find(pending_action_.ref);
  if (!element || !element->typeable) {
    OnActionDone(false, "not_found");
    return;
  }
  if (element->from_page_map()) {
    DoPageMapType(*element);
    return;
  }
  action_selector_ = element->selector;
  pending_step_.elements = {StepElement(*element, pending_action_.text)};
  CaptureLocators(
      {action_selector_},
      base::BindOnce(&WebAgent::ProbeElements, weak_factory_.GetWeakPtr(),
                     std::vector<std::string>{action_selector_},
                     base::BindOnce(&WebAgent::OnTypeProbed,
                                    weak_factory_.GetWeakPtr())));
}

void WebAgent::OnTypeProbed(std::vector<ElementProbe> probes) {
  if (cancelled_) return;
  const ElementProbe& p = probes[0];
  if (!p.found) {
    OnActionDone(false, "not_found");
    return;
  }
  if (!pending_step_.elements.empty())
    pending_step_.elements[0].field_kind = p.field_kind;
  // Model output never fills a password field, confirmed or not.
  if (p.field_kind == FieldKind::kPassword) {
    OnActionDone(false, kRefusePasswordNote);
    return;
  }
  if (agent_safety::IsSensitive(p.field_kind)) {
    RequestConfirmation(
        "TYPE",
        "Type into a " + std::string(agent_safety::FieldKindLabel(p.field_kind)) +
            " field on " + CurrentOriginText(),
        base::BindOnce(&WebAgent::TypeNow, weak_factory_.GetWeakPtr()));
    return;
  }
  TypeNow();
}

void WebAgent::TypeNow() {
  // The observed field's selector and the text to type. The password
  // re-check runs in the same script as the write, so a field swapped in
  // after the probe is still refused.
  std::string js = agent_actions::BuildTypeScript(
      action_origin_.Serialize(), action_selector_, pending_action_.text);

  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, base::Value v) {
        if (!self) return;
        bool ok = v.is_string() && v.GetString() == "typed";
        std::string note = v.is_string() ? v.GetString() : "no result";
        if (note == "refused_password")
          note = kRefusePasswordNote;
        self->OnActionDone(ok, note);
      },
      weak_factory_.GetWeakPtr()));
}

void WebAgent::DoFillForm() {
  if (observation_.from_page_map) {
    DoPageMapFillForm();
    return;
  }
  // Each field is an observed text field (ParseAction checked that).
  FieldList fields;
  std::vector<std::string> selectors;
  pending_step_.elements.clear();
  for (const agent_actions::FormFieldValue& f : pending_action_.fields) {
    const ObservedElement* element = observation_.Find(f.ref);
    if (!element || !element->typeable) continue;
    selectors.push_back(element->selector);
    fields.emplace_back(element->selector, f.text);
    pending_step_.elements.push_back(StepElement(*element, f.text));
  }
  if (fields.empty()) {
    OnActionDone(false, "Filled 0 fields");
    return;
  }
  CaptureLocators(
      selectors,
      base::BindOnce(&WebAgent::ProbeElements, weak_factory_.GetWeakPtr(),
                     selectors,
                     base::BindOnce(&WebAgent::OnFillFormProbed,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(fields))));
}

void WebAgent::OnFillFormProbed(FieldList fields,
                                std::vector<ElementProbe> probes) {
  if (cancelled_) return;
  FieldList keep;
  std::vector<Step::Element> kept_elements;  // in step with |keep|
  int skipped_passwords = 0;
  FieldKind sensitive = FieldKind::kOrdinary;
  for (size_t i = 0; i < fields.size(); ++i) {
    const FieldKind kind = probes[i].field_kind;
    if (kind == FieldKind::kPassword) {
      ++skipped_passwords;  // never filled from model output
      continue;
    }
    if (agent_safety::IsSensitive(kind))
      sensitive = kind;
    keep.push_back(std::move(fields[i]));
    if (i < pending_step_.elements.size()) {
      kept_elements.push_back(pending_step_.elements[i]);
      kept_elements.back().field_kind = kind;
    }
  }
  pending_step_.elements = std::move(kept_elements);
  std::string note;
  if (skipped_passwords > 0) {
    note = " (skipped " + base::NumberToString(skipped_passwords) +
           " password field(s): the agent never types passwords)";
  }
  if (keep.empty()) {
    OnActionDone(false, "Filled 0 fields" + note);
    return;
  }
  if (agent_safety::IsSensitive(sensitive)) {
    RequestConfirmation(
        "FILL_FORM",
        "Fill a " + std::string(agent_safety::FieldKindLabel(sensitive)) +
            " field on " + CurrentOriginText(),
        base::BindOnce(&WebAgent::FillNow, weak_factory_.GetWeakPtr(),
                       std::move(keep), std::move(note)));
    return;
  }
  FillNow(std::move(keep), std::move(note));
}

void WebAgent::FillNow(FieldList fields, std::string note) {
  // Password fields are skipped here too, atomically with the write.
  std::string js =
      agent_actions::BuildFillFormScript(action_origin_.Serialize(), fields);

  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, std::string note, size_t expected,
         base::Value v) {
        if (!self) return;
        if (v.is_string() && v.GetString() == "origin_changed") {
          self->OnActionDone(false, "Not filled: the page changed site");
          return;
        }
        int n = v.is_int() ? v.GetInt() : 0;
        // Which fields a partial fill missed isn't known: nothing of this
        // step can be saved in a draft workflow.
        if (n < 0 || static_cast<size_t>(n) != expected)
          self->pending_step_.elements.clear();
        self->OnActionDone(n > 0, "Filled " + base::NumberToString(n) +
                                      " fields" + note);
      },
      weak_factory_.GetWeakPtr(), std::move(note), fields.size()));
}

void WebAgent::DoScroll() {
  // On a page observed through the page map, as assistive technology
  // scrolls it (the page controller); else the scroll script.
  if (observation_.from_page_map) {
    if (VoicePage* controller = PageController()) {
      const PageActionResult result = controller->ScrollPage(
          pending_action_.direction == "up"
              ? VoicePage::PageScrollAction::kUp
              : VoicePage::PageScrollAction::kDown);
      if (result == PageActionResult::kDone) {
        AfterPageAction();
        return;
      }
    }
  }
  // "down" or "up" (ParseAction allows nothing else).
  const std::string js =
      agent_actions::BuildScrollScript(pending_action_.direction);
  // Observe afterwards: the element list favours what is in view.
  EvalJS(js, base::BindOnce(
      [](base::WeakPtr<WebAgent> self, base::Value) {
        if (self) self->ObservePage();
      },
      weak_factory_.GetWeakPtr()));
}

void WebAgent::DoObserve() {
  ObservePage();
}

void WebAgent::DoReplan() {
  // Nothing happens on the page. OnActionDone records the step and has the
  // plan rewritten before the next action (or stops the run when it has
  // been rewritten too often).
  OnActionDone(true, "Asked for a new plan");
}

// ----------------------------------------------------------------
// Site tools (WebMCP)
// ----------------------------------------------------------------

void WebAgent::DoCallTool() {
  // ParseAction checked the id and the arguments against observation_,
  // which hasn't changed since the model was shown it.
  const agent_tools::SiteTool* tool = observation_.FindTool(pending_action_.tool);
  if (!tool) {
    OnActionDone(false, std::string(agent_tools::CallFailureNote(
                            agent_tools::CallState::kNotFound)));
    return;
  }
  action_tool_ = *tool;
  // Credentials and other secrets stay on the page the user is on: never
  // given to a frame of another site.
  if (action_tool_.cross_origin &&
      frame_tools::ArgumentsLookSecret(pending_action_.tool_arguments)) {
    OnActionDone(false,
                 "Not run: what it would give another site's tool looks like "
                 "a password, a code or a card number.");
    return;
  }
  // DispatchAction set action_origin_: the page's origin, in scope.
  const agent_tools::CallCheck check = agent_tools::CheckToolCall(
      action_tool_, pending_action_.tool_arguments_text,
      action_origin_.Serialize());
  if (check.ask) {
    RequestConfirmation(
        "CALL_TOOL", check.message,
        base::BindOnce(&WebAgent::CallToolNow, weak_factory_.GetWeakPtr()));
    return;
  }
  CallToolNow();
}

void WebAgent::CallToolNow() {
  if (cancelled_) return;
  // In the very document that listed the tool (the main document observed,
  // one of its same-origin frames, or an allowed frame of another site),
  // if it is still there and still counts; the script then checks the
  // document's mark, the origin (action_origin_, in scope, or the other
  // site's frame's) and that the page still lists the same tool, and
  // passes the arguments the browser wrote (agent_tools::ReadArguments).
  const content::WeakDocumentPtr tool_document =
      action_tool_.frame < tool_documents_.size()
          ? tool_documents_[action_tool_.frame]
          : content::WeakDocumentPtr();
  site_tools_->Call(tools_main_document_, tool_document, action_origin_,
                    action_tool_, pending_action_.tool_arguments,
                    base::BindOnce(&WebAgent::OnToolCallSettled,
                                   weak_factory_.GetWeakPtr()));
}

void WebAgent::OnToolCallSettled(SiteToolHost::CallEnd end,
                                 agent_tools::CallOutcome outcome) {
  if (cancelled_) return;
  switch (end) {
    case SiteToolHost::CallEnd::kTimedOut:
      OnActionDone(false, std::string(agent_tools::CallFailureNote(
                              agent_tools::CallState::kTimedOut)));
      return;
    case SiteToolHost::CallEnd::kDocumentChanged:
      // The tool loaded another page (a <form> tool submits its form), in
      // the tab or in its frame: that page is its answer. Observe once it
      // has loaded; a page off the run's sites asks the user first
      // (OnPageObserved).
      tool_call_note_ = action_tool_.frame
                            ? "The tool loaded another page in its frame"
                            : "The tool loaded another page";
      WaitForLoadThenObserve();
      return;
    case SiteToolHost::CallEnd::kSettled:
      break;
  }
  if (outcome.state != agent_tools::CallState::kDone) {
    OnActionDone(false,
                 std::string(agent_tools::CallFailureNote(outcome.state)));
    return;
  }
  // Page text: it goes into the next observation, inside the page fence.
  tool_result_ = agent_tools::DescribeToolResult(action_tool_, outcome);
  tool_call_note_ = "The tool answered";
  // The tool may have changed the page too: observe it again.
  ObservePage();
}

void WebAgent::WaitForLoadThenObserve() {
  if (cancelled_) return;
  if (!web_contents_ || !web_contents_->IsLoading()) {
    ObservePage();
    return;
  }
  waiting_for_nav_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::MaybeNavDone, weak_factory_.GetWeakPtr()),
      base::Seconds(kNavTimeoutSeconds));
}

bool WebAgent::IsCurrentDocument(
    const content::WeakDocumentPtr& document) const {
  content::RenderFrameHost* rfh = document.AsRenderFrameHostIfValid();
  return rfh && web_contents_ && rfh == web_contents_->GetPrimaryMainFrame() &&
         rfh->IsActive();
}

// ----------------------------------------------------------------
// Page observation
// ----------------------------------------------------------------

void WebAgent::ObservePage() {
  if (cancelled_) return;
  if (!web_contents_) {
    current_page_state_ = "(no page loaded)";
    OnPageObserved(StructuredPage{});
    return;
  }
  dom_bridge_->GetStructuredPage(
      base::BindOnce(&WebAgent::OnPageObserved, weak_factory_.GetWeakPtr()));
}

void WebAgent::OnPageObserved(const StructuredPage& page) {
  if (cancelled_) return;

  // The tab can leave the allowed set without a NAVIGATE: a redirect, a
  // page-initiated navigation, a click on a script-driven control. Ask
  // before the agent acts on (or reads instructions from) the new site.
  if (web_contents_ && web_contents_->GetPrimaryMainFrame()) {
    const url::Origin current =
        web_contents_->GetPrimaryMainFrame()->GetLastCommittedOrigin();
    if (IsWebOrigin(current) && !IsOriginAllowed(current)) {
      RequestConfirmation(
          "CONTINUE",
          "Keep working on " + current.Serialize() +
              " (the page moved there)",
          base::BindOnce(&WebAgent::AllowOriginAndContinue,
                         weak_factory_.GetWeakPtr(), current, page));
      return;
    }
  }
  ContinueAfterObserve(page);
}

void WebAgent::AllowOriginAndContinue(const url::Origin& origin,
                                      const StructuredPage& page) {
  AllowOrigin(origin);
  ContinueAfterObserve(page);
}

void WebAgent::ContinueAfterObserve(const StructuredPage& page) {
  if (cancelled_) return;
  // The document the snapshot runs in (EvalJS uses the primary main
  // frame): a screenshot is taken only of that document.
  content::WeakDocumentPtr document;
  if (web_contents_ && web_contents_->GetPrimaryMainFrame())
    document = web_contents_->GetPrimaryMainFrame()->GetWeakDocumentPtr();
  if (UsePageMapNow()) {
    ObserveThroughPageMap(page, std::move(document));
    return;
  }
  ObserveThroughSnapshot(page, std::move(document));
}

void WebAgent::ObserveThroughSnapshot(const StructuredPage& page,
                                      content::WeakDocumentPtr document) {
  if (cancelled_) return;
  EvalJS(agent_actions::BuildElementSnapshotScript(),
         base::BindOnce(&WebAgent::OnElementsObserved,
                        weak_factory_.GetWeakPtr(), page, std::move(document)));
}

void WebAgent::OnElementsObserved(const StructuredPage& page,
                                  content::WeakDocumentPtr document,
                                  base::Value snapshot) {
  if (cancelled_) return;

  std::string snapshot_origin;
  observation_ =
      agent_actions::ParseElementSnapshot(snapshot, &snapshot_origin);
  // Elements only from the page the run is on: an http(s) page in the
  // allowed set (OnPageObserved checked it), and the one the snapshot read.
  url::Origin current;
  if (web_contents_ && web_contents_->GetPrimaryMainFrame())
    current = web_contents_->GetPrimaryMainFrame()->GetLastCommittedOrigin();
  const bool origin_allowed = IsWebOrigin(current) && IsOriginAllowed(current);
  const bool snapshot_is_current = snapshot_origin == current.Serialize();
  if (!origin_allowed || !snapshot_is_current) {
    observation_ = agent_actions::Observation();
  }
  observation_source_ = ObservationSource::kElementSnapshot;
  ++observation_counts_.element_snapshot;
  page_map_text_.clear();
  const bool document_is_current =
      snapshot_is_current && IsCurrentDocument(document);
  ListToolsThenContinue(page, std::move(document), std::move(snapshot_origin),
                        origin_allowed, document_is_current);
}

void WebAgent::ListToolsThenContinue(const StructuredPage& page,
                                     content::WeakDocumentPtr document,
                                     std::string snapshot_origin,
                                     bool origin_allowed,
                                     bool document_is_current) {
  if (cancelled_) return;
  url::Origin current;
  if (web_contents_ && web_contents_->GetPrimaryMainFrame())
    current = web_contents_->GetPrimaryMainFrame()->GetLastCommittedOrigin();
  // The last observation's tools are gone with it.
  tools_main_document_ = content::WeakDocumentPtr();
  tool_documents_.clear();

  // The page's own tools (WebMCP), from the same document and origin.
  agent_tools::ListFacts facts;
  facts.switched_on = agent_features::AgentSiteToolsEnabled();
  facts.origin_allowed = origin_allowed;
  facts.snapshot_is_current = document_is_current;
  if (agent_tools::ShouldListTools(facts)) {
    // The main document's tools and its same-origin frames', the allowed
    // frames of other sites', and how many the others have (SiteToolHost).
    content::WeakDocumentPtr listed = document;
    site_tools_->List(
        std::move(listed), current, OtherSitesFor(current),
        base::BindOnce(&WebAgent::OnSiteToolsListed,
                       weak_factory_.GetWeakPtr(), page, std::move(document),
                       std::move(snapshot_origin), origin_allowed));
    return;
  }
  ContinueObservation(page, std::move(document), std::move(snapshot_origin),
                      origin_allowed);
}

void WebAgent::OnSiteToolsListed(const StructuredPage& page,
                                 content::WeakDocumentPtr document,
                                 std::string observed_origin,
                                 bool origin_allowed,
                                 SiteToolHost::Listing listing) {
  if (cancelled_) return;
  // Frames of other sites with tools the user hasn't decided on: ask about
  // each (the run waits), then go on, listing again if one was allowed.
  if (IsCurrentDocument(document) && !listing.counted.empty() &&
      confirm_cb_) {
    auto question = std::make_unique<FrameQuestion>();
    question->main_origin = url::Origin::Create(GURL(observed_origin));
    for (SiteToolHost::Listing::Counted& counted : listing.counted) {
      const std::string key = observed_origin + " " + counted.origin;
      if (std::find(frame_pairs_asked_.begin(), frame_pairs_asked_.end(),
                    key) == frame_pairs_asked_.end()) {
        frame_pairs_asked_.push_back(key);
        question->to_ask.push_back(std::move(counted));
      }
    }
    if (!question->to_ask.empty()) {
      question->page = page;
      question->document = std::move(document);
      question->observed_origin = std::move(observed_origin);
      question->origin_allowed = origin_allowed;
      question->listing = std::move(listing);
      frame_question_ = std::move(question);
      AskNextFrameQuestion();
      return;
    }
  }
  // Only while the observed document is still the tab's (SiteToolHost
  // listed it and its same-origin frames at the origin its snapshot read).
  if (IsCurrentDocument(document)) {
    // The main document's tools first, then each frame's: at most 8 in
    // all. With the planner on, a tool is kept only if the schema with a
    // plan (the larger one) converts too.
    agent_actions::AddSiteTools(std::move(listing.tools), &observation_,
                                planner_on_);
    tools_main_document_ = document;
    tool_documents_ = std::move(listing.documents);
  }
  DVLOG(1) << "[WebAgent] " << observation_.tools.size() << " site tool(s)";
  ContinueObservation(page, std::move(document), std::move(observed_origin),
                      origin_allowed);
}

SiteToolHost::OtherSites WebAgent::OtherSitesFor(
    const url::Origin& main_origin) const {
  SiteToolHost::OtherSites other_sites;
  if (!web_contents_ || tor::IsMoltNetProfile(Profile::FromBrowserContext(
                            web_contents_->GetBrowserContext()))) {
    return other_sites;  // MoltNet: never another site's frame's tools
  }
  FrameToolPermissions* permissions =
      FrameToolPermissions::Get(web_contents_->GetBrowserContext());
  if (!permissions)
    return other_sites;
  other_sites.listed = permissions->AllowedFrames(main_origin);
  // Count the others' tools only to ask, so not when there is no one to
  // ask.
  other_sites.count_others = !confirm_cb_.is_null();
  return other_sites;
}

void WebAgent::AskNextFrameQuestion() {
  if (cancelled_ || !frame_question_) return;
  FrameQuestion& q = *frame_question_;
  if (!q.to_ask.empty()) {
    const SiteToolHost::Listing::Counted next = std::move(q.to_ask.front());
    q.to_ask.erase(q.to_ask.begin());
    q.asking = next.origin;
    FrameToolPermissions* permissions =
        web_contents_
            ? FrameToolPermissions::Get(web_contents_->GetBrowserContext())
            : nullptr;
    pending_frame_question_id_ = ++next_confirm_id_;
    ConfirmRequest req;
    req.id = pending_frame_question_id_;
    req.action = "FRAME_TOOLS";
    req.message = frame_tools::QuestionText(q.observed_origin, next.origin,
                                            next.tools);
    req.can_remember = permissions && permissions->persistent();
    confirm_cb_.Run(req);
    return;
  }
  std::unique_ptr<FrameQuestion> done = std::move(frame_question_);
  if (done->any_allowed && IsCurrentDocument(done->document)) {
    // Listed again, with the frames now allowed (and nothing counted: this
    // run has asked about every one of them).
    SiteToolHost::OtherSites other_sites = OtherSitesFor(done->main_origin);
    other_sites.count_others = false;
    content::WeakDocumentPtr listed = done->document;
    const url::Origin main_origin = done->main_origin;
    frame_question_ = std::move(done);
    site_tools_->List(std::move(listed), main_origin, std::move(other_sites),
                      base::BindOnce(&WebAgent::OnFrameToolsRelisted,
                                     weak_factory_.GetWeakPtr()));
    return;
  }
  done->listing.counted.clear();
  OnSiteToolsListed(done->page, std::move(done->document),
                    std::move(done->observed_origin), done->origin_allowed,
                    std::move(done->listing));
}

void WebAgent::OnFrameToolsRelisted(SiteToolHost::Listing listing) {
  if (cancelled_ || !frame_question_) return;
  std::unique_ptr<FrameQuestion> done = std::move(frame_question_);
  listing.counted.clear();
  OnSiteToolsListed(done->page, std::move(done->document),
                    std::move(done->observed_origin), done->origin_allowed,
                    std::move(listing));
}

WebAgent::FrameQuestion::FrameQuestion() = default;
WebAgent::FrameQuestion::~FrameQuestion() = default;

void WebAgent::ContinueObservation(const StructuredPage& page,
                                   content::WeakDocumentPtr document,
                                   std::string observed_origin,
                                   bool origin_allowed) {
  if (cancelled_) return;

  // Everything here is page-controlled; BuildPrompt fences it. The element
  // and tool ids are the browser's; labels, tool names, descriptions and a
  // tool's answer are page text. The answer of the tool just called comes
  // first, so the step's history keeps it.
  if (observation_.from_page_map) {
    // The page map's observation (agent_page_map::BuildObservation()):
    // URL, title, headings, lists, text and ELEMENTS, within its budget.
    current_page_state_ = tool_result_ + page_map_text_;
  } else {
    std::string compact = page.ToCompactText();
    if (static_cast<int>(compact.size()) > kMaxPageChars)
      compact = compact.substr(0, kMaxPageChars) + "\n[… truncated]";
    current_page_state_ = tool_result_ +
                           "URL: " + page.page_url +
                           "\nTitle: " + page.page_title +
                           "\n\n" + compact +
                           "\nELEMENTS:\n" +
                           agent_actions::DescribeElements(observation_);
  }
  tool_result_.clear();
  if (!observation_.tools.empty()) {
    current_page_state_ += "TOOLS (functions this page offers):\n" +
                           agent_tools::DescribeTools(observation_.tools);
  }

  // A new observation: the last one's screenshot no longer applies. Take
  // one of this one if the model sees images (AgentScreenshotter decides,
  // and answers at once when it won't capture). Not of a page-map
  // observation: its elements have no selectors to measure and tag.
  screenshot_.reset();
  if (observation_.from_page_map) {
    FinishObservation(page);
    return;
  }
  AgentScreenshotter::Request request;
  request.observation = observation_;
  request.document = std::move(document);
  request.observed_origin = std::move(observed_origin);
  request.origin_allowed = origin_allowed;
  request.model_sees_images = runtime_ && runtime_->LoadedModelSeesImages();
  request.run_allows_images = !screenshots_failed_;
  screenshotter_->Capture(std::move(request),
                          base::BindOnce(&WebAgent::OnScreenshot,
                                         weak_factory_.GetWeakPtr(), page));
}

void WebAgent::OnScreenshot(const StructuredPage& page,
                            std::optional<ModelImage> image) {
  if (cancelled_) return;
  screenshot_ = std::move(image);
  DVLOG(1) << "[WebAgent] Observation " << (screenshot_ ? "with" : "without")
           << " a screenshot";
  FinishObservation(page);
}

void WebAgent::FinishObservation(const StructuredPage& page) {
  if (cancelled_) return;

  // Bootstrap path: first observe before the very first LLM call.
  if (iteration_ == 0 && history_.empty() && pending_step_.action.empty()) {
    IterateOnce();
    return;
  }

  // All other paths — the observe completes the current action.
  pending_step_.observation = current_page_state_;
  // An action that found its element stale observed the page again; it did
  // nothing.
  if (!stale_note_.empty()) {
    std::string note = std::move(stale_note_);
    stale_note_.clear();
    OnActionDone(false, std::move(note));
    return;
  }
  switch (pending_action_.type) {
    case ActionType::kNavigate:
    case ActionType::kSearch:
      OnActionDone(true, "Navigated to " + page.page_url);
      break;
    case ActionType::kClick:
      OnActionDone(true, "Clicked and re-observed page");
      break;
    case ActionType::kScroll:
      OnActionDone(true, "Scrolled");
      break;
    case ActionType::kSelect:
      OnActionDone(true, "Chose \"" + pending_action_.option +
                             "\" and re-observed page");
      break;
    case ActionType::kCallTool:
      OnActionDone(true, tool_call_note_);
      break;
    default:
      OnActionDone(true, "Page observed");
      break;
  }
}

// ----------------------------------------------------------------
// Origin scope / user confirmation
// ----------------------------------------------------------------

bool WebAgent::IsOriginAllowed(const url::Origin& origin) const {
  return std::any_of(allowed_origins_.begin(), allowed_origins_.end(),
                     [&](const url::Origin& o) {
                       return o.IsSameOriginWith(origin);
                     });
}

void WebAgent::AllowOrigin(const url::Origin& origin) {
  if (IsWebOrigin(origin) && !IsOriginAllowed(origin))
    allowed_origins_.push_back(origin);
}

std::string WebAgent::CurrentOriginText() const {
  if (web_contents_ && web_contents_->GetPrimaryMainFrame()) {
    const url::Origin& o =
        web_contents_->GetPrimaryMainFrame()->GetLastCommittedOrigin();
    if (IsWebOrigin(o))
      return o.Serialize();
  }
  return "this page";
}

void WebAgent::RequestConfirmation(const std::string& action,
                                   const std::string& message,
                                   base::OnceClosure on_allow) {
  if (cancelled_) return;
  if (!confirm_cb_) {
    // No way to ask the user: fail closed.
    Finish(false, "Stopped before \"" + message +
                      "\": it needs your approval and no approval prompt "
                      "is available.");
    return;
  }
  pending_confirm_id_ = ++next_confirm_id_;
  pending_confirm_ = std::move(on_allow);
  pending_confirm_message_ = message;
  ConfirmRequest req;
  req.id = pending_confirm_id_;
  req.action = action;
  req.message = message;
  confirm_cb_.Run(req);
}

// ----------------------------------------------------------------
// Navigation wait
// ----------------------------------------------------------------

void WebAgent::MaybeNavDone() {
  if (!waiting_for_nav_) return;
  waiting_for_nav_ = false;
  ObservePage();
}

// ----------------------------------------------------------------
// Action completion / finish
// ----------------------------------------------------------------

void WebAgent::OnActionDone(bool ok, std::string note) {
  if (cancelled_) return;

  // TYPE and FILL_FORM write to the page without observing it again: the
  // screenshot would show the fields as they were. So may a tool call that
  // failed (one that succeeded observed the page again).
  if (pending_action_.type == ActionType::kType ||
      pending_action_.type == ActionType::kFillForm ||
      (pending_action_.type == ActionType::kCallTool && !ok)) {
    screenshot_.reset();
  }

  pending_step_.success = ok;
  pending_step_.note    = note;
  if (pending_step_.observation.empty())
    pending_step_.observation = current_page_state_;
  pending_step_.number = iteration_ + 1;

  // Skip recording the silent bootstrap observe.
  bool is_bootstrap = iteration_ == 0 && history_.empty() &&
                      pending_step_.action == "OBSERVE";
  const bool recorded = !pending_step_.action.empty() && !is_bootstrap;
  if (recorded) {
    history_.push_back(pending_step_);
    if (step_cb_)
      step_cb_.Run(pending_step_);
  }

  iteration_++;
  const ActionType done_type = pending_action_.type;
  pending_step_ = Step();
  pending_action_ = agent_actions::AgentAction();
  action_selector_.clear();

  // The plan is rewritten when the model asked (REPLAN), when actions keep
  // failing on one step, or when one step takes too many actions.
  if (recorded && plan_.has_plan()) {
    const agent_plan::ReplanReason reason =
        done_type == ActionType::kReplan ? agent_plan::ReplanReason::kAsked
                                         : plan_.AfterAction(ok);
    if (reason != agent_plan::ReplanReason::kNone &&
        !ScheduleReplan(reason)) {
      return;  // stopped: the plan has been rewritten too often
    }
  }

  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::IterateOnce, weak_factory_.GetWeakPtr()));
}

void WebAgent::Finish(bool success, const std::string& result) {
  if (cancelled_) return;
  cancelled_ = true;
  pending_confirm_.Reset();
  // The page's accessibility mode goes with the run, unless voice mode
  // holds it too.
  ReleasePage();
  DVLOG(1) << "[WebAgent] Done success=" << success << " (page map: "
           << observation_counts_.page_map << " observations, element "
           << "snapshot: " << observation_counts_.element_snapshot << ")";
  if (done_cb_) std::move(done_cb_).Run(success, result);
}

// ----------------------------------------------------------------
// JS execution
// ----------------------------------------------------------------

void WebAgent::EvalJS(const std::string& script,
                      base::OnceCallback<void(base::Value)> cb) {
  if (!web_contents_ || !web_contents_->GetPrimaryMainFrame()) {
    std::move(cb).Run(base::Value());
    return;
  }
  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(script),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb, base::Value v) {
            std::move(cb).Run(std::move(v));
          },
          std::move(cb)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

// ----------------------------------------------------------------
// LLM prompt construction
// ----------------------------------------------------------------

std::vector<agent_prompt::HistoryStep> WebAgent::HistoryForPrompt() const {
  std::vector<agent_prompt::HistoryStep> history;
  for (const Step& step : history_) {
    agent_prompt::HistoryStep h;
    h.number = step.number;
    h.action = step.action;
    h.target = step.target;
    h.value = step.value;
    h.success = step.success;
    h.note = step.note;
    h.observation = step.observation;
    history.push_back(std::move(h));
  }
  return history;
}

agent_prompt::PromptInput WebAgent::BuildPromptInput() const {
  agent_prompt::PromptInput input;
  input.goal = goal_;
  input.history = HistoryForPrompt();
  input.page_state = current_page_state_;
  input.max_steps = kMaxIterations;
  input.steps_remaining = kMaxIterations - iteration_;
  input.retry_note = retry_note_;
  input.with_screenshot = screenshot_.has_value();
  input.with_tools = !observation_.tools.empty();
  input.with_select = observation_.HasSelectable();
  if (plan_.has_plan()) {
    // Model output: agent_prompt fences it, apart from the GOAL.
    input.plan = agent_plan::DescribePlan(plan_.plan());
    input.plan_progress = plan_.DescribeProgress();
  }
  return input;
}

// ----------------------------------------------------------------
// The page map (agent_page_map.h, docs/design/AGENT_ACTIONS.md §2.10)
// ----------------------------------------------------------------

voice::VoicePageController* WebAgent::PageController() {
  content::WebContents* contents = web_contents();
  if (!holds_page_ || !contents) {
    return nullptr;
  }
  VoicePage* controller = VoicePage::FromWebContents(contents);
  return controller && controller->active() ? controller : nullptr;
}

void WebAgent::HoldPage() {
  content::WebContents* contents = web_contents();
  if (holds_page_ || !contents) {
    return;
  }
  // The accessibility mode for this tab only (VoicePageController::
  // AccessibilityMode()); voice mode may hold it already.
  VoicePage::CreateForWebContents(contents);
  VoicePage::FromWebContents(contents)->Start(VoicePage::Holder::kWebAgent);
  holds_page_ = true;
}

void WebAgent::ReleasePage() {
  map_quiet_timer_.Stop();
  map_deadline_timer_.Stop();
  trees_subscription_ = base::CallbackListSubscription();
  pending_map_.reset();
  if (!holds_page_) {
    return;
  }
  holds_page_ = false;
  if (content::WebContents* contents = web_contents()) {
    if (VoicePage* controller = VoicePage::FromWebContents(contents)) {
      controller->Stop(VoicePage::Holder::kWebAgent);
    }
  }
}

bool WebAgent::UsePageMapNow() {
  content::WebContents* contents = web_contents();
  return page_map_on_ && PageController() && contents &&
         contents->GetPrimaryMainFrame() &&
         agent_safety::IsAllowedNavigationURL(
             contents->GetPrimaryMainFrame()->GetLastCommittedURL());
}

void WebAgent::ObserveThroughPageMap(const StructuredPage& page,
                                     content::WeakDocumentPtr document) {
  VoicePage* controller = PageController();
  if (!controller) {
    ObserveThroughSnapshot(page, std::move(document));
    return;
  }
  pending_map_ = std::make_unique<PendingMap>();
  pending_map_->page = page;
  pending_map_->document = std::move(document);
  pending_map_->started = base::TimeTicks::Now();
  // The renderer sends its tree, and every change to it, apart from the
  // page's load: map it once it has been quiet a moment.
  trees_subscription_ = controller->AddTreesUpdatedCallback(base::BindRepeating(
      &WebAgent::OnPageTreeChanged, weak_factory_.GetWeakPtr()));
  map_quiet_timer_.Start(FROM_HERE, kMapQuiet,
                         base::BindOnce(&WebAgent::OnPageMapSettled,
                                        weak_factory_.GetWeakPtr()));
  map_deadline_timer_.Start(FROM_HERE, kMapWait,
                            base::BindOnce(&WebAgent::OnPageMapSettled,
                                           weak_factory_.GetWeakPtr()));
}

void WebAgent::OnPageTreeChanged() {
  if (!pending_map_) {
    return;
  }
  map_quiet_timer_.Start(FROM_HERE, kMapQuiet,
                         base::BindOnce(&WebAgent::OnPageMapSettled,
                                        weak_factory_.GetWeakPtr()));
}

void WebAgent::OnPageMapSettled() {
  if (cancelled_ || !pending_map_) {
    return;
  }
  VoicePage* controller = PageController();
  const bool waited = base::TimeTicks::Now() - pending_map_->started >= kMapWait;
  // Quiet, but no tree yet (the mode was only just taken): wait for it, up
  // to the deadline.
  if (!waited && controller && !controller->HasPageTree()) {
    map_quiet_timer_.Start(FROM_HERE, kMapQuiet,
                           base::BindOnce(&WebAgent::OnPageMapSettled,
                                          weak_factory_.GetWeakPtr()));
    return;
  }
  map_quiet_timer_.Stop();
  map_deadline_timer_.Stop();
  trees_subscription_ = base::CallbackListSubscription();
  std::unique_ptr<PendingMap> pending = std::move(pending_map_);
  FinishPageMapObservation(pending->page, std::move(pending->document));
}

void WebAgent::FinishPageMapObservation(const StructuredPage& page,
                                        content::WeakDocumentPtr document) {
  if (cancelled_) return;
  VoicePage* controller = PageController();
  content::WebContents* contents = web_contents();
  // No tree came, or the page moved on meanwhile: the element snapshot.
  if (!controller || !contents || !controller->HasPageTree() ||
      !IsCurrentDocument(document)) {
    ObserveThroughSnapshot(page, std::move(document));
    return;
  }
  content::RenderFrameHost* main = contents->GetPrimaryMainFrame();
  const url::Origin current = main->GetLastCommittedOrigin();
  const bool origin_allowed = IsWebOrigin(current) && IsOriginAllowed(current);

  const voice::PageMap map = controller->BuildMap();
  agent_page_map::PageMapInput input;
  input.url = main->GetLastCommittedURL().spec();
  input.origin = current.Serialize();
  // The trees the map holds, and of those the frames of the page's own
  // origin: only their items are listed (another site's frame never is).
  std::vector<int32_t> trees;
  auto note_tree = [&](int32_t tree) {
    if (tree >= 0 && std::find(trees.begin(), trees.end(), tree) == trees.end())
      trees.push_back(tree);
  };
  note_tree(map.page_tree);
  for (const voice::PageItem& item : map.items)
    note_tree(item.handle.tree);
  for (const voice::PageList& list : map.lists)
    note_tree(list.container.tree);
  for (const voice::PageHeading& heading : map.headings)
    note_tree(heading.handle.tree);
  for (int32_t tree : trees) {
    const std::optional<url::Origin> origin = controller->OriginOfTree(tree);
    if (origin && IsWebOrigin(*origin) && origin->IsSameOriginWith(current))
      input.same_origin_trees.push_back(tree);
  }
  // The options of its selects, read from the live tree.
  size_t selects = 0;
  for (const voice::PageItem& item : map.items) {
    if (item.kind != voice::ItemKind::kSelect || item.disabled ||
        std::find(input.same_origin_trees.begin(),
                  input.same_origin_trees.end(),
                  item.handle.tree) == input.same_origin_trees.end()) {
      continue;
    }
    if (++selects > kMaxSelectsRead)
      break;
    input.select_options.emplace_back(item.number,
                                      controller->OptionsOf(item.handle));
  }
  input.page_text = page.main_content;

  agent_page_map::PageMapObservation observed =
      agent_page_map::BuildObservation(map, input);
  if (!observed.usable) {
    // Nothing on the map the agent could use: a canvas page, unlabelled
    // controls, a tree that isn't complete yet.
    ObserveThroughSnapshot(page, std::move(document));
    return;
  }
  observation_ = std::move(observed.observation);
  // Elements only from the page the run is on (OnPageObserved asked about
  // any other), as for the element snapshot.
  if (!origin_allowed)
    observation_.elements.clear();
  page_map_text_ = std::move(observed.text);
  observation_source_ = ObservationSource::kPageMap;
  ++observation_counts_.page_map;
  DVLOG(1) << "[WebAgent] Observed through the page map: "
           << observation_.elements.size() << " element(s)";
  ListToolsThenContinue(page, std::move(document), current.Serialize(),
                        origin_allowed, /*document_is_current=*/true);
}

WebAgent::LiveResult WebAgent::FindLiveTarget(
    const agent_actions::ObservedElement& element,
    LiveTarget* target) {
  VoicePage* controller = PageController();
  content::WebContents* contents = web_contents();
  if (!controller || !contents || !contents->GetPrimaryMainFrame()) {
    return LiveResult::kStale;
  }
  // The page as it is now: the element's node, of the kind and with the
  // name the model was shown.
  const voice::PageMap fresh = controller->BuildMap();
  const voice::PageItem* item = agent_page_map::FindLiveItem(element, fresh);
  if (!item) {
    return LiveResult::kStale;
  }
  PageActionResult error = PageActionResult::kStale;
  std::optional<VoicePage::TargetFacts> facts =
      controller->Inspect(item->handle, &error);
  if (!facts) {
    return error == PageActionResult::kNotAllowed ? LiveResult::kBlocked
                                                  : LiveResult::kStale;
  }
  // Origins per frame: its frame still has the origin it was observed
  // with, which is the page's, a web origin in the run's scope.
  switch (agent_page_map::CheckFrameOrigin(
      element.frame_origin, facts->origin,
      contents->GetPrimaryMainFrame()->GetLastCommittedOrigin(),
      allowed_origins_)) {
    case agent_page_map::FrameCheck::kOk:
      break;
    case agent_page_map::FrameCheck::kStale:
      return LiveResult::kStale;
    case agent_page_map::FrameCheck::kRefused:
      return LiveResult::kBlocked;
  }
  target->handle = item->handle;
  target->item = *item;
  target->probe = agent_page_map::ProbeNode(facts->chain, facts->form_index);
  return LiveResult::kFound;
}

void WebAgent::ReobserveStale(std::string note) {
  if (cancelled_) return;
  stale_note_ = std::move(note);
  ObservePage();
}

void WebAgent::FailLive(LiveResult result, std::string_view what) {
  if (result == LiveResult::kBlocked) {
    OnActionDone(false, std::string(what) +
                            " not done: the agent only acts on this site's "
                            "own http(s) pages");
    return;
  }
  ReobserveStale(kStaleNote);
}

void WebAgent::DoPageMapClick(const agent_actions::ObservedElement& element) {
  pending_step_.elements = {StepElement(element, std::string())};
  LiveTarget target;
  const LiveResult found = FindLiveTarget(element, &target);
  if (found != LiveResult::kFound) {
    FailLive(found, "Click");
    return;
  }
  pending_step_.elements[0].field_kind = target.probe.field_kind;
  // The same questions as for the element snapshot's probe.
  agent_page_map::ClickQuestion question = agent_page_map::CheckClick(
      target.probe, allowed_origins_, CurrentOriginText());
  if (question.effects.empty()) {
    PressNow();
    return;
  }
  const std::string what = target.probe.label.empty()
                               ? pending_step_.target
                               : "\"" + target.probe.label + "\"";
  RequestConfirmation(
      "CLICK",
      "Click " + what + " — it " + base::JoinString(question.effects, " and "),
      base::BindOnce(&WebAgent::AllowOriginAndPress,
                     weak_factory_.GetWeakPtr(),
                     std::move(question.new_origin)));
}

void WebAgent::AllowOriginAndPress(std::optional<url::Origin> origin) {
  if (origin)
    AllowOrigin(*origin);
  PressNow();
}

void WebAgent::PressNow() {
  if (cancelled_) return;
  const ObservedElement* element = observation_.Find(pending_action_.ref);
  if (!element) {
    OnActionDone(false, "Click failed: not_found");
    return;
  }
  // Found again right before the press: the user may have taken a while
  // to answer.
  LiveTarget target;
  const LiveResult found = FindLiveTarget(*element, &target);
  if (found != LiveResult::kFound) {
    FailLive(found, "Click");
    return;
  }
  // The element's default action: Blink notifies user activation and
  // dispatches its click as for assistive technology, so media it starts
  // plays with sound.
  const PageActionResult result = PageController()->Press(target.handle);
  if (result == PageActionResult::kStale) {
    ReobserveStale(kStaleNote);
    return;
  }
  if (result != PageActionResult::kDone) {
    OnActionDone(false, "Click failed: " +
                            std::string(VoicePage::ActionResultName(result)));
    return;
  }
  AfterPageAction();
}

void WebAgent::DoPageMapType(const agent_actions::ObservedElement& element) {
  pending_step_.elements = {StepElement(element, pending_action_.text)};
  LiveTarget target;
  const LiveResult found = FindLiveTarget(element, &target);
  if (found != LiveResult::kFound) {
    FailLive(found, "Typing");
    return;
  }
  pending_step_.elements[0].field_kind = target.probe.field_kind;
  // Never a credential field on this path, whatever the probe or the map
  // says; the controller refuses one again.
  const agent_page_map::TypeCheck check = agent_page_map::CheckType(target.item);
  if (check != agent_page_map::TypeCheck::kType ||
      target.probe.field_kind != FieldKind::kOrdinary) {
    const agent_page_map::TypeCheck refusal =
        check != agent_page_map::TypeCheck::kType
            ? check
            : target.probe.field_kind == FieldKind::kPassword
                  ? agent_page_map::TypeCheck::kRefusePassword
                  : agent_page_map::TypeCheck::kRefuseCredential;
    OnActionDone(false,
                 std::string(agent_page_map::TypeRefusalNote(refusal)));
    return;
  }
  const PageActionResult result =
      PageController()->SetText(target.handle, pending_action_.text);
  switch (result) {
    case PageActionResult::kDone:
      // As for the snapshot's TYPE: the page isn't observed again.
      OnActionDone(true, "typed");
      return;
    case PageActionResult::kStale:
      ReobserveStale(kStaleNote);
      return;
    case PageActionResult::kCredentialField:
      OnActionDone(false, std::string(agent_page_map::TypeRefusalNote(
                              agent_page_map::TypeCheck::kRefuseCredential)));
      return;
    default:
      OnActionDone(false, "Not typed: " + std::string(
                                              VoicePage::ActionResultName(
                                                  result)));
      return;
  }
}

void WebAgent::DoPageMapFillForm() {
  pending_step_.elements.clear();
  int filled = 0;
  int refused = 0;
  for (const agent_actions::FormFieldValue& field : pending_action_.fields) {
    const ObservedElement* element = observation_.Find(field.ref);
    if (!element || !element->typeable) {
      continue;
    }
    LiveTarget target;
    const LiveResult found = FindLiveTarget(*element, &target);
    if (found != LiveResult::kFound) {
      // Nothing more is written to a page that changed.
      pending_step_.elements.clear();
      FailLive(found, "Form fill");
      return;
    }
    if (agent_page_map::CheckType(target.item) !=
            agent_page_map::TypeCheck::kType ||
        target.probe.field_kind != FieldKind::kOrdinary) {
      ++refused;
      continue;
    }
    if (PageController()->SetText(target.handle, field.text) ==
        PageActionResult::kDone) {
      ++filled;
      pending_step_.elements.push_back(StepElement(*element, field.text));
    }
  }
  std::string note = "Filled " + base::NumberToString(filled) + " fields";
  if (refused > 0) {
    note += " (skipped " + base::NumberToString(refused) +
            " password, card or code field(s): the agent never types there)";
  }
  if (static_cast<size_t>(filled) != pending_action_.fields.size())
    pending_step_.elements.clear();
  OnActionDone(filled > 0, std::move(note));
}

void WebAgent::DoSelect() {
  // ParseAction checked the element is a select of the observation and the
  // option one it lists.
  const ObservedElement* element = observation_.Find(pending_action_.ref);
  if (!element || !element->selectable || !element->from_page_map()) {
    OnActionDone(false, "Not chosen: not_found");
    return;
  }
  pending_step_.elements = {StepElement(*element, pending_action_.option)};
  LiveTarget target;
  const LiveResult found = FindLiveTarget(*element, &target);
  if (found != LiveResult::kFound) {
    FailLive(found, "Choosing");
    return;
  }
  const PageActionResult result =
      PageController()->SelectOption(target.handle, pending_action_.option);
  switch (result) {
    case PageActionResult::kDone:
      AfterPageAction();
      return;
    case PageActionResult::kStale:
      ReobserveStale(kStaleNote);
      return;
    case PageActionResult::kNoSuchOption:
      OnActionDone(false,
                   "Not chosen: the select no longer lists that option");
      return;
    default:
      OnActionDone(false, "Not chosen: " + std::string(
                                               VoicePage::ActionResultName(
                                                   result)));
      return;
  }
}

void WebAgent::AfterPageAction() {
  if (cancelled_) return;
  // The action reaches the renderer apart from the scripts that observe
  // the page: give it a moment, then observe once anything it loaded has.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::WaitForLoadThenObserve,
                     weak_factory_.GetWeakPtr()),
      kAfterPageAction);
}

// ----------------------------------------------------------------
// Planner
// ----------------------------------------------------------------

void WebAgent::AskPlanner() {
  if (cancelled_) return;

  agent_prompt::PlanPromptInput input;
  input.goal = goal_;
  input.history = HistoryForPrompt();
  input.page_state = current_page_state_;
  input.earlier_plan = earlier_plan_;
  input.replan_reason = replan_reason_;
  input.retry_note = plan_retry_note_;
  input.max_plan_steps = static_cast<int>(agent_plan::kMaxPlanSteps);
  std::string prompt =
      agent_prompt::ToRawPrompt(agent_prompt::BuildPlanPrompt(input));

  PromptOptions opts;
  opts.max_tokens = agent_plan::kMaxPlanReplyTokens;
  opts.temperature = 0.2f;
  opts.stream = false;
  // The reply must be a plan: 1 to kMaxPlanSteps steps, each capped.
  opts.json_schema = agent_plan::BuildPlanSchema();

  if (HasModelForTesting()) {
    GenerationResult result = TestingModel().Run(prompt, opts);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&WebAgent::OnPlanReply,
                                  weak_factory_.GetWeakPtr(),
                                  std::move(result)));
    return;
  }

  // Text-only, even when the model sees images: a plan needs the page's
  // text, and the screenshot belongs to the next action's request.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](BrowserAIRuntime* rt, std::string p,
             PromptOptions opts) -> GenerationResult {
            return rt->RunPrompt(p, opts);
          },
          runtime_.get(), std::move(prompt), std::move(opts)),
      base::BindOnce(
          [](base::WeakPtr<WebAgent> self, GenerationResult result) {
            if (self) self->OnPlanReply(std::move(result));
          },
          weak_factory_.GetWeakPtr()));
}

void WebAgent::OnPlanReply(GenerationResult result) {
  if (cancelled_) return;

  agent_plan::PlanOutcome outcome = agent_plan::InterpretPlanReply(
      ToModelReply(result), bad_plan_replies_);
  // Model output: log only its shape.
  DVLOG(1) << "[WebAgent] Plan reply: decision="
           << static_cast<int>(outcome.decision);

  switch (outcome.decision) {
    case agent_plan::PlanDecision::kStop:
      Finish(false, outcome.message);
      return;
    case agent_plan::PlanDecision::kRetry:
      LOG(WARNING) << "[WebAgent] Unusable plan; asking again";
      ++bad_plan_replies_;
      plan_retry_note_ = std::move(outcome.message);
      AskPlanner();
      return;
    case agent_plan::PlanDecision::kWithoutPlan:
      // A plan only helps: without one the agent is what it was before the
      // planner, with every check in place.
      LOG(WARNING) << "[WebAgent] No usable plan; going on without one";
      plan_needed_ = false;
      plan_.Drop();
      earlier_plan_.clear();
      replan_reason_.clear();
      bad_plan_replies_ = 0;
      plan_retry_note_.clear();
      ReportPlan(PlanEvent::kWithoutPlan, outcome.message);
      AskLLM();
      return;
    case agent_plan::PlanDecision::kUse:
      break;
  }
  const bool replanned = !earlier_plan_.empty();
  plan_needed_ = false;
  earlier_plan_.clear();
  replan_reason_.clear();
  bad_plan_replies_ = 0;
  plan_retry_note_.clear();
  plan_.Adopt(std::move(outcome.plan));
  ReportPlan(replanned ? PlanEvent::kReplanned : PlanEvent::kPlanned,
             std::string());
  AskLLM();
}

bool WebAgent::ScheduleReplan(agent_plan::ReplanReason reason) {
  const int step = plan_.current_step();
  const int count = static_cast<int>(plan_.plan().size());
  if (!plan_.UseReplan()) {
    Finish(false, agent_plan::kReplanLimitMessage);
    return false;
  }
  earlier_plan_ = agent_plan::DescribePlan(plan_.plan());
  replan_reason_ = agent_plan::ReplanReasonText(reason, step, count);
  plan_needed_ = true;
  bad_plan_replies_ = 0;
  plan_retry_note_.clear();
  return true;
}

void WebAgent::ReportPlan(PlanEvent event, const std::string& note) {
  if (!plan_cb_)
    return;
  PlanUpdate update;
  update.event = event;
  update.steps = plan_.plan();
  update.current = plan_.current_step();
  update.replans = plan_.replans();
  update.note = note;
  plan_cb_.Run(update);
}

agent_actions::ActionSet WebAgent::ActionsForNextStep() const {
  return plan_.has_plan() ? agent_actions::kPlannedAgentActions
                          : agent_actions::kAgentActions;
}

std::optional<agent_actions::PlanSteps> WebAgent::PlanStepsForNextStep()
    const {
  if (!plan_.has_plan())
    return std::nullopt;
  return plan_.Steps();
}

// ----------------------------------------------------------------
// Step description
// ----------------------------------------------------------------

WebAgent::Step WebAgent::DescribeAction(
    const agent_actions::AgentAction& action) const {
  Step step;
  step.action = std::string(agent_actions::ActionName(action.type));
  step.reason = action.reason;
  step.plan_step = action.plan_step;
  step.observed_with = observation_source_;
  auto describe = [&](const std::string& ref) {
    const ObservedElement* e = observation_.Find(ref);
    return e ? agent_actions::DescribeElement(*e) : ref;
  };
  switch (action.type) {
    case ActionType::kNavigate:
      step.target = action.url;
      step.url = action.url;
      break;
    case ActionType::kSearch:
      step.target = action.query;
      step.url = agent_actions::BuildSearchURL(action.query);
      break;
    case ActionType::kClick:
      step.target = describe(action.ref);
      break;
    case ActionType::kType:
      step.target = describe(action.ref);
      step.value = action.text;
      break;
    case ActionType::kFillForm:
      step.target = base::NumberToString(action.fields.size()) + " field(s)";
      for (const agent_actions::FormFieldValue& f : action.fields) {
        if (!step.value.empty())
          step.value += "; ";
        step.value += describe(f.ref) + " = " + f.text;
      }
      break;
    case ActionType::kScroll:
      step.target = action.direction;
      break;
    case ActionType::kObserve:
      step.target = "current";
      break;
    case ActionType::kDone:
    case ActionType::kError:
      step.target = action.result;
      break;
    case ActionType::kCallTool: {
      const agent_tools::SiteTool* tool = observation_.FindTool(action.tool);
      // A frame's tool by the browser's number of its frame (the approval
      // shows the frame's address).
      step.target = tool ? "tool \"" + tool->name + "\"" +
                               (tool->frame ? " in frame " +
                                                  base::NumberToString(
                                                      tool->frame)
                                            : std::string())
                         : action.tool;
      step.value = action.tool_arguments_text;
      break;
    }
    case ActionType::kReplan:
      step.target = "plan step " + base::NumberToString(action.plan_step);
      break;
    case ActionType::kSelect:
      step.target = describe(action.ref);
      step.value = action.option;
      break;
  }
  return step;
}

}  // namespace molt_ai
