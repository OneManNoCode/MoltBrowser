// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Page actions the AI chat proposes (molt_ai/agents/chat_actions.h,
// docs/design/AGENT_ACTIONS.md §4): after the text reply to a typed
// message that asks to act on the page, the browser observes the active
// tab, asks the on-device model for one action under a grammar, validates
// it and probes what it touches; the page shows the proposal, and it runs
// only through HandleRunChatProposal, on the document it was proposed for.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/chat_actions.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/tor/molt_net_requests_policy.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

using molt_ai::chat_handler_helpers::HasCallbackArgs;

namespace agent_actions = molt_ai::agent_actions;
namespace agent_safety = molt_ai::agent_safety;
namespace chat_actions = molt_ai::chat_actions;

// Defined here: only this file's steps carry it.
struct MoltAIChatHandler::ChatActionDecision {
  std::string callback_id;
  // The user's words (the typed message).
  std::string request;
  // The document observed, its origin, URL and title.
  content::WeakDocumentPtr document;
  url::Origin origin;
  std::string page_url;
  std::string page_title;
  agent_actions::Observation observation;
  // Unusable replies so far, and the note the next prompt carries.
  int bad_replies = 0;
  std::string retry_note;
  // chat_action_epoch_ when it started.
  int epoch = 0;
};

MoltAIChatHandler::ChatProposal::ChatProposal() = default;
MoltAIChatHandler::ChatProposal::~ChatProposal() = default;

namespace {

constexpr char kNoLongerAvailable[] =
    "This action is no longer available. Ask again.";
constexpr char kPageChanged[] =
    "The page changed since the action was proposed, so nothing was done. "
    "Ask again.";
constexpr char kTabChanged[] =
    "The tab changed since the action was proposed, so nothing was done. "
    "Ask again.";
constexpr char kNoWebPage[] =
    "Page actions work on a web page (http:// or https://) open in this "
    "window.";
constexpr char kNoLocalModel[] =
    "Page actions are chosen by the on-device model, and none is loaded. "
    "Pick one in the model menu, then ask again.";

// A positive integer argument (JS numbers may arrive as doubles).
std::optional<int> PositiveIntArg(const base::ListValue& args, size_t index) {
  if (index >= args.size()) {
    return std::nullopt;
  }
  const base::Value& v = args[index];
  if (v.is_int()) {
    return v.GetInt() > 0 ? std::optional<int>(v.GetInt()) : std::nullopt;
  }
  if (v.is_double()) {
    const double d = v.GetDouble();
    if (d >= 1 && d <= 2147483647.0 && static_cast<double>(
                                            static_cast<int>(d)) == d) {
      return static_cast<int>(d);
    }
  }
  return std::nullopt;
}

bool IsWebOrigin(const url::Origin& origin) {
  return !origin.opaque() && (origin.scheme() == url::kHttpScheme ||
                              origin.scheme() == url::kHttpsScheme);
}

base::DictValue ChatStatus(std::string_view status,
                           std::string_view message = {}) {
  base::DictValue d;
  d.Set("status", status);
  if (!message.empty()) {
    d.Set("message", message);
  }
  return d;
}

base::DictValue RunError(std::string_view error) {
  base::DictValue d;
  d.Set("success", false);
  d.Set("error", error);
  return d;
}

}  // namespace

void MoltAIChatHandler::ResolveChatAction(const std::string& callback_id,
                                          base::DictValue result) {
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(result)));
}

