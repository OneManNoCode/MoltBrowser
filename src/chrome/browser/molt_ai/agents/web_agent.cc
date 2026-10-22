// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/web_agent.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/agents/agent_features.h"
#include "chrome/browser/molt_ai/agents/agent_vision.h"
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

}  // namespace

// ----------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------

WebAgent::WebAgent(content::WebContents* web_contents,
                   BrowserAIRuntime* runtime)
    : content::WebContentsObserver(web_contents),
      web_contents_(web_contents),
      runtime_(runtime),
      dom_bridge_(std::make_unique<DOMContentBridge>(web_contents)),
      screenshotter_(std::make_unique<AgentScreenshotter>(web_contents)) {}

WebAgent::~WebAgent() = default;

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

void WebAgent::Start(const std::string& goal,
                     StepCallback on_step,
                     DoneCallback on_done,
                     ConfirmCallback on_confirm) {
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
  tool_call_id_.clear();
  tool_document_ = content::WeakDocumentPtr();
  tool_result_.clear();
  tool_call_note_.clear();
  goal_       = goal;
  step_cb_    = std::move(on_step);
  done_cb_    = std::move(on_done);
  confirm_cb_ = std::move(on_confirm);
  pending_confirm_.Reset();
  pending_confirm_message_.clear();

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

  // Bootstrap: observe wherever the user is before the first LLM call.
  ObservePage();
}

void WebAgent::Cancel() {
  cancelled_ = true;
  pending_confirm_.Reset();
  screenshotter_->Cancel();
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
  // or without a screenshot.
  opts.json_schema = agent_actions::BuildActionSchema(observation_);

  // At most one image, the current observation's; older ones are never
  // sent again.
  sent_screenshot_ = screenshot_.has_value();

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

  agent_actions::ModelReply reply;
  reply.success = result.success;
  reply.cancelled = result.was_cancelled;
  reply.constraint_unsupported = result.constraint_unsupported;
  reply.invalid_output = result.invalid_output;
  reply.truncated = result.truncated;
  reply.text = std::move(result.text);
  reply.error_message = std::move(result.error_message);

  // A request with a screenshot that failed (the model can't take images
  // after all, or the image couldn't be encoded) is asked again without
  // it, and the rest of the run is text-only. Not counted as a bad reply.
  if (sent_screenshot_ &&
      agent_vision::ShouldRetryWithoutImage(reply, result.images_unsupported)) {
    LOG(WARNING) << "[WebAgent] A request with a screenshot failed; "
                    "continuing without screenshots";
    screenshots_failed_ = true;
    screenshot_.reset();
    AskLLM();
    return;
  }

  agent_actions::ReplyOutcome outcome = agent_actions::InterpretReply(
      reply, observation_, bad_replies_in_a_row_, bad_replies_total_);
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
  pending_step_ = DescribeAction(pending_action_);
  DispatchAction();
}

