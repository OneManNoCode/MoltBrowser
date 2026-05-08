// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Manager UI for MoltBrowser's web automation engine.
// Lists saved scripts, lets the user Run / Edit / Pause / Delete each one,
// shows the audit log, and (Sprint 2+) hosts the recording overlay.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_automation_ui.h"

#include <ctime>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_background_browser.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace {

using molt_ai::automation::AutomationScheduler;
using molt_ai::automation::AutomationSecurity;
using molt_ai::automation::AutomationStorage;
using molt_ai::automation::Script;
using molt_ai::automation::Step;
using molt_ai::automation::StepType;
using molt_ai::automation::TrustLevel;
using molt_ai::automation::TrustLevelToString;

// ---- Message Handler ----
class MoltAIAutomationHandler : public content::WebUIMessageHandler {
 public:
  MoltAIAutomationHandler() = default;
  ~MoltAIAutomationHandler() override = default;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "listScripts",
        base::BindRepeating(&MoltAIAutomationHandler::HandleListScripts,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "getScript",
        base::BindRepeating(&MoltAIAutomationHandler::HandleGetScript,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "saveScript",
        base::BindRepeating(&MoltAIAutomationHandler::HandleSaveScript,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "deleteScript",
        base::BindRepeating(&MoltAIAutomationHandler::HandleDeleteScript,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "runScript",
        base::BindRepeating(&MoltAIAutomationHandler::HandleRunScript,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "auditTail",
        base::BindRepeating(&MoltAIAutomationHandler::HandleAuditTail,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "createSampleScript",
        base::BindRepeating(
            &MoltAIAutomationHandler::HandleCreateSampleScript,
            base::Unretained(this)));
    // Day 4: manual creation + import/export.
    web_ui()->RegisterMessageCallback(
        "createBlankScript",
        base::BindRepeating(
            &MoltAIAutomationHandler::HandleCreateBlankScript,
            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "importScript",
        base::BindRepeating(&MoltAIAutomationHandler::HandleImportScript,
                            base::Unretained(this)));
    // Day 5: trust promotion.
    web_ui()->RegisterMessageCallback(
        "promoteTrust",
        base::BindRepeating(&MoltAIAutomationHandler::HandlePromoteTrust,
                            base::Unretained(this)));
  }

 private:
  AutomationStorage storage_;
  AutomationSecurity security_;

  // Convert a Script to a UI-friendly dict (same shape its JSON has, but
  // also pre-renders some computed fields like success_rate).
  base::DictValue ScriptToDict(const Script& s) {
    base::DictValue d = s.ToJSON();
    int rate = (s.stats.runs == 0)
                   ? -1
                   : (100 * s.stats.successes / s.stats.runs);
    d.Set("success_rate", rate);

    // Day 3: precompute next-fire time so the schedule UI doesn't need
    // a second IPC round-trip per card.
    base::Time next = AutomationScheduler::NextFireTime(s.trigger,
                                                          base::Time::Now());
    if (!next.is_null()) {
      d.Set("next_fire_unix",
            static_cast<int>(next.InSecondsFSinceUnixEpoch()));
    }

    // Day 5: trust promotion suggestion.
    TrustLevel suggested =
        security_.ConsiderPromotion(s.security.trust, s.stats);
    if (suggested != s.security.trust) {
      d.Set("suggested_trust", TrustLevelToString(suggested));
    }
    return d;
  }

  void HandleListScripts(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    base::ListValue list;
    for (const auto& s : storage_.ListAll())
      list.Append(ScriptToDict(s));

    base::DictValue result;
    result.Set("scripts", std::move(list));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  void HandleGetScript(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    const std::string id =
        args[1].is_string() ? args[1].GetString() : "";

    auto script = storage_.Load(id);
    base::DictValue result;
    if (script) {
      result.Set("script", ScriptToDict(*script));
      result.Set("found", true);
    } else {
      result.Set("found", false);
    }
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  void HandleSaveScript(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();

    base::DictValue result;
    if (!args[1].is_dict()) {
      result.Set("success", false);
      result.Set("error", "expected script dict");
    } else {
      auto script = Script::FromJSON(args[1].GetDict());
      if (!script) {
        result.Set("success", false);
        result.Set("error", "invalid script");
      } else {
        result.Set("success", storage_.Save(*script));
      }
    }
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  void HandleDeleteScript(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    const std::string id =
        args[1].is_string() ? args[1].GetString() : "";
    base::DictValue result;
    result.Set("success", storage_.Delete(id));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // Run a saved script in a background browser window using the user's
  // main profile (so cookies persist). Optional second arg is a
  // {variable:value} dict that overrides default_variables for this run.
  void HandleRunScript(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    const std::string id =
        args[1].is_string() ? args[1].GetString() : "";

    auto script = storage_.Load(id);
    base::DictValue result;
    if (!script) {
      result.Set("success", false);
      result.Set("error", "script not found");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }

    // Apply any one-shot variable overrides supplied with the run call.
    if (args.size() > 2 && args[2].is_dict()) {
      for (const auto kv : args[2].GetDict()) {
        if (kv.second.is_string())
          script->default_variables[kv.first] = kv.second.GetString();
      }
    }

    storage_.AppendAudit(id, "manual_run_requested", "from_ui");

    Profile* profile = Profile::FromBrowserContext(
        web_ui()->GetWebContents()->GetBrowserContext());
    bool headless =
        script->security.trust >= molt_ai::automation::TrustLevel::TRUSTED;
    molt_ai::automation::RunScriptInBackgroundBrowser(profile, *script,
                                                       headless);

    result.Set("success", true);
    result.Set("steps", static_cast<int>(script->steps.size()));
    result.Set("name", script->name);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  void HandleAuditTail(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();
    int max_lines = 200;
    if (args.size() > 1 && args[1].is_int())
      max_lines = args[1].GetInt();

    base::ListValue lines;
    for (const auto& l : storage_.ReadAuditTail(max_lines))
      lines.Append(l);

    base::DictValue result;
    result.Set("lines", std::move(lines));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // Convenience: drop a sample script onto disk so the user can see the UI
  // populated and try Run on a freshly installed browser.
  void HandleCreateSampleScript(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    Script s;
    s.id = "sample-flight-watch";
    s.name = "Sample: flight price watcher";
    s.created_at_unix = static_cast<int64_t>(std::time(nullptr));
    s.security.domain_whitelist = {"moltsearch.ai"};
    s.security.require_approval_for = {"form_submit", "payment", "login"};

    Step nav;
    nav.type = StepType::NAVIGATE;
    nav.target = "https://moltsearch.ai";
    nav.description = "Open MoltSearch home";
    s.steps.push_back(std::move(nav));

    Step wait;
    wait.type = StepType::WAIT_FOR;
    wait.target = "input";
    wait.timeout_ms = 5000;
    wait.description = "Wait for the search box";
    s.steps.push_back(std::move(wait));

    Step type;
    type.type = StepType::TYPE;
    type.target = "input";
    type.value = "flights to bali";
    type.description = "Enter search query";
    s.steps.push_back(std::move(type));

    Step notify;
    notify.type = StepType::NOTIFY;
    notify.value = "Sample run finished";
    notify.description = "Notify user";
    s.steps.push_back(std::move(notify));

    bool ok = storage_.Save(s);
    base::DictValue result;
    result.Set("success", ok);
    result.Set("script_id", s.id);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // Day 4: create a brand-new empty script the user can edit step-by-step.
  void HandleCreateBlankScript(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 1u);
    const std::string callback_id = args[0].GetString();

    Script s;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    s.id = "new-" + base::NumberToString(now);
    s.name = "New automation";
    s.created_at_unix = now;
    s.security.trust = TrustLevel::CASUAL;
    s.security.require_approval_for = {"form_submit", "payment", "login"};

    bool ok = storage_.Save(s);
    base::DictValue result;
    result.Set("success", ok);
    result.Set("script_id", s.id);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // Day 4: import a script from JSON (paste-or-upload from the UI).
  // Generates a fresh id if the imported one already exists, so the
  // user doesn't accidentally overwrite a script with the same name.
  void HandleImportScript(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 2u);
    const std::string callback_id = args[0].GetString();
    base::DictValue result;

    if (!args[1].is_dict()) {
      result.Set("success", false);
      result.Set("error", "expected script JSON dict");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    auto script = Script::FromJSON(args[1].GetDict());
    if (!script) {
      result.Set("success", false);
      result.Set("error", "invalid script JSON");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    // Don't overwrite existing scripts on import — assign a new id if
    // the original is taken. Reset stats so imported scripts start
    // CASUAL-trust and 0 runs (the importer hasn't earned trust yet).
    if (storage_.Load(script->id)) {
      script->id = script->id + "-imported-" +
          base::NumberToString(static_cast<int64_t>(std::time(nullptr)));
    }
    script->stats = molt_ai::automation::Stats();
    script->security.trust = TrustLevel::CASUAL;
    bool ok = storage_.Save(*script);
    if (ok) storage_.AppendAudit(script->id, "imported", "from_ui");
    result.Set("success", ok);
    result.Set("script_id", script->id);
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }

  // Day 5: explicit trust-level change. The UI sends the new level as a
  // string ("casual"/"approved"/"trusted"/"admin"). We accept any value
  // here since the user is the source of truth on their own machine; the
  // security policy just gates what the runner is willing to do.
  void HandlePromoteTrust(const base::ListValue& args) {
    AllowJavascript();
    CHECK_GE(args.size(), 3u);
    const std::string callback_id = args[0].GetString();
    const std::string id =
        args[1].is_string() ? args[1].GetString() : "";
    const std::string trust_str =
        args[2].is_string() ? args[2].GetString() : "casual";

    auto script = storage_.Load(id);
    base::DictValue result;
    if (!script) {
      result.Set("success", false);
      result.Set("error", "script not found");
      ResolveJavascriptCallback(base::Value(callback_id),
                                base::Value(std::move(result)));
      return;
    }
    TrustLevel before = script->security.trust;
    script->security.trust =
        molt_ai::automation::TrustLevelFromString(trust_str);
    bool ok = storage_.Save(*script);
    if (ok) {
      storage_.AppendAudit(id, "trust_changed",
                            TrustLevelToString(before) + " -> " + trust_str);
    }
    result.Set("success", ok);
    result.Set("trust", TrustLevelToString(script->security.trust));
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(std::move(result)));
  }
};

// ---- Data source ----
class MoltAIAutomationDataSource : public content::URLDataSource {
 public:
  MoltAIAutomationDataSource() = default;
  ~MoltAIAutomationDataSource() override = default;

  std::string GetSource() override { return "molt-ai-automation"; }

  std::string GetMimeType(const GURL& url) override { return "text/html"; }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    std::string html = R"HTML(
<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>MoltBrowser Automations</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  background:#0a0a0f;color:#e0e0e8;min-height:100vh;}
.header{background:linear-gradient(135deg,#1a1a2e,#16213e);
  padding:18px 28px;display:flex;align-items:center;gap:18px;
  border-bottom:1px solid rgba(255,255,255,0.08);}
.header h1{font-size:22px;font-weight:600;}
.header h1 span{color:#ff4444;font-weight:700;}
.badge{background:#ff4444;color:#fff;font-size:10px;padding:2px 8px;
  border-radius:10px;font-weight:600;text-transform:uppercase;}
.bar{flex:1}
.btn{background:#ff4444;border:none;color:#fff;padding:8px 16px;
  border-radius:8px;font-weight:600;cursor:pointer;font-size:13px;}
.btn:hover{background:#ff5555;}
.btn:disabled{opacity:0.5;cursor:not-allowed;}
.btn.ghost{background:transparent;border:1px solid #2a2a3a;color:#ccc;}
.btn.ghost:hover{border-color:#ff4444;color:#fff;}
.btn.green{background:#16a34a;}
.btn.green:hover{background:#22c55e;}
.btn.small{padding:5px 10px;font-size:11px;border-radius:6px;}
.container{max-width:1080px;margin:0 auto;padding:32px 28px 64px;}
.intro{color:#888;margin-bottom:24px;line-height:1.5;}
.intro b{color:#ddd;}
.toolbar-row{display:flex;gap:10px;margin-bottom:24px;align-items:center;}
.toolbar-row .grow{flex:1;}
.search-input{background:#0d0d14;border:1px solid #2a2a3a;color:#fff;
  padding:8px 14px;border-radius:8px;font-size:13px;width:240px;}
.search-input:focus{outline:none;border-color:#ff4444;}
.script-list{display:flex;flex-direction:column;gap:12px;}
.empty{padding:48px;text-align:center;color:#555;background:#0d0d14;
  border:1px dashed #1a1a2a;border-radius:12px;}
.script-card{background:#0d0d14;border:1px solid #1a1a2a;border-radius:12px;
  overflow:hidden;transition:border-color 0.2s,box-shadow 0.2s;}
.script-card.highlight{border-color:#ff4444;
  box-shadow:0 0 0 3px rgba(255,68,68,0.2);}
.script-summary{padding:18px 22px;display:flex;align-items:center;gap:16px;
  cursor:pointer;user-select:none;}
.script-summary:hover{background:rgba(255,255,255,0.02);}
.script-summary .info{flex:1;min-width:0;}
.script-summary .name{font-size:15px;font-weight:600;margin-bottom:4px;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.script-summary .meta{font-size:12px;color:#888;}
.script-summary .actions{display:flex;gap:8px;flex-shrink:0;}
.script-summary .chev{color:#555;font-size:14px;transition:transform 0.2s;
  margin-left:4px;}
.script-card.open .chev{transform:rotate(90deg);}
.script-detail{display:none;padding:0 22px 18px;border-top:1px solid #1a1a2a;
  background:#08080d;}
.script-card.open .script-detail{display:block;}
.detail-section{margin-top:16px;}
.detail-section h4{font-size:11px;text-transform:uppercase;
  letter-spacing:1px;color:#888;margin-bottom:8px;font-weight:600;}
.var-grid{display:grid;grid-template-columns:140px 1fr;gap:8px 12px;
  align-items:center;}
.var-grid label{font-size:12px;color:#aaa;font-family:monospace;}
.var-grid input{background:#0d0d14;border:1px solid #2a2a3a;color:#fff;
  padding:6px 10px;border-radius:6px;font-size:12px;width:100%;
  font-family:monospace;}
.var-grid input:focus{outline:none;border-color:#ff4444;}
.var-empty{color:#555;font-size:12px;font-style:italic;padding:6px 0;}
.steps-list{font-family:monospace;font-size:11px;line-height:1.7;}
.step-row{display:flex;gap:8px;padding:4px 8px;border-radius:4px;}
.step-row:hover{background:rgba(255,255,255,0.03);}
.step-num{color:#444;width:24px;text-align:right;}
.step-type{color:#ff7799;font-weight:600;width:90px;}
.step-target{color:#74e0a0;}
.step-value{color:#888;}
.dot{width:8px;height:8px;border-radius:50%;background:#3a3a3a;
  flex-shrink:0;}
.dot.green{background:#4ade80;}
.dot.yellow{background:#fbbf24;}
.dot.red{background:#f87171;}
.section-title{font-size:13px;color:#888;text-transform:uppercase;
  letter-spacing:1px;margin:24px 0 10px;display:flex;align-items:center;
  gap:10px;}
.section-title .count{background:#1a1a2a;color:#aaa;font-size:11px;
  padding:2px 8px;border-radius:10px;}
.audit{background:#0d0d14;border:1px solid #1a1a2a;border-radius:12px;
  padding:14px 18px;font-family:monospace;font-size:11px;color:#bbb;
  max-height:240px;overflow-y:auto;line-height:1.6;}
.toast{position:fixed;bottom:24px;right:24px;background:#1a3a1a;
  color:#4ade80;padding:12px 18px;border-radius:10px;
  border:1px solid #2a4a2a;font-size:13px;font-weight:600;
  transform:translateY(120px);opacity:0;transition:all 0.25s;
  z-index:1000;}
.toast.show{transform:translateY(0);opacity:1;}
.toast.error{background:#3a1a1a;color:#f87171;border-color:#4a2a2a;}
@keyframes flash{
  0%,100%{box-shadow:0 0 0 0 rgba(255,68,68,0);}
  50%{box-shadow:0 0 0 6px rgba(255,68,68,0.35);}
}
.script-card.flash{animation:flash 0.8s ease-in-out 2;}

/* Day 3: schedule editor */
.sched-row{display:flex;gap:10px;align-items:center;margin-bottom:10px;
  flex-wrap:wrap;}
.sched-row label{font-size:11px;color:#888;text-transform:uppercase;
  letter-spacing:1px;}
.sched-row select,.sched-row input{background:#0d0d14;border:1px solid #2a2a3a;
  color:#fff;padding:6px 10px;border-radius:6px;font-size:12px;
  font-family:inherit;}
.sched-row select:focus,.sched-row input:focus{outline:none;border-color:#ff4444;}
.sched-row .preset{background:#1a1a2a;border:none;color:#aaa;font-size:11px;
  padding:4px 10px;border-radius:14px;cursor:pointer;}
.sched-row .preset:hover{background:#ff4444;color:#fff;}
.sched-help{font-size:11px;color:#666;margin-top:4px;font-family:monospace;}
.next-fire{font-size:12px;color:#fbbf24;margin-top:6px;}
.next-fire.disabled{color:#555;}

/* Day 4: step editor */
.step-edit-row{display:grid;
  grid-template-columns:24px 100px 1fr 1fr 80px;
  gap:6px;align-items:center;margin-bottom:6px;}
.step-edit-row select,.step-edit-row input{background:#0d0d14;
  border:1px solid #2a2a3a;color:#fff;padding:5px 8px;border-radius:5px;
  font-size:11px;font-family:monospace;}
.step-edit-row .step-num{font-family:monospace;color:#666;text-align:right;}
.step-edit-row .step-ctrls{display:flex;gap:3px;}
.step-edit-row .step-ctrls button{background:#1a1a2a;border:none;color:#aaa;
  width:22px;height:22px;border-radius:4px;cursor:pointer;font-size:11px;
  display:flex;align-items:center;justify-content:center;}
.step-edit-row .step-ctrls button:hover{background:#ff4444;color:#fff;}
.add-step-btn{background:transparent;border:1px dashed #2a2a3a;color:#888;
  padding:8px;border-radius:6px;cursor:pointer;width:100%;font-size:12px;
  font-family:inherit;margin-top:6px;}
.add-step-btn:hover{border-color:#ff4444;color:#fff;}

/* Day 5: trust badge */
.trust-badge{font-size:10px;font-weight:600;text-transform:uppercase;
  padding:2px 8px;border-radius:10px;letter-spacing:0.5px;}
.trust-casual{background:#3a2a2a;color:#f87171;}
.trust-approved{background:#3a3a1a;color:#fbbf24;}
.trust-trusted{background:#1a3a2a;color:#4ade80;}
.trust-admin{background:#2a2a3a;color:#a78bfa;}
.promote-banner{background:linear-gradient(90deg,#16a34a,#22c55e);
  color:#fff;padding:10px 14px;border-radius:8px;font-size:13px;
  margin-top:12px;display:flex;align-items:center;gap:12px;
  box-shadow:0 0 0 1px rgba(74,222,128,0.3);}
.promote-banner .grow{flex:1;}
.promote-banner button{background:rgba(255,255,255,0.15);border:none;
  color:#fff;padding:6px 12px;border-radius:5px;font-weight:600;
  cursor:pointer;font-size:12px;}
.promote-banner button:hover{background:rgba(255,255,255,0.25);}
.promote-banner button.dismiss{background:transparent;}

/* Day 5: confirmation modal */
.modal-bg{position:fixed;inset:0;background:rgba(0,0,0,0.7);
  display:none;align-items:center;justify-content:center;z-index:2000;}
.modal-bg.show{display:flex;}
.modal{background:#0d0d14;border:1px solid #2a2a3a;border-radius:12px;
  padding:24px;max-width:480px;width:90%;}
.modal h3{font-size:16px;margin-bottom:10px;}
.modal p{color:#aaa;font-size:13px;line-height:1.6;margin-bottom:16px;}
.modal .actions{display:flex;gap:10px;justify-content:flex-end;}
</style>
</head>
<body>

<div class="header">
  <h1><span>Molt</span>Automation</h1>
  <span class="badge">Beta</span>
  <div class="bar"></div>
  <a href="molt://ai/" style="color:#888;text-decoration:none;font-size:13px;">AI Chat</a>
  <a href="molt://ai-agent/" style="color:#888;text-decoration:none;font-size:13px;">Agent</a>
  <a href="molt://ai-settings/" style="color:#888;text-decoration:none;font-size:13px;">Settings</a>
</div>

<div class="container">

  <p class="intro">
    Record any web workflow, save it, and replay it on a schedule.
    Each script runs <b>locally</b> with your logged-in cookies and uses
    the <b>bundled local AI</b> for decisions like &ldquo;is this price low
    enough?&rdquo; To record: click the <b>&#9210; Record</b> button in the
    toolbar, perform your workflow, then click <b>&#x25A0; Stop</b>.
    <br><br>
    No cloud, no telemetry, no Google. Schedules survive browser restarts.
  </p>

  <div class="toolbar-row">
    <button class="btn"        onclick="addBlank()">+ New blank</button>
    <button class="btn ghost"  onclick="addSample()">+ Sample</button>
    <button class="btn ghost"  onclick="document.getElementById('import-input').click()">
      &#8682; Import
    </button>
    <input type="file" id="import-input" accept=".molt,.json,application/json"
           style="display:none" onchange="onImport(event)">
    <button class="btn ghost"  onclick="refresh()">&#x21bb; Refresh</button>
    <div class="grow"></div>
    <input type="text" class="search-input" id="search"
           placeholder="Search by name..."
           oninput="renderScripts(allScripts)">
    <button class="btn ghost"
            onclick="document.getElementById('audit-block').style.display='block';refreshAudit();">
      Audit log
    </button>
  </div>

  <div class="section-title">
    Saved automations
    <span class="count" id="count">0</span>
  </div>
  <div class="script-list" id="list">
    <div class="empty" id="empty">
      No automations yet. Click <b>+ Sample script</b> to create one,
      or hit the toolbar <b>&#9210; Record</b> button on any tab to
      capture your first workflow.
    </div>
  </div>

  <div id="audit-block" style="display:none;">
    <div class="section-title">Audit log (most recent first)</div>
    <pre class="audit" id="audit"></pre>
  </div>

</div>

<div class="modal-bg" id="modal-bg" onclick="closeModalIfBg(event)">
  <div class="modal" id="modal-content"></div>
</div>

<div class="toast" id="toast"></div>

<script>
var idCounter = 0;
var pendingCbs = {};
var allScripts = [];
function sendWithPromise(method) {
  var args = Array.prototype.slice.call(arguments, 1);
  var id = method + '_' + (++idCounter);
  return new Promise(function(resolve, reject) {
    pendingCbs[id] = {resolve: resolve, reject: reject};
    chrome.send(method, [id].concat(args));
  });
}
window.cr = window.cr || {};
cr.webUIResponse = function(id, ok, resp) {
  var cb = pendingCbs[id];
  if (cb) { delete pendingCbs[id]; ok ? cb.resolve(resp) : cb.reject(resp); }
};

function showToast(msg, isError) {
  var t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show' + (isError ? ' error' : '');
  setTimeout(function(){ t.className = 'toast' + (isError ? ' error' : ''); }, 2400);
}

function esc(s) {
  var d = document.createElement('div');
  d.textContent = String(s == null ? '' : s);
  return d.innerHTML;
}

function dotClass(s) {
  if (s.success_rate >= 90) return 'green';
  if (s.success_rate >= 1)  return 'yellow';
  if (s.stats && s.stats.runs > 0) return 'red';
  return '';
}

function triggerLabel(s) {
  if (!s.trigger) return 'Manual';
  switch (s.trigger.type) {
    case 'cron':     return 'Cron: ' + (s.trigger.expression || '?');
    case 'interval': return 'Every ' + (s.trigger.expression || '?') + 's';
    case 'at':       return 'Once at ' + (s.trigger.expression || '?');
    case 'on_event': return 'On: ' + (s.trigger.expression || '?');
    default:         return 'Manual';
  }
}

// Find every {{name}} placeholder used in step targets and values.
function extractVarsFromScript(s) {
  var found = {};
  function scan(str) {
    if (!str) return;
    var re = /\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}/g;
    var m;
    while ((m = re.exec(str)) !== null) found[m[1]] = true;
  }
  (s.steps || []).forEach(function(step){
    scan(step.target); scan(step.value); scan(step.description);
  });
  return Object.keys(found).sort();
}

function stepShortDesc(step) {
  var t = step.type || '?';
  var pieces = ['<span class="step-type">' + esc(t.toUpperCase()) + '</span>'];
  if (step.target) pieces.push(
      '<span class="step-target">' + esc(step.target) + '</span>');
  if (step.value)  pieces.push(
      '<span class="step-value">= ' + esc(step.value) + '</span>');
  return pieces.join(' ');
}

// Day 3: list of step types the editor dropdown offers.
var STEP_TYPES = ['navigate','click','type','scroll','keypress','wait',
  'wait_for','extract','ai_extract','ai_decide','if','else','end_if',
  'loop','end_loop','assert','notify','screenshot','run_js'];

function buildScheduleHTML(s) {
  var trig = s.trigger || {type:'manual', expression:''};
  var t = trig.type || 'manual';
  var expr = trig.expression || '';

  function opt(v, label, sel) {
    return '<option value="' + v + '"' + (v===sel?' selected':'') + '>' +
           esc(label) + '</option>';
  }

  var typeSelect = '<select data-sched="type">' +
    opt('manual','Manual (only when I click Run)', t) +
    opt('cron','Cron expression', t) +
    opt('interval','Every N seconds', t) +
    opt('at','Once at a specific time', t) +
    '</select>';

  // Cron presets — clickable shortcuts that fill the expression input.
  var presets =
    '<button class="preset" data-cron="0 9 * * 1-5">Weekdays 9am</button>' +
    '<button class="preset" data-cron="0 */6 * * *">Every 6 hours</button>' +
    '<button class="preset" data-cron="0 9 * * 6">Saturdays 9am</button>' +
    '<button class="preset" data-cron="*/15 * * * *">Every 15 min</button>';

  var cronBlock = '<div data-sched-pane="cron"' +
      (t==='cron'?'':' style="display:none"') + '>' +
    '<div class="sched-row">' +
      '<label>Cron</label>' +
      '<input type="text" data-sched="expr-cron" placeholder="m h dom mon dow"' +
        ' value="' + esc(t==='cron'?expr:'') + '" style="min-width:240px;">' +
      presets +
    '</div>' +
    '<div class="sched-help">Standard 5-field cron. Supports * , - / ranges and lists.</div>' +
    '</div>';

  var intervalSecs = (t==='interval' && expr) ? parseInt(expr,10) : 3600;
  var intervalBlock = '<div data-sched-pane="interval"' +
      (t==='interval'?'':' style="display:none"') + '>' +
    '<div class="sched-row">' +
      '<label>Run every</label>' +
      '<input type="number" min="10" data-sched="expr-interval-num"' +
        ' value="' + intervalSecs + '" style="width:90px;">' +
      '<select data-sched="expr-interval-unit">' +
        '<option value="1">seconds</option>' +
        '<option value="60">minutes</option>' +
        '<option value="3600" selected>hours</option>' +
      '</select>' +
    '</div>' +
    '</div>';

  // For "at": split ISO into datetime-local format.
  var atVal = (t==='at') ? expr.replace('Z','').slice(0,16) : '';
  var atBlock = '<div data-sched-pane="at"' +
      (t==='at'?'':' style="display:none"') + '>' +
    '<div class="sched-row">' +
      '<label>Fire at</label>' +
      '<input type="datetime-local" data-sched="expr-at" value="' + esc(atVal) + '">' +
    '</div>' +
    '</div>';

  var nextLine = '';
  if (s.next_fire_unix) {
    var d = new Date(s.next_fire_unix * 1000);
    var ms = d.getTime() - Date.now();
    var rel;
    if (ms < 0) rel = 'now';
    else if (ms < 60*1000) rel = 'in <1m';
    else if (ms < 60*60*1000) rel = 'in ' + Math.round(ms/60000) + 'm';
    else if (ms < 24*60*60*1000) rel = 'in ' + Math.round(ms/3600000) + 'h';
    else rel = 'in ' + Math.round(ms/86400000) + 'd';
    nextLine = '<div class="next-fire">Next fire: ' + esc(d.toLocaleString()) +
               ' (' + rel + ')</div>';
  } else if (t !== 'manual') {
    nextLine = '<div class="next-fire disabled">No upcoming fire</div>';
  }

  var enabledHTML = '<label style="font-size:12px;color:#aaa;">' +
    '<input type="checkbox" data-sched="enabled"' +
    (s.enabled === false ? '' : ' checked') + ' style="margin-right:6px;">' +
    'Enabled (untick to pause without losing schedule)</label>';

  return ''
    + '<div class="detail-section">'
    +   '<h4>Schedule</h4>'
    +   '<div class="sched-row">' + typeSelect + enabledHTML + '</div>'
    +   cronBlock + intervalBlock + atBlock
    +   nextLine
    +   '<div style="margin-top:10px;">'
    +     '<button class="btn green small" data-act="save-schedule">Save schedule</button> '
    +     '<button class="btn ghost small" data-act="snooze-1h">Snooze 1h</button> '
    +     '<button class="btn ghost small" data-act="skip-next">Skip next</button>'
    +   '</div>'
    + '</div>';
}

function buildStepEditor(s) {
  var html = '';
  (s.steps || []).forEach(function(step, i){
    var typeOpts = STEP_TYPES.map(function(t){
      return '<option value="' + t + '"' +
             (step.type === t ? ' selected' : '') + '>' + t + '</option>';
    }).join('');
    html +=
      '<div class="step-edit-row" data-step="' + i + '">' +
        '<span class="step-num">' + (i + 1) + '.</span>' +
        '<select data-field="type">' + typeOpts + '</select>' +
        '<input data-field="target" placeholder="selector / url"' +
          ' value="' + esc(step.target || '') + '">' +
        '<input data-field="value" placeholder="value / prompt"' +
          ' value="' + esc(step.value || '') + '">' +
        '<div class="step-ctrls">' +
          '<button data-step-act="up" title="Move up">&uarr;</button>' +
          '<button data-step-act="down" title="Move down">&darr;</button>' +
          '<button data-step-act="del" title="Delete">&times;</button>' +
        '</div>' +
      '</div>';
  });
  if (!html) html = '<div class="var-empty">No steps. Click +Add step.</div>';
  return html +
    '<button class="add-step-btn" data-act="add-step">+ Add step</button>';
}

function buildTrustPanel(s) {
  var trust = (s.security && s.security.trust) || 'casual';
  var levels = ['casual','approved','trusted','admin'];
  var dropdown = '<select data-trust="select">' +
    levels.map(function(l){
      return '<option value="' + l + '"' +
             (l===trust?' selected':'') + '>' +
             l[0].toUpperCase() + l.slice(1) + '</option>';
    }).join('') + '</select>';

  var promote = '';
  if (s.suggested_trust && s.suggested_trust !== trust) {
    promote =
      '<div class="promote-banner">' +
        '<span>&#10003;</span>' +
        '<div class="grow">This script ran successfully ' +
          ((s.stats && s.stats.successes) || 0) +
          ' times. Promote to <b>' + esc(s.suggested_trust) +
          '</b> for ' +
          (s.suggested_trust === 'trusted'
            ? 'headless background runs'
            : 'fewer approval prompts') + '?' +
        '</div>' +
        '<button data-act="trust-promote" data-target="' +
          esc(s.suggested_trust) + '">Yes, promote</button>' +
        '<button class="dismiss" data-act="trust-dismiss">Later</button>' +
      '</div>';
  }
  return '<div class="detail-section">' +
    '<h4>Trust level</h4>' +
    '<div class="sched-row">' + dropdown +
      '<button class="btn ghost small" data-act="save-trust">Save</button>' +
    '</div>' +
    '<div class="sched-help">' +
      'Casual: only manual runs, no submit-shaped clicks. ' +
      'Approved: auto-fill OK after one approval. ' +
      'Trusted: scheduled headless runs allowed. ' +
      'Admin: RUN_JS and chained scripts.' +
    '</div>' +
    promote +
    '</div>';
}

function buildDetailHTML(s) {
  var vars = extractVarsFromScript(s);
  var dv = s.default_variables || {};
  var varsBlock = vars.length === 0
    ? '<div class="var-empty">No {{variables}} used in this script.</div>'
    : '<div class="var-grid">' + vars.map(function(name){
        var val = dv[name] || '';
        return '<label>{{' + esc(name) + '}}</label>' +
               '<input type="text" data-var="' + esc(name) + '"' +
               ' value="' + esc(val) + '" placeholder="value...">';
      }).join('') + '</div>';

  var lastRun = s.stats && s.stats.last_run_unix
      ? new Date(s.stats.last_run_unix * 1000).toLocaleString()
      : 'never';
  var runs = (s.stats && s.stats.runs) || 0;
  var ok   = (s.stats && s.stats.successes) || 0;

  return ''
    + buildScheduleHTML(s)
    + '<div class="detail-section">'
    +   '<h4>Variables (used by {{name}} placeholders inside steps)</h4>'
    +   varsBlock
    + '</div>'
    + '<div class="detail-section">'
    +   '<h4>Steps (' + (s.steps ? s.steps.length : 0) + ')</h4>'
    +   '<div class="step-editor">' + buildStepEditor(s) + '</div>'
    + '</div>'
    + buildTrustPanel(s)
    + '<div class="detail-section">'
    +   '<h4>Stats</h4>'
    +   '<div style="font-size:12px;color:#aaa;line-height:1.7;">'
    +     'Runs: ' + runs + ' &middot; Successes: ' + ok
    +     ' &middot; Last run: ' + esc(lastRun)
    +     (s.stats && s.stats.last_result
        ? '<br>Last result: ' + esc(s.stats.last_result) : '')
    +   '</div>'
    + '</div>'
    + '<div class="detail-section">'
    +   '<button class="btn green"  data-act="run-with-vars">'
    +     '&#9654; Run with these values</button> '
    +   '<button class="btn ghost"  data-act="save-vars">Save values as defaults</button> '
    +   '<button class="btn ghost"  data-act="save-steps">Save step changes</button> '
    +   '<button class="btn ghost"  data-act="export">&#8681; Export</button> '
    +   '<button class="btn ghost"  data-act="copy-id">Copy ID</button> '
    + '</div>';
}

function collectVars(card) {
  var vars = {};
  card.querySelectorAll('input[data-var]').forEach(function(inp){
    vars[inp.getAttribute('data-var')] = inp.value;
  });
  return vars;
}

function renderScripts(scripts) {
  allScripts = scripts;
  var query = (document.getElementById('search').value || '').toLowerCase();
  var filtered = scripts.filter(function(s){
    if (!query) return true;
    return (s.name || '').toLowerCase().indexOf(query) !== -1 ||
           (s.id   || '').toLowerCase().indexOf(query) !== -1;
  });

  var list = document.getElementById('list');
  var empty = document.getElementById('empty');
  document.getElementById('count').textContent = scripts.length;
  list.innerHTML = '';
  if (!filtered.length) {
    if (!scripts.length) { list.appendChild(empty); }
    else { list.innerHTML =
      '<div class="empty">No automations match &ldquo;' + esc(query) +
      '&rdquo;</div>'; }
    return;
  }

  // Highlight just-recorded script if URL hash carries #new=ID.
  var newId = (location.hash.match(/^#new=(.+)$/) || [])[1];

  filtered.forEach(function(s) {
    var card = document.createElement('div');
    card.className = 'script-card';
    if (s.id === newId) card.classList.add('flash', 'open', 'highlight');
    var lastRun = s.stats && s.stats.last_run_unix
        ? new Date(s.stats.last_run_unix * 1000).toLocaleString()
        : 'never';
    var trust = (s.security && s.security.trust) || 'casual';
    card.innerHTML =
      '<div class="script-summary">' +
        '<span class="dot ' + dotClass(s) + '"></span>' +
        '<div class="info">' +
          '<div class="name">' + esc(s.name) +
            ' <span class="trust-badge trust-' + trust + '">' +
            esc(trust) + '</span></div>' +
          '<div class="meta">' + esc(triggerLabel(s)) +
          ' &middot; ' + (s.steps ? s.steps.length : 0) + ' steps' +
          ' &middot; last run: ' + esc(lastRun) +
          (s.stats && s.stats.last_result
            ? ' &middot; ' + esc(s.stats.last_result) : '') +
          '</div>' +
        '</div>' +
        '<div class="actions">' +
          '<button class="btn"        data-act="run">&#9654; Run</button>' +
          '<button class="btn ghost"  data-act="del">Delete</button>' +
        '</div>' +
        '<span class="chev">&#10095;</span>' +
      '</div>' +
      '<div class="script-detail">' + buildDetailHTML(s) + '</div>';

    var summary = card.querySelector('.script-summary');
    summary.onclick = function(e) {
      if (e.target.tagName === 'BUTTON') return;
      card.classList.toggle('open');
    };
    card.querySelector('[data-act="run"]').onclick = function(e) {
      e.stopPropagation();
      runScript(s.id);
    };
    card.querySelector('[data-act="del"]').onclick = function(e) {
      e.stopPropagation();
      deleteScript(s.id);
    };
    var rwv = card.querySelector('[data-act="run-with-vars"]');
    if (rwv) rwv.onclick = function() {
      runScript(s.id, collectVars(card));
    };
    var sv = card.querySelector('[data-act="save-vars"]');
    if (sv) sv.onclick = function() {
      saveVarsFor(s, collectVars(card));
    };
    var cp = card.querySelector('[data-act="copy-id"]');
    if (cp) cp.onclick = function() {
      navigator.clipboard.writeText(s.id);
      showToast('ID copied: ' + s.id);
    };

    // Day 3: schedule editor wiring.
    var schedSel = card.querySelector('[data-sched="type"]');
    if (schedSel) schedSel.onchange = function() {
      // Show only the right pane.
      var t = this.value;
      ['cron','interval','at'].forEach(function(p){
        var el = card.querySelector('[data-sched-pane="' + p + '"]');
        if (el) el.style.display = (p === t) ? '' : 'none';
      });
    };
    card.querySelectorAll('button.preset[data-cron]').forEach(function(btn){
      btn.onclick = function() {
        var inp = card.querySelector('[data-sched="expr-cron"]');
        if (inp) inp.value = btn.getAttribute('data-cron');
      };
    });
    var saveSched = card.querySelector('[data-act="save-schedule"]');
    if (saveSched) saveSched.onclick = function() {
      saveSchedule(s, card);
    };
    var snooze = card.querySelector('[data-act="snooze-1h"]');
    if (snooze) snooze.onclick = function() {
      snoozeFor(s, 3600);
    };
    var skipNext = card.querySelector('[data-act="skip-next"]');
    if (skipNext) skipNext.onclick = function() {
      skipNextFire(s);
    };

    // Day 4: step editor wiring.
    card.querySelectorAll('.step-edit-row').forEach(function(row){
      var idx = parseInt(row.getAttribute('data-step'), 10);
      ['type','target','value'].forEach(function(field){
        var el = row.querySelector('[data-field="' + field + '"]');
        if (el) el.onchange = function() {
          if (!s.steps[idx]) return;
          s.steps[idx][field] = el.value;
        };
      });
      row.querySelectorAll('[data-step-act]').forEach(function(btn){
        btn.onclick = function() {
          var act = btn.getAttribute('data-step-act');
          if (act === 'up' && idx > 0) {
            var tmp = s.steps[idx-1]; s.steps[idx-1] = s.steps[idx]; s.steps[idx] = tmp;
          } else if (act === 'down' && idx < s.steps.length - 1) {
            var tmp = s.steps[idx+1]; s.steps[idx+1] = s.steps[idx]; s.steps[idx] = tmp;
          } else if (act === 'del') {
            s.steps.splice(idx, 1);
          }
          card.querySelector('.step-editor').innerHTML = buildStepEditor(s);
          // Re-bind handlers by re-rendering this card only.
          renderScripts(allScripts);
        };
      });
    });
    var addStep = card.querySelector('[data-act="add-step"]');
    if (addStep) addStep.onclick = function() {
      s.steps = s.steps || [];
      s.steps.push({type:'navigate', target:'', value:'', description:''});
      card.querySelector('.step-editor').innerHTML = buildStepEditor(s);
      renderScripts(allScripts);
    };
    var saveSteps = card.querySelector('[data-act="save-steps"]');
    if (saveSteps) saveSteps.onclick = function() {
      // Read back final type/target/value from the DOM in case onchange
      // didn't fire on all fields (e.g. user clicked Save without
      // blurring the input).
      card.querySelectorAll('.step-edit-row').forEach(function(row){
        var idx = parseInt(row.getAttribute('data-step'), 10);
        if (!s.steps[idx]) return;
        ['type','target','value'].forEach(function(field){
          var el = row.querySelector('[data-field="' + field + '"]');
          if (el) s.steps[idx][field] = el.value;
        });
      });
      saveScript(s);
    };

    var exp = card.querySelector('[data-act="export"]');
    if (exp) exp.onclick = function() { exportScript(s); };

    // Day 5: trust panel wiring.
    var saveTrust = card.querySelector('[data-act="save-trust"]');
    if (saveTrust) saveTrust.onclick = function() {
      var sel = card.querySelector('[data-trust="select"]');
      if (sel) promoteTrust(s.id, sel.value);
    };
    var promoteBtn = card.querySelector('[data-act="trust-promote"]');
    if (promoteBtn) promoteBtn.onclick = function() {
      promoteTrust(s.id, promoteBtn.getAttribute('data-target'));
    };
    var dismissBtn = card.querySelector('[data-act="trust-dismiss"]');
    if (dismissBtn) dismissBtn.onclick = function() {
      var banner = card.querySelector('.promote-banner');
      if (banner) banner.style.display = 'none';
    };

    list.appendChild(card);
  });

  // Scroll the highlighted card into view if we just got here from
  // the toolbar Stop action.
  if (newId) {
    var hl = list.querySelector('.script-card.highlight');
    if (hl) hl.scrollIntoView({behavior:'smooth', block:'center'});
  }
}

function refresh() {
  sendWithPromise('listScripts').then(function(r) {
    renderScripts(r.scripts || []);
  }).catch(function(e) { showToast('Failed to load: ' + e, true); });
}

function refreshAudit() {
  sendWithPromise('auditTail', 200).then(function(r) {
    var lines = (r.lines || []).slice().reverse();
    document.getElementById('audit').textContent =
      lines.length ? lines.join('\n') : '(empty)';
  });
}

function addSample() {
  sendWithPromise('createSampleScript').then(function(r) {
    if (r.success) { showToast('Sample script created'); refresh(); }
    else           { showToast('Failed to create sample', true); }
  });
}

function runScript(id, vars) {
  var args = vars ? ['runScript', id, vars] : ['runScript', id];
  sendWithPromise.apply(null, args).then(function(r) {
    if (r.success) showToast('Running: ' + (r.name || id));
    else           showToast('Run failed: ' + (r.error || ''), true);
    refresh();
    refreshAudit();
  });
}

function saveVarsFor(scriptObj, vars) {
  // Merge new vars into the script's default_variables and save.
  scriptObj.default_variables = scriptObj.default_variables || {};
  Object.keys(vars).forEach(function(k){
    scriptObj.default_variables[k] = vars[k];
  });
  sendWithPromise('saveScript', scriptObj).then(function(r){
    if (r.success) showToast('Default values saved');
    else showToast('Save failed: ' + (r.error || ''), true);
  });
}

function deleteScript(id) {
  if (!confirm('Delete this automation?')) return;
  sendWithPromise('deleteScript', id).then(function(r) {
    if (r.success) { showToast('Deleted'); refresh(); }
    else           { showToast('Delete failed', true); }
  });
}

// Day 3: schedule save / snooze / skip
function saveSchedule(scriptObj, card) {
  var t = card.querySelector('[data-sched="type"]').value;
  var enabled = card.querySelector('[data-sched="enabled"]').checked;
  var expr = '';
  if (t === 'cron') {
    expr = card.querySelector('[data-sched="expr-cron"]').value.trim();
    if (!expr) { showToast('Cron expression is required', true); return; }
  } else if (t === 'interval') {
    var n = parseInt(card.querySelector('[data-sched="expr-interval-num"]').value, 10);
    var u = parseInt(card.querySelector('[data-sched="expr-interval-unit"]').value, 10);
    if (!n || n < 1) { showToast('Enter a positive interval', true); return; }
    expr = String(n * u);
  } else if (t === 'at') {
    var v = card.querySelector('[data-sched="expr-at"]').value;
    if (!v) { showToast('Pick a date and time', true); return; }
    // datetime-local has no timezone — append local offset.
    expr = new Date(v).toISOString();
  }
  scriptObj.trigger = {type: t, expression: expr,
                        timezone: '', until_unix: 0};
  scriptObj.enabled = enabled;
  saveScript(scriptObj, 'Schedule saved');
}

function saveScript(scriptObj, successMsg) {
  sendWithPromise('saveScript', scriptObj).then(function(r){
    if (r.success) { showToast(successMsg || 'Saved'); refresh(); }
    else { showToast('Save failed: ' + (r.error || ''), true); }
  });
}

function snoozeFor(scriptObj, seconds) {
  // Push the next-fire deadline by N seconds by setting last_fired_unix
  // to (now - interval_or_offset). For cron, this won't strictly skip
  // — instead we set enabled=false + a one-shot re-enable timer (not
  // implemented for v1). Simple v1: bump last_fired so interval-based
  // triggers wait `seconds` more before firing.
  if (!scriptObj.trigger) scriptObj.trigger = {type:'manual'};
  scriptObj.trigger.last_fired_unix =
    Math.floor(Date.now() / 1000) - 0 + seconds; // a bit forward
  saveScript(scriptObj, 'Snoozed for 1h');
}

function skipNextFire(scriptObj) {
  if (!scriptObj.trigger) return;
  // Mark "last_fired" as the upcoming computed fire so the scheduler's
  // catch-up logic considers it already done.
  if (scriptObj.next_fire_unix) {
    scriptObj.trigger.last_fired_unix = scriptObj.next_fire_unix + 1;
  } else {
    scriptObj.trigger.last_fired_unix = Math.floor(Date.now() / 1000);
  }
  saveScript(scriptObj, 'Next fire skipped');
}

// Day 4: blank create + import + export
function addBlank() {
  sendWithPromise('createBlankScript').then(function(r){
    if (r.success) {
      showToast('Blank script created');
      location.hash = '#new=' + r.script_id;
      refresh();
    } else {
      showToast('Failed to create blank', true);
    }
  });
}

function exportScript(scriptObj) {
  var pretty = JSON.stringify(scriptObj, null, 2);
  var blob = new Blob([pretty], {type: 'application/json'});
  var url  = URL.createObjectURL(blob);
  var a    = document.createElement('a');
  a.href = url;
  a.download = (scriptObj.id || 'molt-script') + '.molt';
  a.click();
  setTimeout(function() { URL.revokeObjectURL(url); }, 4000);
  showToast('Exported ' + a.download);
}

function onImport(ev) {
  var f = ev.target.files && ev.target.files[0];
  if (!f) return;
  var reader = new FileReader();
  reader.onload = function() {
    var parsed;
    try { parsed = JSON.parse(reader.result); }
    catch (e) { showToast('Not valid JSON: ' + e.message, true); return; }
    sendWithPromise('importScript', parsed).then(function(r){
      if (r.success) {
        showToast('Imported as ' + r.script_id);
        location.hash = '#new=' + r.script_id;
        refresh();
      } else {
        showToast('Import failed: ' + (r.error || ''), true);
      }
    });
  };
  reader.readAsText(f);
  // Reset so the same file can be re-imported if needed.
  ev.target.value = '';
}

// Day 5: trust promotion
function promoteTrust(id, level) {
  sendWithPromise('promoteTrust', id, level).then(function(r){
    if (r.success) { showToast('Trust set to ' + r.trust); refresh(); }
    else           { showToast('Trust update failed', true); }
  });
}

// Day 5: simple modal helpers (used by future flows)
function closeModalIfBg(e) {
  if (e.target.id === 'modal-bg') closeModal();
}
function closeModal() {
  document.getElementById('modal-bg').classList.remove('show');
}

refresh();
// Auto-refresh every 30s so next-fire countdowns stay live.
setInterval(refresh, 30000);
</script>
</body></html>
)HTML";
    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldServeMimeTypeAsContentTypeHeader() override { return true; }
};

}  // namespace

MoltAIAutomationUI::MoltAIAutomationUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIAutomationDataSource>());
  web_ui->AddMessageHandler(std::make_unique<MoltAIAutomationHandler>());
}

MoltAIAutomationUI::~MoltAIAutomationUI() = default;