// ------------------------------------------------------------------
// proposeChatAction
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleProposeChatAction(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  const std::string callback_id = args[0].GetString();
  const std::optional<int> token = PositiveIntArg(args, 1);
  // Whatever happens next, an earlier proposal is stale now.
  chat_proposal_.reset();
  if (!token || *token != chat_action_token_) {
    // Not the reply of the last typed request (or already used).
    ResolveChatAction(callback_id, ChatStatus("none"));
    return;
  }
  std::string request = std::move(chat_action_request_);
  chat_action_token_ = 0;
  chat_action_prompt_callback_id_.clear();
  chat_action_request_.clear();

  // The page: the active tab of the chat's own window, never one across
  // MoltNet's boundary (BrowserForChatAction), and a web page.
  bool crossed_moltnet = false;
  BrowserWindowInterface* browser = BrowserForChatAction(&crossed_moltnet);
  if (crossed_moltnet) {
    ResolveChatAction(callback_id,
                      ChatStatus("unavailable",
                                 molt_ai::tor::kOtherWindowRefusedInMoltNet));
    return;
  }
  content::WebContents* tab =
      browser && browser->GetTabStripModel()
          ? browser->GetTabStripModel()->GetActiveWebContents()
          : nullptr;
  content::RenderFrameHost* frame =
      tab && tab != web_ui()->GetWebContents() ? tab->GetPrimaryMainFrame()
                                               : nullptr;
  if (!frame || !IsWebOrigin(frame->GetLastCommittedOrigin())) {
    ResolveChatAction(callback_id, ChatStatus("unavailable", kNoWebPage));
    return;
  }
  // Decided on-device only: under a grammar, and the page's element list
  // never leaves the device. A cloud chat proposes nothing unless an
  // on-device model is loaded (nothing is loaded for it here).
  if (GetOrCreateRuntime()->GetLoadedModelId().empty()) {
    ResolveChatAction(callback_id, ChatStatus("unavailable", kNoLocalModel));
    return;
  }

  auto decision = std::make_unique<ChatActionDecision>();
  decision->callback_id = callback_id;
  decision->request = std::move(request);
  decision->document = frame->GetWeakDocumentPtr();
  decision->origin = frame->GetLastCommittedOrigin();
  decision->page_url = tab->GetLastCommittedURL().spec();
  decision->page_title = base::UTF16ToUTF8(tab->GetTitle());
  decision->epoch = ++chat_action_epoch_;
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(agent_actions::BuildElementSnapshotScript()),
      base::BindOnce(&MoltAIChatHandler::OnChatActionSnapshot,
                     weak_ptr_factory_.GetWeakPtr(), std::move(decision)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void MoltAIChatHandler::OnChatActionSnapshot(
    std::unique_ptr<ChatActionDecision> decision,
    base::Value snapshot) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  if (decision->epoch != chat_action_epoch_) {
    ResolveChatAction(decision->callback_id, ChatStatus("cancelled"));
    return;
  }
  if (!decision->document.AsRenderFrameHostIfValid()) {
    ResolveChatAction(decision->callback_id,
                      ChatStatus("failed", kPageChanged));
    return;
  }
  // Elements only from the document observed: the snapshot must have run
  // in its origin (as the web agent requires).
  std::string snapshot_origin;
  decision->observation =
      agent_actions::ParseElementSnapshot(snapshot, &snapshot_origin);
  if (snapshot_origin != decision->origin.Serialize()) {
    decision->observation = agent_actions::Observation();
  }
  RunChatActionDecision(std::move(decision));
}

void MoltAIChatHandler::RunChatActionDecision(
    std::unique_ptr<ChatActionDecision> decision) {
  molt_ai::BrowserAIRuntime* runtime = GetOrCreateRuntime();
  // Stop (HandleCancelGeneration) cancels this request.
  const uint64_t request_id = runtime->NewRequestId();
  active_request_id_ = request_id;

  const chat_actions::DecisionPrompt prompt = chat_actions::BuildDecisionPrompt(
      decision->request, decision->page_url, decision->page_title,
      decision->observation, decision->retry_note);
  molt_ai::ChatPrompt chat;
  chat.system_prompt = prompt.system_prompt;
  chat.context_preamble = "\n\n";
  chat.context = prompt.context;
  chat.user_message = prompt.user_message;

  molt_ai::PromptOptions opts;
  opts.max_tokens = agent_actions::kMaxReplyTokens;
  opts.temperature = 0.2f;
  opts.reasoning = molt_ai::ReasoningMode::kOff;
  opts.request_id = request_id;
  // One action on the elements the model is shown; checked again in
  // OnChatActionDecision (the reply is untrusted).
  opts.json_schema = chat_actions::BuildDecisionSchema(decision->observation);

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::TaskPriority::USER_BLOCKING, base::MayBlock(),
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](molt_ai::BrowserAIRuntime* rt, molt_ai::ChatPrompt chat,
             molt_ai::PromptOptions opts) -> molt_ai::GenerationResult {
            base::ScopedClosureRunner release_request(
                base::BindOnce(&molt_ai::BrowserAIRuntime::FinishRequest,
                               base::Unretained(rt), opts.request_id));
            return rt->StreamChat(chat, molt_ai::TokenCallback(), opts);
          },
          base::Unretained(runtime), std::move(chat), std::move(opts)),
      base::BindOnce(&MoltAIChatHandler::OnChatActionDecision,
                     weak_ptr_factory_.GetWeakPtr(), std::move(decision),
                     request_id));
}