void WebAgent::DispatchAction() {
  if (cancelled_) return;

  const ActionType a = pending_action_.type;
  if (a == ActionType::kClick || a == ActionType::kType ||
      a == ActionType::kFillForm || a == ActionType::kCallTool) {
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

void WebAgent::DoClick() {
  // ParseAction checked the id against observation_, which hasn't changed
  // since the model was shown it.
  const ObservedElement* element = observation_.Find(pending_action_.ref);
  if (!element) {
    OnActionDone(false, "Click failed: not_found");
    return;
  }
  action_selector_ = element->selector;
  ProbeElements({action_selector_},
                base::BindOnce(&WebAgent::OnClickProbed,
                               weak_factory_.GetWeakPtr()));
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
  // state-changing action. Both need the user's approval.
  std::vector<std::string> effects;
  std::optional<url::Origin> new_origin;
  GURL dest(p.navigates_to);
  if (agent_safety::IsAllowedNavigationURL(dest)) {
    url::Origin o = url::Origin::Create(dest);
    if (!IsOriginAllowed(o)) {
      effects.push_back("opens " + o.Serialize());
      new_origin = o;
    }
  }
  if (p.submits_form)
    effects.push_back("submits a form on " + CurrentOriginText());
  if (effects.empty()) {
    ClickNow();
    return;
  }
  std::string what =
      p.label.empty() ? pending_step_.target : "\"" + p.label + "\"";
  RequestConfirmation(
      "CLICK", "Click " + what + " — it " + base::JoinString(effects, " and "),
      base::BindOnce(&WebAgent::AllowOriginAndClick,
                     weak_factory_.GetWeakPtr(), std::move(new_origin)));
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
  action_selector_ = element->selector;
  ProbeElements({action_selector_},
                base::BindOnce(&WebAgent::OnTypeProbed,
                               weak_factory_.GetWeakPtr()));
}

void WebAgent::OnTypeProbed(std::vector<ElementProbe> probes) {
  if (cancelled_) return;
  const ElementProbe& p = probes[0];
  if (!p.found) {
    OnActionDone(false, "not_found");
    return;
  }
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
  // Each field is an observed text field (ParseAction checked that).
  FieldList fields;
  std::vector<std::string> selectors;
  for (const agent_actions::FormFieldValue& f : pending_action_.fields) {
    const ObservedElement* element = observation_.Find(f.ref);
    if (!element || !element->typeable) continue;
    selectors.push_back(element->selector);
    fields.emplace_back(element->selector, f.text);
  }
  if (fields.empty()) {
    OnActionDone(false, "Filled 0 fields");
    return;
  }
  ProbeElements(selectors,
                base::BindOnce(&WebAgent::OnFillFormProbed,
                               weak_factory_.GetWeakPtr(), std::move(fields)));
}

void WebAgent::OnFillFormProbed(FieldList fields,
                                std::vector<ElementProbe> probes) {
  if (cancelled_) return;
  FieldList keep;
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
  }
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
      [](base::WeakPtr<WebAgent> self, std::string note, base::Value v) {
        if (!self) return;
        if (v.is_string() && v.GetString() == "origin_changed") {
          self->OnActionDone(false, "Not filled: the page changed site");
          return;
        }
        int n = v.is_int() ? v.GetInt() : 0;
        self->OnActionDone(n > 0, "Filled " + base::NumberToString(n) +
                                      " fields" + note);
      },
      weak_factory_.GetWeakPtr(), std::move(note)));
}

void WebAgent::DoScroll() {
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
  content::RenderFrameHost* main =
      web_contents_ ? web_contents_->GetPrimaryMainFrame() : nullptr;
  if (!main) {
    OnActionDone(false, "No active tab");
    return;
  }
  tool_document_ = main->GetWeakDocumentPtr();
  tool_call_id_ = "c" + base::NumberToString(++next_tool_request_);
  // The script calls the tool only on action_origin_, only if the page
  // still lists it exactly as observed, with the arguments the browser
  // wrote (agent_tools::ReadArguments).
  EvalJS(agent_tools::BuildToolCallScript(tool_call_id_,
                                          action_origin_.Serialize(),
                                          action_tool_,
                                          pending_action_.tool_arguments),
         base::BindOnce(&WebAgent::OnToolCallStarted,
                        weak_factory_.GetWeakPtr()));
}

void WebAgent::OnToolCallStarted(base::Value result) {
  if (cancelled_) return;
  const agent_tools::CallOutcome started =
      agent_tools::ParseToolCallStart(result);
  if (started.state != agent_tools::CallState::kStarted) {
    OnActionDone(false,
                 std::string(agent_tools::CallFailureNote(started.state)));
    return;
  }
  PollIsolatedWorld(
      agent_tools::BuildToolCallPollScript(tool_call_id_), tool_document_,
      base::TimeTicks::Now() +
          base::Milliseconds(agent_tools::kCallTimeoutMs),
      base::BindOnce(&WebAgent::OnToolCallSettled,
                     weak_factory_.GetWeakPtr()));
}

