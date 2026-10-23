// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Save as automation": a successful web agent run saved as a draft
// Agent-mode workflow (docs/design/AGENT_ACTIONS.md §2.9). What a draft
// keeps, withholds and refuses is automation/agent_run_draft.h; this is the
// chat's side of it:
//   * the run is recorded as WebAgent reports its steps (the browser's own
//     record of what each acted on, WebAgent::Step);
//   * when it ends with DONE, its result says whether it can be saved
//     (`save_run`) or why not (`save_note`);
//   * saveAgentRun saves it, only on the user's click in the agent bar,
//     only the last run, once, and never in MoltNet (the regular profile's
//     studio and scheduler would run it outside Tor);
//   * openAgentStudio opens Agent mode, where the draft is reviewed and its
//     sites confirmed before it can run.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"

#include <cmath>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/common/molt_blocking_scope.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_id.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"

namespace {

using molt_ai::automation::AutomationSecurity;
using molt_ai::automation::AutomationStorage;
using molt_ai::automation::agent_draft::BuildDraft;
using molt_ai::automation::agent_draft::Draft;
using molt_ai::automation::agent_draft::Refusal;
using molt_ai::automation::agent_draft::RunElement;
using molt_ai::automation::agent_draft::RunStep;
using molt_ai::chat_handler_helpers::HasCallbackArgs;

constexpr char kRunGone[] =
    "This run can no longer be saved. Run the agent again to save it.";

int64_t NowUnix() {
  return static_cast<int64_t>(std::time(nullptr));
}

// The browser's record of a step, in the converter's terms. Nothing the
// model wrote about it (its reason) and no page text but the element
// labels.
RunStep ToRunStep(const molt_ai::WebAgent::Step& step) {
  RunStep out;
  out.action = step.action;
  out.success = step.success;
  out.url = step.url;
  if (step.action == "SEARCH") {
    out.query = step.target;
  } else if (step.action == "SCROLL") {
    out.direction = step.target;
  }
  out.origin = step.origin;
  for (const molt_ai::WebAgent::Step::Element& e : step.elements) {
    RunElement element;
    element.selector = e.selector;
    element.kind = e.kind;
    element.input_type = e.input_type;
    element.label = e.label;
    element.field_kind = e.field_kind;
    element.text = e.text;
    out.elements.push_back(std::move(element));
  }
  return out;
}

}  // namespace

int MoltAIChatHandler::BeginAgentRunRecord(const std::string& goal,
                                           content::WebContents* target) {
  agent_run_ = molt_ai::automation::agent_draft::Run();
  agent_run_.goal = goal;
  if (target) {
    agent_run_.start_url = target->GetLastCommittedURL().spec();
  }
  const Profile* chat_profile = Profile::FromBrowserContext(
      web_ui()->GetWebContents()->GetBrowserContext());
  const Profile* tab_profile =
      target ? Profile::FromBrowserContext(target->GetBrowserContext())
             : nullptr;
  agent_run_.in_moltnet = molt_ai::tor::IsMoltNetProfile(chat_profile) ||
                          molt_ai::tor::IsMoltNetProfile(tab_profile);
  return ++agent_run_id_;
}

void MoltAIChatHandler::RecordAgentRunStep(
    int run_id,
    const molt_ai::WebAgent::Step& step) {
  if (run_id != agent_run_id_ ||
      agent_run_.steps.size() >
          molt_ai::automation::agent_draft::kMaxDraftSteps) {
    return;
  }
  agent_run_.steps.push_back(ToRunStep(step));
}

void MoltAIChatHandler::FinishAgentRunRecord(int run_id,
                                             bool success,
                                             base::DictValue* result) {
  if (run_id != agent_run_id_) {
    return;
  }
  if (!success) {
    ForgetAgentRun();
    return;
  }
  agent_run_.succeeded = true;
  const Draft draft =
      BuildDraft(agent_run_, SavedWorkflowsAllowed(), NowUnix());
  if (draft.refusal == Refusal::kNone) {
    result->Set("save_run", run_id);
    return;
  }
  // Why not, in the browser's fixed words; nothing of the run is kept.
  if (!draft.message.empty()) {
    result->Set("save_note", draft.message);
  }
  ForgetAgentRun();
}