void MoltAIChatHandler::OnChatActionDecision(
    std::unique_ptr<ChatActionDecision> decision,
    uint64_t request_id,
    molt_ai::GenerationResult result) {
  if (active_request_id_ == request_id) {
    active_request_id_ = 0;
  }
  if (!IsJavascriptAllowed()) {
    return;
  }
  if (decision->epoch != chat_action_epoch_) {
    ResolveChatAction(decision->callback_id, ChatStatus("cancelled"));
    return;
  }

  agent_actions::ModelReply reply;
  reply.success = result.success;
  reply.cancelled = result.was_cancelled;
  reply.constraint_unsupported = result.constraint_unsupported;
  reply.invalid_output = result.invalid_output;
  reply.truncated = result.truncated;
  reply.text = std::move(result.text);
  reply.error_message = std::move(result.error_message);
  agent_actions::ReplyOutcome outcome = chat_actions::InterpretDecision(
      reply, decision->observation, decision->bad_replies);
  // The reply can carry typed text: log its shape only.
  DVLOG(1) << "[MoltAI] chat action reply (" << reply.text.size()
           << " chars) decision=" << static_cast<int>(outcome.decision);

  switch (outcome.decision) {
    case agent_actions::ReplyDecision::kStop:
      ResolveChatAction(
          decision->callback_id,
          ChatStatus(reply.cancelled ? "cancelled" : "failed",
                     reply.cancelled ? std::string() : outcome.message));
      return;
    case agent_actions::ReplyDecision::kRetry:
      if (!decision->document.AsRenderFrameHostIfValid()) {
        ResolveChatAction(decision->callback_id,
                          ChatStatus("failed", kPageChanged));
        return;
      }
      ++decision->bad_replies;
      decision->retry_note = std::move(outcome.message);
      RunChatActionDecision(std::move(decision));
      return;
    case agent_actions::ReplyDecision::kAct:
      break;
  }
  if (outcome.action.type == agent_actions::ActionType::kDone) {
    // No page action for this request.
    ResolveChatAction(decision->callback_id, ChatStatus("none"));
    return;
  }
  content::RenderFrameHost* frame = decision->document.AsRenderFrameHostIfValid();
  if (!frame) {
    ResolveChatAction(decision->callback_id,
                      ChatStatus("failed", kPageChanged));
    return;
  }

  auto proposal = std::make_unique<ChatProposal>();
  proposal->id = ++next_chat_proposal_id_;
  proposal->action = std::move(outcome.action);
  proposal->observation = std::move(decision->observation);
  proposal->scope =
      chat_actions::Scope::ForRequest(decision->origin, decision->request);
  proposal->document = decision->document;
  proposal->origin = decision->origin;
  const std::vector<std::string> selectors =
      chat_actions::TargetSelectors(proposal->action, proposal->observation);
  ProbeChatActionTargets(
      frame, selectors,
      base::BindOnce(&MoltAIChatHandler::OnChatProposalProbed,
                     weak_ptr_factory_.GetWeakPtr(), decision->callback_id,
                     std::move(proposal), decision->epoch));
}