void WebAgent::OnToolCallSettled(PollEnd end, base::Value result) {
  if (cancelled_) return;
  switch (end) {
    case PollEnd::kTimedOut:
      OnActionDone(false, std::string(agent_tools::CallFailureNote(
                              agent_tools::CallState::kTimedOut)));
      return;
    case PollEnd::kDocumentChanged:
      // The tool loaded another page (a <form> tool submits its form): that
      // page is its answer. Observe it once it has loaded; a page off the
      // run's sites asks the user first (OnPageObserved).
      tool_call_note_ = "The tool loaded another page";
      WaitForLoadThenObserve();
      return;
    case PollEnd::kSettled:
      break;
  }
  const agent_tools::CallOutcome outcome =
      agent_tools::ParseToolCallPoll(result);
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

void WebAgent::PollIsolatedWorld(std::string script,
                                 content::WeakDocumentPtr document,
                                 base::TimeTicks deadline,
                                 PollCallback done) {
  if (cancelled_) return;
  if (!IsCurrentDocument(document)) {
    std::move(done).Run(PollEnd::kDocumentChanged, base::Value());
    return;
  }
  const std::string js = script;
  EvalJS(js, base::BindOnce(&WebAgent::OnPolled, weak_factory_.GetWeakPtr(),
                            std::move(script), std::move(document), deadline,
                            std::move(done)));
}

void WebAgent::OnPolled(std::string script,
                        content::WeakDocumentPtr document,
                        base::TimeTicks deadline,
                        PollCallback done,
                        base::Value result) {
  if (cancelled_) return;
  const std::string* state =
      result.is_dict() ? result.GetDict().FindString("state") : nullptr;
  if (!state || *state != "pending") {
    std::move(done).Run(PollEnd::kSettled, std::move(result));
    return;
  }
  if (base::TimeTicks::Now() >= deadline) {
    std::move(done).Run(PollEnd::kTimedOut, base::Value());
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::PollIsolatedWorld, weak_factory_.GetWeakPtr(),
                     std::move(script), std::move(document), deadline,
                     std::move(done)),
      base::Milliseconds(agent_tools::kPollIntervalMs));
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

  // The page's own tools (WebMCP), from the same document and origin.
  agent_tools::ListFacts facts;
  facts.switched_on = agent_features::AgentSiteToolsEnabled();
  facts.origin_allowed = origin_allowed;
  facts.snapshot_is_current = snapshot_is_current && IsCurrentDocument(document);
  if (agent_tools::ShouldListTools(facts)) {
    ListSiteTools(page, std::move(document), std::move(snapshot_origin),
                  origin_allowed);
    return;
  }
  ContinueObservation(page, std::move(document), std::move(snapshot_origin),
                      origin_allowed);
}

void WebAgent::ListSiteTools(const StructuredPage& page,
                             content::WeakDocumentPtr document,
                             std::string observed_origin,
                             bool origin_allowed) {
  if (cancelled_) return;
  std::string request_id = "l" + base::NumberToString(++next_tool_request_);
  const std::string js = agent_tools::BuildToolListScript(request_id);
  EvalJS(js, base::BindOnce(&WebAgent::OnSiteToolListStarted,
                            weak_factory_.GetWeakPtr(), page,
                            std::move(document), std::move(observed_origin),
                            origin_allowed, std::move(request_id)));
}

void WebAgent::OnSiteToolListStarted(const StructuredPage& page,
                                     content::WeakDocumentPtr document,
                                     std::string observed_origin,
                                     bool origin_allowed,
                                     std::string request_id,
                                     base::Value result) {
  if (cancelled_) return;
  // 'unavailable' (no WebMCP on this page, the common case) or a failure:
  // no tools, and no polling.
  if (agent_tools::ParseToolList(result).state !=
      agent_tools::ListState::kPending) {
    ContinueObservation(page, std::move(document), std::move(observed_origin),
                        origin_allowed);
    return;
  }
  content::WeakDocumentPtr polled = document;
  PollIsolatedWorld(
      agent_tools::BuildToolListPollScript(request_id), std::move(polled),
      base::TimeTicks::Now() +
          base::Milliseconds(agent_tools::kListTimeoutMs),
      base::BindOnce(&WebAgent::OnSiteToolsListed, weak_factory_.GetWeakPtr(),
                     page, std::move(document), std::move(observed_origin),
                     origin_allowed));
}