void MoltAIChatHandler::ForgetAgentRun() {
  agent_run_ = molt_ai::automation::agent_draft::Run();
  ++agent_run_id_;
}

// ------------------------------------------------------------------
// HandleSaveAgentRun — "Save as automation" in the agent bar.
// Args: [callback_id, run_id]
// Returns: {success: true, id, name, message} | {success: false, error}
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleSaveAgentRun(const base::ListValue& args) {
  AllowJavascript();
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  const std::string callback_id = args[0].GetString();
  base::DictValue out;
  out.Set("success", false);
  auto reply = [&](std::string_view error) {
    if (!error.empty()) {
      out.Set("error", error);
    }
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(out)));
  };

  // Only the user's own click on the button saves a run, never the page
  // script on its own (the click's activation reaches the browser first:
  // the WebUI message pipe is frame-associated).
  content::RenderFrameHost* rfh = web_ui()->GetRenderFrameHost();
  if (!rfh || !rfh->HasTransientUserActivation()) {
    reply("Click \xE2\x80\x9CSave as automation\xE2\x80\x9D to save the run.");
    return;
  }
  // The last run only, once, and only one that ended with DONE. (A JS
  // number may arrive as a double.)
  std::optional<int> run_id;
  if (args[1].is_int()) {
    run_id = args[1].GetInt();
  } else if (args[1].is_double()) {
    const double d = args[1].GetDouble();
    if (d >= 1 && d <= 1e9 && d == std::floor(d)) {
      run_id = static_cast<int>(d);
    }
  }
  if (!run_id || *run_id != agent_run_id_ || !agent_run_.succeeded) {
    reply(kRunGone);
    return;
  }
  // MoltNet: re-checked here, whatever the page was told when the run
  // ended (BuildDraft refuses too).
  const bool allowed = SavedWorkflowsAllowed();
  Draft draft = BuildDraft(agent_run_, allowed, NowUnix());
  if (!draft.script) {
    ForgetAgentRun();
    reply(draft.message.empty() ? std::string_view(kRunGone)
                                : std::string_view(draft.message));
    return;
  }

  molt_ai::automation::Script& script = *draft.script;
  bool saved = false;
  {
    ScopedAllowBlockingForMolt allow_blocking;
    AutomationStorage storage;
    if (storage.EnsureDirectory()) {
      std::string id = "agent_" + base::NumberToString(NowUnix());
      while (storage.Load(id)) {
        id += "_";
      }
      script.id = id;
      // A new workflow: CASUAL, whatever it says (it says CASUAL).
      AutomationSecurity::ClampTrustForSave(script, /*existing=*/nullptr);
      saved = storage.Save(script);
      if (saved) {
        storage.AppendAudit(script.id, "draft_from_agent_run",
                            "casual, sites unconfirmed");
      }
    }
  }
  if (!saved) {
    reply("Couldn\xE2\x80\x99t save the workflow.");
    return;
  }
  // Saved once: the run's record (typed text included) is dropped.
  ForgetAgentRun();
  out.Set("success", true);
  out.Set("id", script.id);
  out.Set("name", script.name);
  out.Set("message", draft.message);
  reply(std::string_view());
}

// ------------------------------------------------------------------
// HandleOpenAgentStudio — open Agent mode in the chat's own window. The
// studio shares the content side panel with the chat, so in the side panel
// this replaces the chat (and this handler): fire-and-forget.
// Args: []
// ------------------------------------------------------------------
void MoltAIChatHandler::HandleOpenAgentStudio(const base::ListValue& args) {
  // Agent mode is the regular profile's; a MoltNet chat doesn't open it.
  if (!SavedWorkflowsAllowed()) {
    return;
  }
  // The chat's own window (a tab's, or the one hosting its side panel),
  // never another one.
  bool crossed_moltnet = false;
  BrowserWindowInterface* browser = BrowserForChatAction(&crossed_moltnet);
  if (!browser) {
    return;
  }
  if (SidePanelUI* side_panel_ui = browser->GetFeatures().side_panel_ui()) {
    side_panel_ui->Show(SidePanelEntryId::kMoltAgent);
  }
}