void MoltAIChatHandler::ProbeChatActionTargets(
    content::RenderFrameHost* frame,
    const std::vector<std::string>& selectors,
    base::OnceCallback<void(std::vector<agent_safety::ElementProbe>)> done) {
  if (selectors.empty()) {
    std::move(done).Run({});
    return;
  }
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(agent_safety::BuildElementProbeScript(selectors)),
      base::BindOnce(
          [](size_t n,
             base::OnceCallback<void(std::vector<agent_safety::ElementProbe>)>
                 done,
             base::Value v) {
            std::move(done).Run(agent_safety::ParseElementProbes(v, n));
          },
          selectors.size(), std::move(done)),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void MoltAIChatHandler::OnChatProposalProbed(
    std::string callback_id,
    std::unique_ptr<ChatProposal> proposal,
    int epoch,
    std::vector<agent_safety::ElementProbe> probes) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  if (epoch != chat_action_epoch_) {
    ResolveChatAction(callback_id, ChatStatus("cancelled"));
    return;
  }
  if (!proposal->document.AsRenderFrameHostIfValid()) {
    ResolveChatAction(callback_id, ChatStatus("failed", kPageChanged));
    return;
  }
  const chat_actions::Check check = chat_actions::CheckProposal(
      proposal->action, proposal->observation, proposal->scope,
      proposal->origin, probes);
  if (check.verdict == chat_actions::Verdict::kRefuse) {
    ResolveChatAction(callback_id, ChatStatus("failed", check.message));
    return;
  }
  proposal->shown_reason =
      check.verdict == chat_actions::Verdict::kAsk ? check.message
                                                   : std::string();
  base::DictValue result = ChatStatus("proposal");
  result.Set("id", proposal->id);
  result.Set("verb", agent_actions::ActionName(proposal->action.type));
  result.Set("summary", chat_actions::DescribeProposal(proposal->action,
                                                       proposal->observation));
  result.Set("reason", proposal->shown_reason);
  chat_proposal_ = std::move(proposal);
  ResolveChatAction(callback_id, std::move(result));
}

// ------------------------------------------------------------------
// runChatProposal
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleRunChatProposal(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  const std::string callback_id = args[0].GetString();
  const std::optional<int> id = PositiveIntArg(args, 1);
  // Only an explicit true is the user's Allow.
  const bool confirmed =
      args.size() > 2u && args[2].is_bool() && args[2].GetBool();
  if (!chat_proposal_ || !id || *id != chat_proposal_->id) {
    ResolveChatAction(callback_id, RunError(kNoLongerAvailable));
    return;
  }
  std::string error;
  content::RenderFrameHost* frame = ChatProposalFrame(*chat_proposal_, &error);
  if (!frame) {
    chat_proposal_.reset();
    ResolveChatAction(callback_id, RunError(error));
    return;
  }
  // Probe again: the page may have changed what the action touches since
  // the proposal (or since the user read it).
  ProbeChatActionTargets(
      frame,
      chat_actions::TargetSelectors(chat_proposal_->action,
                                    chat_proposal_->observation),
      base::BindOnce(&MoltAIChatHandler::OnChatProposalRechecked,
                     weak_ptr_factory_.GetWeakPtr(), callback_id, *id,
                     confirmed));
}

void MoltAIChatHandler::OnChatProposalRechecked(
    std::string callback_id,
    int id,
    bool confirmed,
    std::vector<agent_safety::ElementProbe> probes) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  if (!chat_proposal_ || chat_proposal_->id != id) {
    ResolveChatAction(callback_id, RunError(kNoLongerAvailable));
    return;
  }
  std::string error;
  content::RenderFrameHost* frame = ChatProposalFrame(*chat_proposal_, &error);
  if (!frame) {
    chat_proposal_.reset();
    ResolveChatAction(callback_id, RunError(error));
    return;
  }
  const chat_actions::Check check = chat_actions::CheckProposal(
      chat_proposal_->action, chat_proposal_->observation,
      chat_proposal_->scope, chat_proposal_->origin, probes);
  if (check.verdict == chat_actions::Verdict::kRefuse) {
    chat_proposal_.reset();
    ResolveChatAction(callback_id, RunError(check.message));
    return;
  }
  if (!chat_actions::MayRun(check, confirmed, chat_proposal_->shown_reason)) {
    // Needs the user's OK for this reason first (Auto mode too).
    chat_proposal_->shown_reason = check.message;
    base::DictValue result;
    result.Set("success", false);
    result.Set("needs_confirmation", true);
    result.Set("reason", check.message);
    ResolveChatAction(callback_id, std::move(result));
    return;
  }
  // A proposal runs once.
  std::unique_ptr<ChatProposal> proposal = std::move(chat_proposal_);
  RunChatProposalNow(callback_id, *proposal, frame);
}