void WebAgent::OnSiteToolsListed(const StructuredPage& page,
                                 content::WeakDocumentPtr document,
                                 std::string observed_origin,
                                 bool origin_allowed,
                                 PollEnd end,
                                 base::Value result) {
  if (cancelled_) return;
  if (end == PollEnd::kSettled && IsCurrentDocument(document)) {
    agent_tools::ToolList list = agent_tools::ParseToolList(result);
    // Only the observed document's tools, at the origin its snapshot read.
    if (list.state == agent_tools::ListState::kDone &&
        list.origin == observed_origin) {
      std::vector<agent_tools::SiteTool> tools;
      for (const agent_tools::ListedTool& listed : list.tools) {
        std::optional<agent_tools::SiteTool> tool =
            agent_tools::MakeSiteTool(listed);
        if (tool)
          tools.push_back(std::move(*tool));
      }
      agent_actions::AddSiteTools(std::move(tools), &observation_);
    }
  }
  DVLOG(1) << "[WebAgent] " << observation_.tools.size() << " site tool(s)";
  ContinueObservation(page, std::move(document), std::move(observed_origin),
                      origin_allowed);
}

void WebAgent::ContinueObservation(const StructuredPage& page,
                                   content::WeakDocumentPtr document,
                                   std::string observed_origin,
                                   bool origin_allowed) {
  if (cancelled_) return;

  std::string compact = page.ToCompactText();
  if (static_cast<int>(compact.size()) > kMaxPageChars)
    compact = compact.substr(0, kMaxPageChars) + "\n[… truncated]";

  // Everything here is page-controlled; BuildPrompt fences it. The element
  // and tool ids are the browser's; labels, tool names, descriptions and a
  // tool's answer are page text. The answer of the tool just called comes
  // first, so the step's history keeps it.
  current_page_state_ = tool_result_ +
                         "URL: " + page.page_url +
                         "\nTitle: " + page.page_title +
                         "\n\n" + compact +
                         "\nELEMENTS:\n" +
                         agent_actions::DescribeElements(observation_);
  tool_result_.clear();
  if (!observation_.tools.empty()) {
    current_page_state_ += "TOOLS (functions this page offers):\n" +
                           agent_tools::DescribeTools(observation_.tools);
  }

  // A new observation: the last one's screenshot no longer applies. Take
  // one of this one if the model sees images (AgentScreenshotter decides,
  // and answers at once when it won't capture).
  screenshot_.reset();
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
  if (!pending_step_.action.empty() && !is_bootstrap) {
    history_.push_back(pending_step_);
    if (step_cb_)
      step_cb_.Run(pending_step_);
  }

  iteration_++;
  pending_step_ = Step();
  pending_action_ = agent_actions::AgentAction();
  action_selector_.clear();

  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&WebAgent::IterateOnce, weak_factory_.GetWeakPtr()));
}

void WebAgent::Finish(bool success, const std::string& result) {
  if (cancelled_) return;
  cancelled_ = true;
  pending_confirm_.Reset();
  DVLOG(1) << "[WebAgent] Done success=" << success;
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

agent_prompt::PromptInput WebAgent::BuildPromptInput() const {
  agent_prompt::PromptInput input;
  input.goal = goal_;
  for (const Step& step : history_) {
    agent_prompt::HistoryStep h;
    h.number = step.number;
    h.action = step.action;
    h.target = step.target;
    h.value = step.value;
    h.success = step.success;
    h.note = step.note;
    h.observation = step.observation;
    input.history.push_back(std::move(h));
  }
  input.page_state = current_page_state_;
  input.max_steps = kMaxIterations;
  input.steps_remaining = kMaxIterations - iteration_;
  input.retry_note = retry_note_;
  input.with_screenshot = screenshot_.has_value();
  input.with_tools = !observation_.tools.empty();
  return input;
}

// ----------------------------------------------------------------
// Step description
// ----------------------------------------------------------------

WebAgent::Step WebAgent::DescribeAction(
    const agent_actions::AgentAction& action) const {
  Step step;
  step.action = std::string(agent_actions::ActionName(action.type));
  step.reason = action.reason;
  auto describe = [&](const std::string& ref) {
    const ObservedElement* e = observation_.Find(ref);
    return e ? agent_actions::DescribeElement(*e) : ref;
  };
  switch (action.type) {
    case ActionType::kNavigate:
      step.target = action.url;
      break;
    case ActionType::kSearch:
      step.target = action.query;
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
      step.target = tool ? "tool \"" + tool->name + "\"" : action.tool;
      step.value = action.tool_arguments_text;
      break;
    }
  }
  return step;
}

}  // namespace molt_ai