void MoltAIChatHandler::RunChatProposalNow(const std::string& callback_id,
                                           const ChatProposal& proposal,
                                           content::RenderFrameHost* frame) {
  const agent_actions::AgentAction& action = proposal.action;
  const std::string origin = proposal.origin.Serialize();
  auto selector_of = [&](const std::string& ref) {
    const agent_actions::ObservedElement* e = proposal.observation.Find(ref);
    return e ? e->selector : std::string();
  };
  std::string script;
  switch (action.type) {
    case agent_actions::ActionType::kNavigate:
    case agent_actions::ActionType::kSearch: {
      // CheckProposal allowed only http(s), and a site outside the
      // proposal's scope only with the user's OK.
      const GURL url(chat_actions::NavigationURL(action));
      if (!agent_safety::IsAllowedNavigationURL(url)) {
        ResolveChatAction(callback_id,
                          RunError("Only http:// and https:// pages can be "
                                   "opened."));
        return;
      }
      content::WebContents::FromRenderFrameHost(frame)
          ->GetController()
          .LoadURL(url, content::Referrer(),
                   ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
      base::DictValue result;
      result.Set("success", true);
      result.Set("message",
                 "Opening " + url::Origin::Create(url).Serialize());
      ResolveChatAction(callback_id, std::move(result));
      return;
    }
    case agent_actions::ActionType::kClick:
      script = agent_actions::BuildClickScript(origin, selector_of(action.ref));
      break;
    case agent_actions::ActionType::kType:
      script = agent_actions::BuildTypeScript(origin, selector_of(action.ref),
                                              action.text);
      break;
    case agent_actions::ActionType::kFillForm: {
      std::vector<std::pair<std::string, std::string>> fields;
      for (const agent_actions::FormFieldValue& f : action.fields) {
        fields.emplace_back(selector_of(f.ref), f.text);
      }
      script = agent_actions::BuildFillFormScript(origin, fields);
      break;
    }
    case agent_actions::ActionType::kScroll:
      script = agent_actions::BuildScrollScript(action.direction);
      break;
    case agent_actions::ActionType::kObserve:
    case agent_actions::ActionType::kDone:
    case agent_actions::ActionType::kError:
      ResolveChatAction(callback_id, RunError("Not a page action."));
      return;
  }
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(script),
      base::BindOnce(
          [](base::WeakPtr<MoltAIChatHandler> self, std::string cb_id,
             agent_actions::AgentAction action, base::Value v) {
            if (!self || !self->IsJavascriptAllowed()) {
              return;
            }
            const chat_actions::ScriptOutcome outcome =
                chat_actions::InterpretScriptResult(action, v);
            base::DictValue result;
            result.Set("success", outcome.success);
            result.Set(outcome.success ? "message" : "error", outcome.message);
            self->ResolveChatAction(cb_id, std::move(result));
          },
          weak_ptr_factory_.GetWeakPtr(), callback_id, action),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

content::RenderFrameHost* MoltAIChatHandler::ChatProposalFrame(
    const ChatProposal& proposal,
    std::string* error) {
  // The same document, still the primary main frame, with the same origin.
  content::RenderFrameHost* frame =
      proposal.document.AsRenderFrameHostIfValid();
  if (!frame || !frame->IsActive() || !frame->IsInPrimaryMainFrame() ||
      frame->GetLastCommittedOrigin() != proposal.origin) {
    *error = kPageChanged;
    return nullptr;
  }
  // ... and still the active tab of the chat's window, on the chat's side
  // of MoltNet's boundary.
  bool crossed_moltnet = false;
  BrowserWindowInterface* browser = BrowserForChatAction(&crossed_moltnet);
  if (crossed_moltnet) {
    *error = molt_ai::tor::kOtherWindowRefusedInMoltNet;
    return nullptr;
  }
  content::WebContents* active =
      browser && browser->GetTabStripModel()
          ? browser->GetTabStripModel()->GetActiveWebContents()
          : nullptr;
  if (!active || content::WebContents::FromRenderFrameHost(frame) != active) {
    *error = kTabChanged;
    return nullptr;
  }
  return frame;
}
