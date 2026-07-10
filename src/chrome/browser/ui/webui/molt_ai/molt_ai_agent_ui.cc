// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai-agent/ — Agent-mode automation studio, hosted in the browser
// side panel (AgentSidePanelWebView). Records user actions step-by-step, lets
// the user review/edit them, saves them as reusable workflows, runs them (with
// per-run variable values), and schedules them on an interval. Built on the
// existing automation engine (AutomationStorage / AutomationRecorder /
// AutomationRunner / AutomationScheduler).

#include "chrome/browser/ui/webui/molt_ai/molt_ai_agent_ui.h"

#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_background_browser.h"
#include "chrome/browser/molt_ai/automation/automation_recorder_tab_helper.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_factory.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "url/gurl.h"

namespace {

using molt_ai::automation::AutomationRecorderTabHelper;
using molt_ai::automation::AutomationScheduler;
using molt_ai::automation::AutomationSchedulerServiceFactory;
using molt_ai::automation::AutomationStorage;
using molt_ai::automation::RunScriptInBackgroundBrowser;
using molt_ai::automation::Script;
using molt_ai::automation::Step;
using molt_ai::automation::StepType;
using molt_ai::automation::StepTypeToString;
using molt_ai::automation::Trigger;
using molt_ai::automation::TriggerType;

// ---------------------------------------------------------------------------
// Friendly rendering helpers.
// ---------------------------------------------------------------------------
std::string StepIcon(StepType t) {
  switch (t) {
    case StepType::NAVIGATE: return "🌐";
    case StepType::CLICK: return "👆";
    case StepType::TYPE: return "⌨️";
    case StepType::SCROLL: return "📜";
    case StepType::KEYPRESS: return "⏎";
    case StepType::WAIT:
    case StepType::WAIT_FOR: return "⏳";
    case StepType::EXTRACT:
    case StepType::AI_EXTRACT: return "📋";
    case StepType::AI_DECIDE: return "🧠";
    case StepType::ASSERT: return "✅";
    case StepType::NOTIFY: return "🔔";
    default: return "•";
  }
}

// The {index,type,icon,title,detail,value} dict the UI renders per live step.
base::DictValue RecorderStepDict(const Step& s, int index) {
  base::DictValue d;
  d.Set("index", index);
  d.Set("type", StepTypeToString(s.type));
  d.Set("icon", StepIcon(s.type));
  d.Set("title", s.description.empty() ? StepTypeToString(s.type)
                                       : s.description);
  d.Set("detail", s.target);
  d.Set("value", s.value);
  return d;
}

std::string ScheduleLabel(const Trigger& t) {
  switch (t.type) {
    case TriggerType::INTERVAL: {
      int secs = 0;
      base::StringToInt(t.expression, &secs);
      if (secs >= 3600 && secs % 3600 == 0)
        return "every " + base::NumberToString(secs / 3600) + "h";
      if (secs >= 60 && secs % 60 == 0)
        return "every " + base::NumberToString(secs / 60) + " min";
      return "every " + base::NumberToString(secs) + "s";
    }
    case TriggerType::CRON:
      return "on schedule";
    case TriggerType::AT:
      return "once";
    case TriggerType::ON_EVENT:
      return "on " + t.expression;
    default:
      return "";
  }
}

// Summary dict for the library list.
base::DictValue WorkflowSummary(const Script& s) {
  base::DictValue d;
  d.Set("id", s.id);
  d.Set("name", s.name);
  d.Set("stepCount", static_cast<int>(s.steps.size()));
  d.Set("enabled", s.enabled);
  if (s.stats.runs > 0 && s.stats.last_run_unix > 0) {
    base::DictValue lr;
    lr.Set("ok", s.stats.last_failed_step_index < 0);
    lr.Set("ts", static_cast<double>(s.stats.last_run_unix));
    lr.Set("message", s.stats.last_result);
    d.Set("lastRun", std::move(lr));
  }
  if (s.trigger.type != TriggerType::MANUAL) {
    base::DictValue sch;
    sch.Set("label", ScheduleLabel(s.trigger));
    base::Time next =
        AutomationScheduler::NextFireTime(s.trigger, base::Time::Now());
    sch.Set("nextFire",
            next.is_null() ? 0.0 : next.InSecondsFSinceUnixEpoch());
    d.Set("schedule", std::move(sch));
  }
  return d;
}

std::string NewWorkflowId() {
  return "wf_" +
         base::NumberToString(static_cast<int64_t>(std::time(nullptr)));
}

// ---------------------------------------------------------------------------
// AgentStudioHandler — the JS <-> C++ bridge for the studio.
// ---------------------------------------------------------------------------
class AgentStudioHandler : public content::WebUIMessageHandler {
 public:
  AgentStudioHandler() = default;
  ~AgentStudioHandler() override = default;
  AgentStudioHandler(const AgentStudioHandler&) = delete;
  AgentStudioHandler& operator=(const AgentStudioHandler&) = delete;

  void RegisterMessages() override {
    auto bind = [this](const char* name, auto method) {
      web_ui()->RegisterMessageCallback(
          name, base::BindRepeating(method, base::Unretained(this)));
    };
    bind("listWorkflows", &AgentStudioHandler::HandleListWorkflows);
    bind("getWorkflow", &AgentStudioHandler::HandleGetWorkflow);
    bind("saveWorkflow", &AgentStudioHandler::HandleSaveWorkflow);
    bind("deleteWorkflow", &AgentStudioHandler::HandleDeleteWorkflow);
    bind("duplicateWorkflow", &AgentStudioHandler::HandleDuplicateWorkflow);
    bind("runWorkflow", &AgentStudioHandler::HandleRunWorkflow);
    bind("startRecording", &AgentStudioHandler::HandleStartRecording);
    bind("stopRecording", &AgentStudioHandler::HandleStopRecording);
    bind("saveSchedule", &AgentStudioHandler::HandleSaveSchedule);
    bind("cancelRun", &AgentStudioHandler::HandleCancelRun);
  }

 private:
  Profile* profile() {
    return Profile::FromBrowserContext(
        web_ui()->GetWebContents()->GetBrowserContext());
  }

  AutomationStorage* storage() {
    if (Profile* p = profile()) {
      if (auto* svc = AutomationSchedulerServiceFactory::GetForProfile(p)) {
        return svc->storage();
      }
    }
    return nullptr;
  }

  // The browser tab the user is actually working in (NOT this side-panel
  // WebContents, which is not a tab).
  content::WebContents* ActiveTab() {
    Profile* p = profile();
    if (!p) {
      return nullptr;
    }
    Browser* b = chrome::FindLastActiveWithProfile(p);
    if (b && b->tab_strip_model()) {
      return b->tab_strip_model()->GetActiveWebContents();
    }
    return nullptr;
  }

  void ResolveDict(const base::ListValue& args, base::DictValue out) {
    if (args.empty() || !args[0].is_string()) {
      return;
    }
    ResolveJavascriptCallback(base::Value(args[0].GetString()),
                              base::Value(std::move(out)));
  }

  // -------- messages --------
  void HandleListWorkflows(const base::ListValue& args) {
    AllowJavascript();
    base::ListValue list;
    if (auto* st = storage()) {
      for (const auto& s : st->ListAll()) {
        list.Append(WorkflowSummary(s));
      }
    }
    base::DictValue out;
    out.Set("workflows", std::move(list));
    ResolveDict(args, std::move(out));
  }

  void HandleGetWorkflow(const base::ListValue& args) {
    AllowJavascript();
    base::DictValue out;
    if (args.size() >= 2 && args[1].is_string()) {
      if (auto* st = storage()) {
        if (auto s = st->Load(args[1].GetString())) {
          out = s->ToJSON();
          // The UI reads {{variable}} defaults under "variables"; expose an
          // alias so it round-trips (the Script model stores them under
          // "default_variables").
          if (const base::DictValue* dv = out.FindDict("default_variables")) {
            out.Set("variables", dv->Clone());
          }
        }
      }
    }
    ResolveDict(args, std::move(out));
  }

  void HandleSaveWorkflow(const base::ListValue& args) {
    AllowJavascript();
    base::DictValue out;
    if (args.size() >= 2 && args[1].is_dict()) {
      base::DictValue in = args[1].GetDict().Clone();
      // The UI sends variable defaults under "variables"; Script::FromJSON
      // reads "default_variables" — copy so they persist.
      if (const base::DictValue* vars = in.FindDict("variables")) {
        if (!in.FindDict("default_variables")) {
          in.Set("default_variables", vars->Clone());
        }
      }
      // Assign an id for brand-new (recorded / blank) workflows.
      const std::string* existing = in.FindString("id");
      if (!existing || existing->empty()) {
        in.Set("id", NewWorkflowId());
      }
      if (auto s = Script::FromJSON(in)) {
        if (s->created_at_unix == 0) {
          s->created_at_unix = static_cast<int64_t>(std::time(nullptr));
        }
        if (auto* st = storage()) {
          st->Save(*s);
          out.Set("id", s->id);
        }
      }
    }
    ResolveDict(args, std::move(out));
  }

  void HandleDeleteWorkflow(const base::ListValue& args) {
    AllowJavascript();
    base::DictValue out;
    bool ok = false;
    if (args.size() >= 2 && args[1].is_string()) {
      if (auto* st = storage()) {
        ok = st->Delete(args[1].GetString());
      }
    }
    out.Set("ok", ok);
    ResolveDict(args, std::move(out));
  }

  void HandleDuplicateWorkflow(const base::ListValue& args) {
    AllowJavascript();
    base::DictValue out;
    if (args.size() >= 3 && args[1].is_string() && args[2].is_string()) {
      if (auto* st = storage()) {
        if (auto s = st->Load(args[1].GetString())) {
          s->id = NewWorkflowId();
          s->name = args[2].GetString();
          s->created_at_unix = static_cast<int64_t>(std::time(nullptr));
          s->stats = molt_ai::automation::Stats();  // fresh history
          s->trigger = Trigger();                    // copy starts manual
          st->Save(*s);
          out.Set("id", s->id);
        }
      }
    }
    ResolveDict(args, std::move(out));
  }

  void HandleRunWorkflow(const base::ListValue& args) {
    AllowJavascript();
    base::DictValue out;
    bool ok = false;
    if (args.size() >= 2 && args[1].is_string()) {
      const std::string id = args[1].GetString();
      if (auto* st = storage()) {
        if (auto s = st->Load(id)) {
          std::map<std::string, std::string> overrides;
          if (args.size() >= 3 && args[2].is_dict()) {
            for (const auto kv : args[2].GetDict()) {
              if (kv.second.is_string()) {
                overrides[kv.first] = kv.second.GetString();
              }
            }
          }
          const int64_t before = s->stats.last_run_unix;
          // Foreground (minimize=false) so the user can watch the replay.
          RunScriptInBackgroundBrowser(profile(), *s, /*minimize=*/false,
                                       /*start_index=*/0, overrides);
          ok = true;
          // The background run writes its result to disk on finish; poll for
          // it so we can fire 'run-complete' back to the panel. (Per-step
          // 'run-progress' is future work — the popup window shows live steps.)
          PollRunResult(id, before, /*attempts=*/80);
        }
      }
    }
    out.Set("ok", ok);
    ResolveDict(args, std::move(out));
  }

  void PollRunResult(const std::string& id, int64_t before, int attempts) {
    if (!IsJavascriptAllowed() || attempts <= 0) {
      return;
    }
    auto* st = storage();
    if (auto s = st ? st->Load(id) : std::nullopt) {
      if (s->stats.last_run_unix > before) {
        base::DictValue d;
        d.Set("id", id);
        d.Set("ok", s->stats.last_failed_step_index < 0);
        d.Set("message", s->stats.last_result.empty() ? "Finished"
                                                       : s->stats.last_result);
        FireWebUIListener("run-complete", base::Value(std::move(d)));
        return;
      }
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AgentStudioHandler::PollRunResult,
                       weak_factory_.GetWeakPtr(), id, before, attempts - 1),
        base::Milliseconds(1500));
  }

  void HandleStartRecording(const base::ListValue& args) {
    AllowJavascript();
    content::WebContents* tab = ActiveTab();
    if (!tab) {
      base::DictValue st;
      st.Set("recording", false);
      st.Set("count", 0);
      st.Set("error", "no active tab");
      FireWebUIListener("recorder-state", base::Value(std::move(st)));
      return;
    }
    AutomationRecorderTabHelper::CreateForWebContents(tab);
    auto* helper = AutomationRecorderTabHelper::FromWebContents(tab);
    if (helper) {
      helper->StartRecording(base::BindRepeating(
          &AgentStudioHandler::OnRecorderStep, weak_factory_.GetWeakPtr()));
    }
    base::DictValue st;
    st.Set("recording", true);
    st.Set("count", 0);
    FireWebUIListener("recorder-state", base::Value(std::move(st)));
  }

  void HandleStopRecording(const base::ListValue& args) {
    AllowJavascript();
    base::DictValue out;
    content::WebContents* tab =
        AutomationRecorderTabHelper::GetActivelyRecordingContents();
    if (tab) {
      if (auto* helper =
              AutomationRecorderTabHelper::FromWebContents(tab)) {
        Script draft = helper->StopRecording();
        if (draft.name.empty()) {
          draft.name = "New workflow";
        }
        out.Set("draft", draft.ToJSON());
      }
    }
    base::DictValue st;
    st.Set("recording", false);
    st.Set("count", 0);
    FireWebUIListener("recorder-state", base::Value(std::move(st)));
    ResolveDict(args, std::move(out));
  }

  void HandleSaveSchedule(const base::ListValue& args) {
    AllowJavascript();
    base::DictValue out;
    out.Set("ok", false);
    if (args.size() >= 3 && args[1].is_string() && args[2].is_dict()) {
      if (auto* st = storage()) {
        if (auto s = st->Load(args[1].GetString())) {
          const base::DictValue& t = args[2].GetDict();
          const std::string* type = t.FindString("type");
          const std::string* expr = t.FindString("expression");
          Trigger trig;
          if (type && *type == "interval") {
            trig.type = TriggerType::INTERVAL;
            // UI sends "<n>m" / "<n>h"; the engine wants seconds.
            int secs = 3600;
            std::string e = expr ? *expr : "60m";
            if (!e.empty() && (e.back() == 'm' || e.back() == 'h')) {
              char unit = e.back();
              int n = 0;
              base::StringToInt(e.substr(0, e.size() - 1), &n);
              secs = (unit == 'h') ? n * 3600 : n * 60;
            } else {
              base::StringToInt(e, &secs);
            }
            if (secs < 60) {
              secs = 60;
            }
            trig.expression = base::NumberToString(secs);
          } else if (type && *type == "daily" && expr) {
            // "HH:MM" -> cron "MM HH * * *".
            std::string hh = expr->substr(0, expr->find(':'));
            std::string mm = expr->substr(expr->find(':') + 1);
            trig.type = TriggerType::CRON;
            trig.expression = mm + " " + hh + " * * *";
          } else if (type && *type == "cron") {
            trig.type = TriggerType::CRON;
            trig.expression = expr ? *expr : "";
          } else {
            trig.type = TriggerType::MANUAL;
          }
          s->trigger = trig;
          s->enabled = trig.type != TriggerType::MANUAL;
          st->Save(*s);
          if (Profile* p = profile()) {
            if (auto* svc =
                    AutomationSchedulerServiceFactory::GetForProfile(p)) {
              if (svc->scheduler()) {
                svc->scheduler()->Reschedule();  // pick up immediately
              }
            }
          }
          base::Time next =
              AutomationScheduler::NextFireTime(s->trigger, base::Time::Now());
          out.Set("ok", true);
          out.Set("nextFire",
                  next.is_null() ? 0.0 : next.InSecondsFSinceUnixEpoch());
        }
      }
    }
    ResolveDict(args, std::move(out));
  }

  void HandleCancelRun(const base::ListValue& args) {
    AllowJavascript();
    // Background runs self-close on completion; there is no cross-process
    // cancel handle yet. Acknowledge so the UI can reset its state.
    base::DictValue out;
    out.Set("ok", true);
    ResolveDict(args, std::move(out));
  }

  // Live recorder callback — fires for every captured step.
  void OnRecorderStep(const Step& s, int total) {
    if (!IsJavascriptAllowed()) {
      return;
    }
    FireWebUIListener("recorder-step",
                      base::Value(RecorderStepDict(s, total)));
    base::DictValue st;
    st.Set("recording", true);
    st.Set("count", total);
    FireWebUIListener("recorder-state", base::Value(std::move(st)));
  }

  base::WeakPtrFactory<AgentStudioHandler> weak_factory_{this};
};

// ---------------------------------------------------------------------------
// Data source — serves the studio HTML with a relaxed CSP (inline script/style
// + no trusted-types, matching the other molt pages).
// ---------------------------------------------------------------------------
class MoltAIAgentDataSource : public content::URLDataSource {
 public:
  MoltAIAgentDataSource() = default;
  ~MoltAIAgentDataSource() override = default;

  std::string GetSource() override { return "molt-ai-agent"; }
  std::string GetMimeType(const GURL& url) override { return "text/html"; }
  bool ShouldServeMimeTypeAsContentTypeHeader() override { return true; }

  std::string GetContentSecurityPolicy(
      network::mojom::CSPDirectiveName directive) override {
    switch (directive) {
      case network::mojom::CSPDirectiveName::ScriptSrc:
        return "script-src chrome://resources 'self' 'unsafe-inline';";
      case network::mojom::CSPDirectiveName::StyleSrc:
        return "style-src 'self' 'unsafe-inline';";
      case network::mojom::CSPDirectiveName::TrustedTypes:
        return std::string();
      case network::mojom::CSPDirectiveName::RequireTrustedTypesFor:
        return std::string();
      default:
        return content::URLDataSource::GetContentSecurityPolicy(directive);
    }
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    std::string html = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Agent mode</title>
<style>
/* ==========================================================================
   MoltBrowser — Agent mode automation studio (480px side panel)
   Dark liquid-glass aesthetic, matching molt-net / molt-ai-chat.
   Semantic status colors are kept distinct from the brand accent:
     ok = green, fail = red, scheduled = amber, brand = coral, AI = violet.
   ========================================================================== */
:root{
  --bg:#0a0b10;
  --bg-grad-a:#141232;
  --bg-grad-b:#0a0b10;
  --bg-grad-c:#0f0b1c;
  --surface:rgba(255,255,255,0.045);
  --surface2:rgba(255,255,255,0.075);
  --surface3:rgba(255,255,255,0.11);
  --text:#f4f5fa;
  --muted:rgba(233,236,247,0.64);
  --faint:rgba(233,236,247,0.40);
  --dim:rgba(233,236,247,0.26);
  --border:rgba(255,255,255,0.11);
  --border-hi:rgba(255,255,255,0.22);
  --edge-soft:rgba(255,255,255,0.06);
  --specular:rgba(255,255,255,0.26);
  --accent:#ff5257;          /* brand coral */
  --accent-2:#ff7a54;
  --accent-deep:#e0353b;
  --violet:#a78bfa;          /* AI / secondary */
  --violet-2:#8ea2ff;
  --ok:#5fe3a1;              /* success */
  --ok-deep:#2fae74;
  --warn:#e6b455;            /* scheduled / amber */
  --err:#f0736f;            /* failure */
  --glass:rgba(18,20,30,0.55);
  --glass-strong:rgba(22,24,36,0.72);
  --blur:blur(30px) saturate(1.7);
  --shadow:0 22px 52px -16px rgba(0,0,0,0.7),inset 0 1px 0 var(--specular);
  --shadow-sm:0 10px 26px -12px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.20);
  --font:-apple-system,"SF Pro Text",BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
  --mono:"SF Mono",ui-monospace,"JetBrains Mono",Menlo,monospace;
  --r:14px;
}
*{margin:0;padding:0;box-sizing:border-box}
html{
  background:
    radial-gradient(120% 80% at 82% -6%,var(--bg-grad-a),transparent 58%),
    radial-gradient(120% 90% at 8% 108%,var(--bg-grad-c),transparent 60%),
    var(--bg-grad-b);
  background-color:var(--bg);
}
html,body{height:100%}
body{
  font-family:var(--font);color:var(--text);background:transparent;
  -webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility;
  width:480px;max-width:100vw;height:100vh;margin:0 auto;position:relative;overflow:hidden;
}
button,input,textarea,select{font-family:inherit}
::selection{background:rgba(167,139,250,0.35)}

/* ---- ambient orbs (glass substrate) ---- */
.ambient{position:fixed;inset:-12%;z-index:0;pointer-events:none;overflow:hidden;opacity:0.85}
.ambient span{position:absolute;border-radius:50%;filter:blur(80px);will-change:transform}
.ambient .o1{width:62%;height:62%;top:-16%;left:-12%;background:radial-gradient(circle at 34% 34%,#4a2f8f,#241645 66%,transparent 74%);animation:drift1 36s ease-in-out infinite alternate}
.ambient .o2{width:54%;height:54%;bottom:-16%;right:-14%;background:radial-gradient(circle at 60% 40%,#c0303f,#7a2036 62%,transparent 74%);animation:drift2 42s ease-in-out infinite alternate}
.ambient .o3{width:50%;height:50%;top:38%;left:26%;background:radial-gradient(circle at 44% 56%,#159aad,#0f5f6e 60%,transparent 74%);box-shadow:0 0 220px 60px rgba(91,58,134,0.5);animation:drift3 48s ease-in-out infinite alternate}
@keyframes drift1{to{transform:translate3d(8%,7%,0)}}
@keyframes drift2{to{transform:translate3d(-8%,-6%,0)}}
@keyframes drift3{to{transform:translate3d(5%,-8%,0) scale(1.08)}}

@keyframes recpulse{0%,100%{box-shadow:0 0 0 0 rgba(255,82,87,0.55)}70%{box-shadow:0 0 0 12px rgba(255,82,87,0)}}
@keyframes dotpulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.45;transform:scale(.82)}}
@keyframes spin{to{transform:rotate(360deg)}}
@keyframes rise{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
@keyframes sheetin{from{transform:translateY(18px);opacity:0}to{transform:none;opacity:1}}

/* ---- shell ---- */
.app{position:relative;z-index:1;height:100%;display:flex;flex-direction:column}
.topbar{
  flex:0 0 auto;height:54px;padding:0 14px;display:flex;align-items:center;gap:11px;
  border-bottom:1px solid var(--edge-soft);
  background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  box-shadow:inset 0 1px 0 var(--specular);position:relative;z-index:6;
}
.brand-tile{width:28px;height:28px;border-radius:9px;flex:0 0 auto;
  background:conic-gradient(from 200deg,#ff5257,#a78bfa,#8ea2ff,#ff5257);
  box-shadow:0 0 14px rgba(167,139,250,0.5),inset 0 1px 0 rgba(255,255,255,0.4);
  display:flex;align-items:center;justify-content:center;font-size:15px}
.brand-txt{display:flex;flex-direction:column;line-height:1.15;min-width:0}
.brand-txt .t{font-size:15px;font-weight:700;letter-spacing:-.01em}
.brand-txt .s{font-size:11px;color:var(--faint);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.topbar .back{margin-right:2px;width:30px;height:30px;border-radius:9px;border:1px solid transparent;background:none;color:var(--muted);
  font-size:17px;cursor:pointer;display:none;align-items:center;justify-content:center;transition:.15s;flex:0 0 auto}
.topbar .back:hover{background:var(--surface2);color:var(--text)}
.topbar.sub .back{display:flex}
.topbar-actions{margin-left:auto;display:flex;gap:4px;align-items:center}

/* ---- viewport / views ---- */
.viewport{flex:1 1 auto;position:relative;overflow:hidden}
.view{position:absolute;inset:0;display:flex;flex-direction:column;overflow:hidden;
  opacity:0;pointer-events:none;transform:translateY(6px);transition:opacity .2s,transform .2s}
.view.active{opacity:1;pointer-events:auto;transform:none}
.scroll{flex:1 1 auto;overflow-y:auto;padding:14px 14px 20px}
.scroll::-webkit-scrollbar{width:9px}
.scroll::-webkit-scrollbar-thumb{background:rgba(255,255,255,.13);border-radius:5px;border:2px solid transparent;background-clip:content-box}
.scroll::-webkit-scrollbar-thumb:hover{background:rgba(255,255,255,.22);background-clip:content-box}

/* ---- generic buttons ---- */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;border:1px solid var(--border);
  background:var(--surface2);color:var(--text);border-radius:12px;padding:10px 14px;font-size:13px;font-weight:600;
  cursor:pointer;transition:background .15s,border-color .15s,transform .06s,box-shadow .15s;white-space:nowrap;user-select:none}
.btn:hover{background:var(--surface3);border-color:var(--border-hi)}
.btn:active{transform:translateY(1px)}
.btn.ghost{background:none;border-color:transparent;color:var(--muted)}
.btn.ghost:hover{background:var(--surface2);color:var(--text)}
.btn.primary{background:linear-gradient(180deg,var(--accent-2),var(--accent-deep));border-color:transparent;color:#fff;
  box-shadow:0 10px 24px -10px rgba(255,82,87,0.6),inset 0 1px 0 rgba(255,255,255,0.25)}
.btn.primary:hover{filter:brightness(1.06)}
.btn.violet{background:linear-gradient(180deg,#b79bff,#7c5cff);border-color:transparent;color:#fff;
  box-shadow:0 10px 24px -10px rgba(124,92,255,0.6),inset 0 1px 0 rgba(255,255,255,0.25)}
.btn.ok{background:linear-gradient(180deg,#67e7ab,var(--ok-deep));border-color:transparent;color:#062117;
  box-shadow:0 10px 24px -10px rgba(95,227,161,0.55)}
.btn.danger{color:var(--err);border-color:rgba(240,115,111,0.35);background:rgba(240,115,111,0.08)}
.btn.danger:hover{background:rgba(240,115,111,0.16)}
.btn.block{width:100%}
.btn.lg{padding:14px 16px;font-size:14px;border-radius:14px}
.btn:disabled{opacity:.5;cursor:default;pointer-events:none}

/* ---- LIBRARY ---- */
.lib-head{padding:16px 14px 6px}
.lib-title{font-size:22px;font-weight:750;letter-spacing:-.02em}
.lib-sub{font-size:12.5px;color:var(--muted);margin-top:3px}
.lib-actions{display:flex;flex-direction:column;gap:9px;padding:12px 14px 6px}
.lib-actions .row2{display:flex;gap:9px}
.lib-actions .row2 .btn{flex:1}
.rec-btn{position:relative}
.rec-btn .rd{width:11px;height:11px;border-radius:50%;background:#fff;box-shadow:0 0 0 3px rgba(255,255,255,0.25)}
.sec-label{font-size:10.5px;font-weight:700;letter-spacing:.14em;text-transform:uppercase;color:var(--faint);padding:16px 4px 2px}

.card{position:relative;background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  border:1px solid var(--border);border-radius:16px;padding:13px 13px 11px;margin-bottom:11px;box-shadow:var(--shadow-sm);
  animation:rise .22s ease both}
.card .cname{font-size:14.5px;font-weight:700;letter-spacing:-.01em;padding-right:26px;word-break:break-word}
.card .csub{display:flex;align-items:center;gap:6px;flex-wrap:wrap;font-size:11.5px;color:var(--muted);margin-top:4px}
.card .csub .dotsep{color:var(--dim)}
.stat{display:inline-flex;align-items:center;gap:4px;font-weight:600}
.stat.ok{color:var(--ok)} .stat.fail{color:var(--err)} .stat.never{color:var(--faint);font-weight:500}
.chip{display:inline-flex;align-items:center;gap:5px;font-size:11px;font-weight:600;border-radius:999px;padding:4px 9px;margin-top:8px;
  background:rgba(230,180,85,0.12);color:var(--warn);border:1px solid rgba(230,180,85,0.28)}
.chip .cg{font-size:12px}
.card-actions{display:flex;gap:5px;margin-top:11px;padding-top:10px;border-top:1px solid var(--edge-soft)}
.mini{display:inline-flex;align-items:center;gap:5px;background:none;border:1px solid transparent;color:var(--muted);
  border-radius:9px;padding:6px 9px;font-size:12px;font-weight:600;cursor:pointer;transition:.14s}
.mini:hover{background:var(--surface2);color:var(--text)}
.mini.run:hover{color:var(--ok);background:rgba(95,227,161,0.1)}
.mini.grow{margin-right:auto}
.kebab{width:30px;justify-content:center;padding:6px 0}
.card-menu{position:absolute;top:38px;right:10px;min-width:150px;z-index:20;
  background:var(--glass-strong);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  border:1px solid var(--border-hi);border-radius:12px;padding:5px;box-shadow:var(--shadow);display:none;animation:rise .12s ease both}
.card-menu.open{display:block}
.menu-item{display:flex;align-items:center;gap:9px;padding:8px 10px;border-radius:8px;font-size:12.5px;color:var(--text);cursor:pointer;transition:.12s}
.menu-item:hover{background:var(--surface2)}
.menu-item.danger{color:var(--err)}
.menu-item.danger:hover{background:rgba(240,115,111,0.14)}

/* card run ticker */
.card-run{display:none;margin-top:10px;padding-top:10px;border-top:1px solid var(--edge-soft)}
.card-run.on{display:block;animation:rise .18s ease both}
.ticker{display:flex;align-items:center;gap:8px;font-size:11.5px;color:var(--muted)}
.ticker .spin{width:12px;height:12px;border-radius:50%;border:2px solid rgba(255,255,255,0.2);border-top-color:var(--violet);animation:spin .7s linear infinite;flex:0 0 auto}
.ticker .tmsg{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1}
.ticker .tct{font-variant-numeric:tabular-nums;color:var(--faint);flex:0 0 auto}
.ticker .tstop{flex:0 0 auto;background:none;border:1px solid var(--border);color:var(--muted);border-radius:7px;
  font-size:10px;line-height:1;padding:4px 7px;cursor:pointer;transition:.14s}
.ticker .tstop:hover{color:var(--err);border-color:rgba(240,115,111,0.45);background:rgba(240,115,111,0.1)}
.pbar{height:5px;border-radius:3px;background:rgba(255,255,255,0.08);margin-top:8px;overflow:hidden}
.pbar i{display:block;height:100%;width:0;border-radius:3px;background:linear-gradient(90deg,var(--violet),var(--violet-2));transition:width .3s ease}
.run-done{display:flex;align-items:center;gap:7px;font-size:12px;font-weight:600;margin-top:9px}
.run-done.ok{color:var(--ok)} .run-done.fail{color:var(--err)}

/* empty state */
.empty{text-align:center;padding:40px 22px;animation:rise .3s ease both}
.empty .ico{font-size:44px;filter:saturate(1.1)}
.empty .eh{font-size:16px;font-weight:700;margin-top:12px}
.empty .ep{font-size:12.5px;color:var(--muted);margin-top:6px;line-height:1.5}

/* ---- RECORDING ---- */
.rec-banner{margin:14px 14px 0;padding:14px 15px;border-radius:16px;display:flex;align-items:center;gap:12px;
  background:linear-gradient(180deg,rgba(255,82,87,0.18),rgba(224,53,59,0.08));border:1px solid rgba(255,82,87,0.4);
  box-shadow:inset 0 1px 0 rgba(255,255,255,0.14)}
.rec-banner .rdot{width:13px;height:13px;border-radius:50%;background:var(--accent);flex:0 0 auto;animation:recpulse 1.6s ease-out infinite}
.rec-banner .rl{flex:1;min-width:0}
.rec-banner .rl .a{font-size:14px;font-weight:700}
.rec-banner .rl .b{font-size:11.5px;color:var(--muted);margin-top:2px}
.rec-banner .rl .b b{color:var(--text);font-variant-numeric:tabular-nums}
.rec-cta{padding:12px 14px 0;display:flex;gap:9px}
.rec-cta .btn{flex:1}
.hint{margin:12px 14px 0;padding:12px 14px;border-radius:14px;background:var(--surface);border:1px solid var(--border);
  font-size:12.5px;color:var(--muted);line-height:1.5;display:flex;gap:10px}
.hint .hi{font-size:18px;flex:0 0 auto}
.rec-list{padding:14px 14px 0}
.rstep{display:flex;align-items:flex-start;gap:11px;padding:11px 11px;border-radius:13px;margin-bottom:9px;
  background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);border:1px solid var(--border);
  box-shadow:var(--shadow-sm);animation:rise .2s ease both}
.rstep .num{width:22px;height:22px;border-radius:7px;background:var(--surface3);color:var(--muted);font-size:11px;font-weight:700;
  display:flex;align-items:center;justify-content:center;flex:0 0 auto;font-variant-numeric:tabular-nums}
.rstep .em{font-size:17px;flex:0 0 auto;line-height:1.3}
.rstep .rc{flex:1;min-width:0}
.rstep .rt{font-size:13px;font-weight:600;line-height:1.3;word-break:break-word}
.rstep .rd{font-size:11px;color:var(--faint);margin-top:2px;font-family:var(--mono);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.rstep .drop{background:none;border:none;color:var(--dim);font-size:15px;cursor:pointer;padding:2px 4px;border-radius:6px;transition:.14s;flex:0 0 auto}
.rstep .drop:hover{color:var(--err);background:rgba(240,115,111,0.12)}
.rec-idle{text-align:center;color:var(--faint);font-size:12px;padding:24px 10px}
.rec-idle .waves{display:inline-flex;gap:4px;margin-bottom:10px}
.rec-idle .waves i{width:4px;height:14px;border-radius:2px;background:var(--violet);animation:dotpulse 1.2s ease-in-out infinite}
.rec-idle .waves i:nth-child(2){animation-delay:.15s;background:var(--accent)}
.rec-idle .waves i:nth-child(3){animation-delay:.3s;background:var(--violet-2)}

/* ---- EDITOR ---- */
.ed-scroll{flex:1 1 auto;overflow-y:auto;padding:14px 14px 16px}
.ed-name-wrap{margin-bottom:12px}
.ed-name{width:100%;background:var(--surface);border:1px solid var(--border);border-radius:13px;padding:12px 14px;
  color:var(--text);font-size:16px;font-weight:700;outline:none;transition:border-color .15s;letter-spacing:-.01em}
.ed-name:focus{border-color:var(--violet)}
.ed-meta{font-size:11.5px;color:var(--faint);margin:8px 2px 4px;display:flex;gap:6px;flex-wrap:wrap;align-items:center}
.estep{position:relative;background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  border:1px solid var(--border);border-radius:14px;padding:12px 12px 11px;margin-bottom:10px;box-shadow:var(--shadow-sm);animation:rise .18s ease both}
.estep .ehead{display:flex;align-items:center;gap:9px}
.estep .en{width:22px;height:22px;border-radius:7px;background:var(--surface3);color:var(--muted);font-size:11px;font-weight:700;
  display:flex;align-items:center;justify-content:center;flex:0 0 auto;font-variant-numeric:tabular-nums}
.estep .eem{font-size:17px;flex:0 0 auto}
.estep .etype{font-size:13px;font-weight:700}
.estep .etype small{font-weight:500;color:var(--faint);font-size:11px;margin-left:5px}
.estep .ectrls{margin-left:auto;display:flex;gap:2px}
.iconbtn{width:26px;height:26px;border-radius:7px;border:none;background:none;color:var(--faint);font-size:14px;cursor:pointer;
  display:flex;align-items:center;justify-content:center;transition:.13s}
.iconbtn:hover{background:var(--surface2);color:var(--text)}
.iconbtn.del:hover{color:var(--err);background:rgba(240,115,111,0.12)}
.iconbtn:disabled{opacity:.28;pointer-events:none}
.efield{margin-top:9px}
.efield label{display:block;font-size:10.5px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--faint);margin-bottom:5px}
.inp{width:100%;background:rgba(0,0,0,0.22);border:1px solid var(--border);border-radius:10px;padding:9px 11px;color:var(--text);
  font-size:13px;outline:none;transition:border-color .15s}
.inp:focus{border-color:var(--violet)}
.inp::placeholder{color:var(--dim)}
textarea.inp{resize:vertical;min-height:52px;line-height:1.45;font-family:inherit}
.inp.mono{font-family:var(--mono);font-size:12px}
.subfields{display:flex;gap:8px}
.subfields .efield{flex:1;margin-top:9px}
.varrow{display:flex;align-items:center;gap:8px;margin-top:9px;flex-wrap:wrap}
.vartog{display:inline-flex;align-items:center;gap:7px;font-size:11.5px;color:var(--muted);cursor:pointer;user-select:none;
  border:1px dashed var(--border-hi);border-radius:9px;padding:6px 10px;transition:.14s;background:none}
.vartog:hover{color:var(--text);border-color:var(--violet);background:rgba(167,139,250,0.08)}
.vartog.on{border-style:solid;border-color:rgba(167,139,250,0.5);background:rgba(167,139,250,0.14);color:var(--violet)}
.varpill{display:inline-flex;align-items:center;gap:5px;font-family:var(--mono);font-size:11.5px;font-weight:600;color:var(--violet);
  background:rgba(167,139,250,0.16);border:1px solid rgba(167,139,250,0.4);border-radius:8px;padding:4px 9px}
.varedit{display:flex;gap:8px;margin-top:9px;padding:10px;border-radius:11px;background:rgba(167,139,250,0.07);border:1px solid rgba(167,139,250,0.25)}
.varedit .efield{margin-top:0;flex:1}
.varedit .efield label{color:var(--violet)}
.varedit .inp{background:rgba(0,0,0,0.28)}

.addwrap{position:relative;margin-top:4px}
.addbtn{width:100%;border:1.5px dashed var(--border-hi);background:none;color:var(--muted);border-radius:13px;padding:12px;
  font-size:13px;font-weight:600;cursor:pointer;transition:.15s;display:flex;align-items:center;justify-content:center;gap:8px}
.addbtn:hover{border-color:var(--violet);color:var(--text);background:rgba(167,139,250,0.06)}
.addmenu{position:absolute;left:0;right:0;bottom:calc(100% + 6px);z-index:25;
  background:var(--glass-strong);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  border:1px solid var(--border-hi);border-radius:14px;padding:6px;box-shadow:var(--shadow);display:none;
  max-height:280px;overflow-y:auto;animation:sheetin .16s ease both}
.addmenu.open{display:block}
.addmenu .menu-item{font-size:13px;padding:9px 11px}
.addmenu .menu-item .em{font-size:16px;width:20px;text-align:center}
.addmenu .grp{font-size:9.5px;font-weight:700;letter-spacing:.12em;text-transform:uppercase;color:var(--faint);padding:8px 10px 3px}

/* editor sticky bar */
.ed-bar{flex:0 0 auto;display:flex;gap:9px;padding:11px 14px;border-top:1px solid var(--edge-soft);
  background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);box-shadow:inset 0 1px 0 var(--specular)}
.ed-bar .btn{flex:1}
.ed-bar .btn.save{flex:1.2}

/* ---- SHEETS (bottom) ---- */
.scrim{position:fixed;inset:0;z-index:40;background:rgba(4,5,10,0.55);-webkit-backdrop-filter:blur(3px);backdrop-filter:blur(3px);
  opacity:0;pointer-events:none;transition:opacity .18s;display:flex;align-items:flex-end;justify-content:center}
.scrim.open{opacity:1;pointer-events:auto}
.sheet{width:100%;max-height:88%;display:flex;flex-direction:column;
  background:var(--glass-strong);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  border-top:1px solid var(--border-hi);border-radius:22px 22px 0 0;box-shadow:0 -20px 60px -18px rgba(0,0,0,0.7),inset 0 1px 0 var(--specular);
  transform:translateY(100%);transition:transform .24s cubic-bezier(.22,1,.36,1);overflow:hidden}
.scrim.open .sheet{transform:none}
.sheet .grab{width:38px;height:4px;border-radius:2px;background:rgba(255,255,255,0.22);margin:9px auto 3px;flex:0 0 auto}
.sheet-head{padding:8px 18px 4px;flex:0 0 auto}
.sheet-head .sh-t{font-size:17px;font-weight:750;letter-spacing:-.01em}
.sheet-head .sh-s{font-size:12.5px;color:var(--muted);margin-top:3px}
.sheet-body{padding:14px 18px 6px;overflow-y:auto;flex:1 1 auto}
.sheet-foot{padding:12px 18px calc(14px + env(safe-area-inset-bottom));flex:0 0 auto;display:flex;gap:9px;border-top:1px solid var(--edge-soft)}
.sheet-foot .btn{flex:1}

/* run sheet var inputs */
.vfield{margin-bottom:13px}
.vfield label{display:flex;align-items:center;gap:6px;font-size:12.5px;font-weight:600;margin-bottom:6px}
.vfield label .vp{font-family:var(--mono);font-size:11px;color:var(--violet);background:rgba(167,139,250,0.14);
  border:1px solid rgba(167,139,250,0.35);border-radius:6px;padding:1px 6px}
.run-live{margin-top:6px;padding:13px;border-radius:14px;background:rgba(0,0,0,0.24);border:1px solid var(--border)}
.run-live .rl-h{font-size:11px;font-weight:700;letter-spacing:.1em;text-transform:uppercase;color:var(--faint);margin-bottom:9px}
.novar{text-align:center;color:var(--muted);font-size:13px;padding:6px 0 12px}

/* schedule sheet */
.opt{display:flex;align-items:flex-start;gap:12px;padding:13px;border-radius:13px;border:1px solid var(--border);
  background:var(--surface);margin-bottom:9px;cursor:pointer;transition:.15s}
.opt:hover{border-color:var(--border-hi);background:var(--surface2)}
.opt.sel{border-color:var(--violet);background:rgba(167,139,250,0.1);box-shadow:0 0 0 1px rgba(167,139,250,0.25)}
.opt .radio{width:19px;height:19px;border-radius:50%;border:2px solid var(--faint);flex:0 0 auto;margin-top:1px;position:relative;transition:.15s}
.opt.sel .radio{border-color:var(--violet)}
.opt.sel .radio::after{content:"";position:absolute;inset:3px;border-radius:50%;background:var(--violet)}
.opt .ob{flex:1;min-width:0}
.opt .ot{font-size:14px;font-weight:650}
.opt .os{font-size:11.5px;color:var(--muted);margin-top:2px}
.opt-inline{display:flex;align-items:center;gap:7px;margin-top:9px;flex-wrap:wrap}
.opt-inline .inp{width:auto}
.opt-inline .num{width:64px;text-align:center}
.opt-inline .tm{width:120px}
.opt-inline select.inp{padding-right:26px}
.next-hint{margin-top:6px;padding:11px 13px;border-radius:12px;background:rgba(230,180,85,0.1);border:1px solid rgba(230,180,85,0.28);
  font-size:12.5px;color:var(--warn);display:flex;align-items:center;gap:8px}
.next-hint b{color:#f5d68a}

/* mini modal (rename / duplicate / delete confirm) */
.mini-modal{width:340px;max-width:calc(100% - 32px);margin:auto;align-self:center;
  background:var(--glass-strong);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  border:1px solid var(--border-hi);border-radius:20px;padding:20px;box-shadow:var(--shadow);
  transform:scale(.94);opacity:0;transition:.2s cubic-bezier(.22,1,.36,1)}
.scrim.center{align-items:center}
.scrim.open .mini-modal{transform:none;opacity:1}
.mini-modal .mm-t{font-size:16px;font-weight:700}
.mini-modal .mm-s{font-size:12.5px;color:var(--muted);margin-top:6px;line-height:1.5}
.mini-modal .inp{margin-top:14px}
.mini-modal .mm-btns{display:flex;gap:9px;margin-top:16px}
.mini-modal .mm-btns .btn{flex:1}

/* toast */
.toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(14px);z-index:60;
  background:var(--glass-strong);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
  border:1px solid var(--border-hi);border-radius:12px;padding:11px 16px;font-size:12.5px;font-weight:600;color:var(--text);
  box-shadow:var(--shadow);opacity:0;pointer-events:none;transition:.24s;max-width:calc(100% - 32px);text-align:center}
.toast.show{opacity:1;transform:translateX(-50%)}
.toast.ok{border-color:rgba(95,227,161,0.4)}
.toast.err{border-color:rgba(240,115,111,0.4);color:var(--err)}

@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style>
</head>
<body>
<div class="ambient"><span class="o1"></span><span class="o2"></span><span class="o3"></span></div>

<div class="app">
  <div class="topbar" id="topbar">
    <button class="back" id="backBtn" title="Back" aria-label="Back">&#8592;</button>
    <div class="brand-tile">&#129302;</div>
    <div class="brand-txt">
      <div class="t">Agent mode</div>
      <div class="s" id="topSub">Record, run &amp; schedule web tasks</div>
    </div>
    <div class="topbar-actions" id="topActions"></div>
  </div>

  <div class="viewport">
    <!-- 1. LIBRARY -->
    <section class="view active" id="view-library">
      <div class="lib-head">
        <div class="lib-title">Your workflows</div>
        <div class="lib-sub" id="libSub">Teach the browser to do a task once &mdash; then let it repeat.</div>
      </div>
      <div class="lib-actions">
        <button class="btn primary lg block rec-btn" id="recordBtn"><span class="rd"></span> Record a workflow</button>
        <div class="row2">
          <button class="btn" id="newBlankBtn">&#43; New blank</button>
          <button class="btn" id="importBtn">&#8615; Import</button>
        </div>
      </div>
      <div class="scroll" id="libList"></div>
    </section>

    <!-- 2. RECORDING -->
    <section class="view" id="view-recording">
      <div class="rec-banner">
        <span class="rdot"></span>
        <div class="rl">
          <div class="a">Recording</div>
          <div class="b">Captured <b id="recCount">0</b> step<span id="recPlural">s</span> so far</div>
        </div>
      </div>
      <div class="rec-cta">
        <button class="btn primary lg grow" id="stopBtn" style="flex:2">&#9632; Stop &amp; review</button>
        <button class="btn ghost" id="discardBtn">Discard</button>
      </div>
      <div class="hint">
        <span class="hi">&#128072;</span>
        <div>Do your task on the page to the left &mdash; click, type, scroll, whatever you need. I&rsquo;ll write down every step for you.</div>
      </div>
      <div class="scroll" id="recList" style="padding-top:0"></div>
    </section>

    <!-- 3. EDITOR -->
    <section class="view" id="view-editor">
      <div class="ed-scroll">
        <div class="ed-name-wrap">
          <input class="ed-name" id="edName" placeholder="Name this workflow" spellcheck="false">
          <div class="ed-meta" id="edMeta"></div>
        </div>
        <div id="edSteps"></div>
        <div class="addwrap">
          <div class="addmenu" id="addMenu"></div>
          <button class="addbtn" id="addStepBtn">&#43; Add step</button>
        </div>
      </div>
      <div class="ed-bar">
        <button class="btn ok save" id="edSaveBtn">Save</button>
        <button class="btn violet" id="edRunBtn">&#9654; Run</button>
        <button class="btn" id="edSchedBtn">&#9200; Schedule</button>
      </div>
    </section>
  </div>
</div>

<!-- RUN sheet -->
<div class="scrim" id="runScrim">
  <div class="sheet" id="runSheet">
    <div class="grab"></div>
    <div class="sheet-head"><div class="sh-t" id="runTitle">Run workflow</div><div class="sh-s" id="runSubT">Fill in the details for this run.</div></div>
    <div class="sheet-body" id="runBody"></div>
    <div class="sheet-foot" id="runFoot"></div>
  </div>
</div>

<!-- SCHEDULE sheet -->
<div class="scrim" id="schedScrim">
  <div class="sheet" id="schedSheet">
    <div class="grab"></div>
    <div class="sheet-head"><div class="sh-t">Schedule</div><div class="sh-s" id="schedSubT">Run this on its own, automatically.</div></div>
    <div class="sheet-body" id="schedBody"></div>
    <div class="sheet-foot">
      <button class="btn ghost" id="schedCancel">Cancel</button>
      <button class="btn violet" id="schedSave" style="flex:1.4">Save schedule</button>
    </div>
  </div>
</div>

<!-- MINI modal -->
<div class="scrim center" id="miniScrim">
  <div class="mini-modal" id="miniModal"></div>
</div>

<div class="toast" id="toast"></div>

<script>
/* ============================ chrome.send bridge ============================
   Same shape as molt-net / molt-ai-chat: sendWithPromise generates a callback
   id, sends [cbId, ...args], and resolves on cr.webUIResponse. FireWebUIListener
   arrives via cr.webUIListenerCallback, re-dispatched as DOM CustomEvents so
   cr.addWebUIListener('event', fn) works without chrome://resources/js/cr.js. */
var idCounter=0, pendingCbs={};
function sendWithPromise(method){
  var args=Array.prototype.slice.call(arguments,1);
  var id=method+'_'+(++idCounter);
  return new Promise(function(resolve,reject){
    pendingCbs[id]={resolve:resolve,reject:reject};
    chrome.send(method,[id].concat(args));
  });
}
window.cr=window.cr||{};
cr.webUIResponse=function(id,ok,resp){var c=pendingCbs[id];if(c){delete pendingCbs[id];ok?c.resolve(resp):c.reject(resp);}};
cr.webUIListenerCallback=function(event){
  var detail=arguments.length>1?arguments[1]:undefined;
  document.dispatchEvent(new CustomEvent(event,{detail:detail}));
};
cr.addWebUIListener=cr.addWebUIListener||function(event,fn){
  document.addEventListener(event,function(e){fn(e.detail);});
};

/* ================================ helpers ================================ */
function $(id){return document.getElementById(id);}
function h(tag,attrs,kids){
  var e=document.createElement(tag);
  if(attrs)for(var k in attrs){
    var v=attrs[k];
    if(v==null)continue;
    if(k==='class')e.className=v;
    else if(k==='text')e.textContent=v;
    else if(k==='html')e.innerHTML=v;              // constant strings only
    else if(k.slice(0,2)==='on')e.addEventListener(k.slice(2).toLowerCase(),v);
    else if(typeof v==='boolean'){if(v)e.setAttribute(k,'');}
    else e.setAttribute(k,v);
  }
  if(kids!=null){
    if(!Array.isArray(kids))kids=[kids];
    kids.forEach(function(c){if(c==null||c===false)return;e.appendChild(typeof c==='object'?c:document.createTextNode(String(c)));});
  }
  return e;
}
function clear(el){while(el.firstChild)el.removeChild(el.firstChild);}

var STEP_EMOJI={navigate:'\u{1F310}',click:'\u{1F446}',type:'⌨️',scroll:'\u{1F4DC}',keypress:'⏎',
  wait:'⏳',wait_for:'⏳',extract:'\u{1F4CB}',ai_decide:'\u{1F9E0}',ai_extract:'\u{1F9E0}',assert:'✅',notify:'\u{1F514}'};
var STEP_LABEL={navigate:'Go to page',click:'Click',type:'Type text',scroll:'Scroll',keypress:'Press key',
  wait:'Wait',wait_for:'Wait for',extract:'Extract',ai_decide:'AI decision',ai_extract:'AI extract',assert:'Check',notify:'Notify'};
function emojiFor(t){return STEP_EMOJI[t]||'⚙️';}
function labelFor(t){return STEP_LABEL[t]||t;}

/* Which field the user primarily edits (and can turn into a variable). */
function primaryOf(t){
  if(t==='navigate'||t==='click'||t==='wait_for'||t==='extract')return 'target';
  return 'value';
}
function fieldLabel(t){
  switch(t){
    case 'navigate':return 'Web address';
    case 'click':return 'What to click';
    case 'type':return 'Text to type';
    case 'scroll':return 'How far';
    case 'keypress':return 'Key';
    case 'wait':return 'Seconds to wait';
    case 'wait_for':return 'Wait until this appears';
    case 'extract':return 'What to grab';
    case 'ai_decide':return 'Ask the AI (yes/no question)';
    case 'ai_extract':return 'Tell the AI what to pull out';
    case 'assert':return 'Must be true';
    case 'notify':return 'Message';
    default:return 'Value';
  }
}
function fieldPlaceholder(t){
  switch(t){
    case 'navigate':return 'https://example.com';
    case 'click':return 'the Search button';
    case 'type':return 'hello world';
    case 'scroll':return 'to the bottom';
    case 'keypress':return 'Enter';
    case 'wait':return '2';
    case 'wait_for':return 'the results list';
    case 'extract':return 'the first price';
    case 'ai_decide':return 'is the price under $50?';
    case 'ai_extract':return 'the product name and price';
    case 'assert':return 'the page says "Success"';
    case 'notify':return 'All done!';
    default:return '';
  }
}
function hasStore(t){return t==='extract'||t==='ai_extract'||t==='ai_decide';}

/* Friendly one-line title for a step. */
function stepTitle(s){
  if(s.description)return s.description;
  var v=s.value||'', t=s.target||'';
  switch(s.type){
    case 'navigate':return 'Go to '+(t||'a page');
    case 'click':return 'Click '+(t||'an element');
    case 'type':return 'Type '+(v?('“'+v+'”'):'text');
    case 'scroll':return 'Scroll '+(v||'the page');
    case 'keypress':return 'Press '+(v||'Enter');
    case 'wait':return 'Wait '+(v||'2')+' second'+((v==='1')?'':'s');
    case 'wait_for':return 'Wait for '+(t||'an element');
    case 'extract':return 'Grab '+(t||'data')+(s.store_as?(' → '+s.store_as):'');
    case 'ai_decide':return v||'Ask the AI a question';
    case 'ai_extract':return 'AI: pull out '+(v||'data');
    case 'assert':return 'Check '+(t||v||'the page');
    case 'notify':return 'Notify: '+(v||'');
    default:return labelFor(s.type);
  }
}
var VAR_RE=/\{\{\s*([A-Za-z0-9_ ]+?)\s*\}\}/g;
function wholeVar(v){var m=/^\{\{\s*([A-Za-z0-9_ ]+?)\s*\}\}$/.exec(v||'');return m?m[1].trim():null;}
function slug(s){return (String(s||'').toLowerCase().replace(/[^a-z0-9]+/g,'_').replace(/^_+|_+$/g,'')||'value').slice(0,24);}

/* Render a possibly variable-laden string into text nodes + violet pills. */
function renderValue(container,val){
  clear(container);
  var s=String(val||''),last=0,m;VAR_RE.lastIndex=0;
  while((m=VAR_RE.exec(s))){
    if(m.index>last)container.appendChild(document.createTextNode(s.slice(last,m.index)));
    container.appendChild(h('span',{'class':'varpill'},['{ }',m[1].trim()]));
    last=m.index+m[0].length;
  }
  if(last<s.length)container.appendChild(document.createTextNode(s.slice(last)));
  if(!s)container.appendChild(document.createTextNode(''));
}

function timeAgo(ts){
  if(!ts)return '';
  var d=Math.floor(Date.now()/1000)-ts;if(d<0)d=0;
  if(d<45)return 'just now';
  var m=Math.round(d/60);if(m<60)return m+'m ago';
  var hh=Math.round(m/60);if(hh<24)return hh+'h ago';
  var dd=Math.round(hh/24);if(dd<7)return dd+'d ago';
  return new Date(ts*1000).toLocaleDateString([],{month:'short',day:'numeric'});
}
function clockTime(ts){return new Date(ts*1000).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'});}
function formatNext(ts){
  if(!ts)return '';
  var now=new Date(),d=new Date(ts*1000);
  var sameDay=now.toDateString()===d.toDateString();
  var tomorrow=new Date(now.getTime()+86400000).toDateString()===d.toDateString();
  var t=clockTime(ts);
  if(sameDay)return 'today '+t;
  if(tomorrow)return 'tomorrow '+t;
  return d.toLocaleDateString([],{month:'short',day:'numeric'})+' '+t;
}

var toastTimer=null;
function toast(msg,kind){
  var t=$('toast');t.textContent=msg;t.className='toast show'+(kind?(' '+kind):'');
  clearTimeout(toastTimer);toastTimer=setTimeout(function(){t.className='toast';},2200);
}

/* ================================ state ================================ */
var view='library';
var recordedSteps=[], droppedIdx={}, recording=false;
var editorState=null;          // {id,name,steps,variables,trigger}
var libraryCards={};           // id -> card element
var activeRun=null;            // {id,stepIndex,total,message,done,ok}
var runSheet={open:false,id:null,name:'',vars:null};
var scheduleCtx={id:null,mode:'manual'};
var openCardMenu=null;

/* ================================ views ================================ */
function showView(v){
  view=v;
  ['library','recording','editor'].forEach(function(name){
    $('view-'+name).classList.toggle('active',name===v);
  });
  var sub=v==='library'?false:true;
  $('topbar').classList.toggle('sub',sub);
  $('topSub').textContent = v==='recording'?'Recording your task…' : v==='editor'?'Editing workflow' : 'Record, run & schedule web tasks';
  closeCardMenu();closeAddMenu();
}
$('backBtn').addEventListener('click',function(){
  if(view==='editor'){showView('library');loadLibrary();}
  else if(view==='recording'){/* backing out of recording = discard */ discardRecording();}
});

/* ============================== LIBRARY ============================== */
function loadLibrary(){
  return sendWithPromise('listWorkflows').then(function(r){
    renderLibrary((r&&r.workflows)||[]);
  }).catch(function(){renderLibrary([]);});
}
function renderLibrary(list){
  var box=$('libList');clear(box);libraryCards={};
  $('libSub').textContent = list.length
    ? (list.length+' workflow'+(list.length===1?'':'s')+' saved')
    : 'Teach the browser to do a task once — then let it repeat.';
  if(!list.length){
    box.appendChild(h('div',{'class':'empty'},[
      h('div',{'class':'ico'},'\u{1F916}'),
      h('div',{'class':'eh'},'No workflows yet'),
      h('div',{'class':'ep'},'Hit “Record a workflow” and do a task on the page — the browser will remember every step and can replay it any time.')
    ]));
    return;
  }
  list.forEach(function(wf){box.appendChild(buildCard(wf));});
  paintRun();
}
function buildCard(wf){
  var sub=h('div',{'class':'csub'});
  sub.appendChild(h('span',{},(wf.stepCount||0)+' step'+((wf.stepCount===1)?'':'s')));
  sub.appendChild(h('span',{'class':'dotsep'},'·'));
  if(!wf.lastRun){
    sub.appendChild(h('span',{'class':'stat never'},'never run'));
  }else if(wf.lastRun.ok){
    sub.appendChild(h('span',{'class':'stat ok'},['ran '+timeAgo(wf.lastRun.ts),' ✓']));
  }else{
    sub.appendChild(h('span',{'class':'stat fail'},['failed '+timeAgo(wf.lastRun.ts),' ✗']));
  }

  var card=h('div',{'class':'card'});
  card.setAttribute('data-wf-id',wf.id);
  card.appendChild(h('div',{'class':'cname'},wf.name||'Untitled workflow'));
  card.appendChild(sub);

  if(wf.schedule){
    card.appendChild(h('div',{'class':'chip'},[
      h('span',{'class':'cg'},'⏰'),
      wf.schedule.label + (wf.schedule.nextFire?('  ·  next '+formatNext(wf.schedule.nextFire)):'')
    ]));
  }

  // run ticker region (hidden until a run targets this card)
  var runReg=h('div',{'class':'card-run'});
  card.appendChild(runReg);

  var actions=h('div',{'class':'card-actions'});
  actions.appendChild(h('button',{'class':'mini run grow',onClick:function(){runWorkflowFlow(wf.id);}},['▶',' Run']));
  actions.appendChild(h('button',{'class':'mini',onClick:function(){openEditor(wf.id);}},['✎',' Edit']));
  actions.appendChild(h('button',{'class':'mini',onClick:function(){openScheduleSheet(wf.id,wf.name);}},['⏰',' Schedule']));
  actions.appendChild(h('button',{'class':'mini kebab',title:'More',onClick:function(e){e.stopPropagation();toggleCardMenu(card,wf);}},'⋯'));
  card.appendChild(actions);

  libraryCards[wf.id]=card;
  return card;
}
function toggleCardMenu(card,wf){
  if(openCardMenu&&openCardMenu.card===card){closeCardMenu();return;}
  closeCardMenu();
  var menu=h('div',{'class':'card-menu open'},[
    h('div',{'class':'menu-item',onClick:function(){closeCardMenu();askDuplicate(wf);}},['⧉',' Duplicate']),
    h('div',{'class':'menu-item',onClick:function(){closeCardMenu();askRename(wf);}},['✎',' Rename']),
    h('div',{'class':'menu-item danger',onClick:function(){closeCardMenu();askDelete(wf);}},['\u{1F5D1}',' Delete'])
  ]);
  card.appendChild(menu);
  // Lift this card above the following cards so the dropdown isn't clipped
  // behind the next workflow; if it's near the bottom, open the menu upward.
  card.style.zIndex='60';
  var cr=card.getBoundingClientRect();
  if(cr.bottom+150>window.innerHeight){menu.style.top='auto';menu.style.bottom='38px';}
  openCardMenu={card:card,menu:menu};
}
function closeCardMenu(){if(openCardMenu){if(openCardMenu.card)openCardMenu.card.style.zIndex='';if(openCardMenu.menu.parentNode)openCardMenu.menu.parentNode.removeChild(openCardMenu.menu);openCardMenu=null;}}
document.addEventListener('click',function(e){
  if(openCardMenu&&!openCardMenu.menu.contains(e.target))closeCardMenu();
  if(addMenuOpen&&!$('addMenu').contains(e.target)&&e.target!==$('addStepBtn'))closeAddMenu();
});

$('recordBtn').addEventListener('click',startRecordingFlow);
$('newBlankBtn').addEventListener('click',function(){
  openEditorDraft({name:'Untitled workflow',steps:[],variables:{},trigger:{type:'manual',expression:''}});
});
$('importBtn').addEventListener('click',function(){$('fileImport').click();});

/* hidden file input for Import (JSON workflow) */
var fileInput=h('input',{type:'file',accept:'.json,application/json',style:'display:none',id:'fileImport',
  onChange:function(e){
    var f=e.target.files&&e.target.files[0];if(!f)return;
    var rd=new FileReader();
    rd.onload=function(){
      try{
        var w=JSON.parse(rd.result);
        openEditorDraft({
          name:(w.name||'Imported workflow'),
          steps:Array.isArray(w.steps)?w.steps:[],
          variables:w.variables||{},
          trigger:w.trigger||{type:'manual',expression:''}
        });
        toast('Workflow imported — review & save','ok');
      }catch(err){toast('That file isn’t a valid workflow','err');}
      e.target.value='';
    };
    rd.readAsText(f);
  }});
document.body.appendChild(fileInput);

/* ============================== RECORDING ============================== */
function startRecordingFlow(){
  recordedSteps=[];droppedIdx={};recording=true;
  renderRecList();updateRecCount(0);
  showView('recording');
  chrome.send('startRecording');   // fire-and-forget per contract
}
function updateRecCount(n){
  $('recCount').textContent=n;
  $('recPlural').textContent=(n===1)?'':'s';
}
function liveCount(){var c=0;recordedSteps.forEach(function(s){if(!droppedIdx[s.index])c++;});return c;}
function renderRecList(){
  var box=$('recList');clear(box);
  var shown=recordedSteps.filter(function(s){return !droppedIdx[s.index];});
  if(!shown.length){
    box.appendChild(h('div',{'class':'rec-idle'},[
      h('div',{'class':'waves'},[h('i'),h('i'),h('i')]),
      h('div',{},'Waiting for your first move…')
    ]));
    return;
  }
  var n=0;
  shown.forEach(function(s){
    n++;
    box.appendChild(h('div',{'class':'rstep'},[
      h('div',{'class':'num'},n),
      h('div',{'class':'em'},s.icon||emojiFor(s.type)),
      h('div',{'class':'rc'},[
        h('div',{'class':'rt'},s.title||stepTitle(s)),
        s.detail?h('div',{'class':'rd'},s.detail):null
      ]),
      h('button',{'class':'drop',title:'Remove this step',onClick:function(){dropStep(s.index);}},'✕')
    ]));
  });
}
function dropStep(index){droppedIdx[index]=true;renderRecList();updateRecCount(liveCount());}

cr.addWebUIListener('recorder-step',function(d){
  if(!d)return;
  recordedSteps.push({index:d.index,type:d.type,icon:d.icon,title:d.title,detail:d.detail,value:d.value});
  renderRecList();updateRecCount(liveCount());
});
cr.addWebUIListener('recorder-state',function(d){
  if(!d)return;recording=!!d.recording;
  if(typeof d.count==='number'&&!recordedSteps.length)updateRecCount(d.count);
});

$('stopBtn').addEventListener('click',function(){
  var btn=$('stopBtn');btn.disabled=true;btn.textContent='Wrapping up…';
  var dropped=Object.assign({},droppedIdx);
  sendWithPromise('stopRecording').then(function(r){
    recording=false;
    var draft=(r&&r.draft)||{name:'Recorded workflow',steps:[],variables:{}};
    var steps=(draft.steps||[]).filter(function(_,i){return !dropped[i];});
    openEditorDraft({
      name:draft.name||'Recorded workflow',
      steps:steps,
      variables:draft.variables||{},
      trigger:{type:'manual',expression:''}
    });
    toast(steps.length+' step'+(steps.length===1?'':'s')+' captured','ok');
  }).catch(function(){
    toast('Couldn’t finish recording','err');showView('library');
  }).then(function(){btn.disabled=false;btn.innerHTML='■ Stop &amp; review';});
});
$('discardBtn').addEventListener('click',discardRecording);
function discardRecording(){
  sendWithPromise('stopRecording').catch(function(){});
  recording=false;recordedSteps=[];droppedIdx={};
  showView('library');loadLibrary();
}

/* ============================== EDITOR ============================== */
function openEditor(id){
  sendWithPromise('getWorkflow',id).then(function(wf){
    openEditorDraft({
      id:wf.id,name:wf.name||'Untitled workflow',
      steps:wf.steps||[],variables:wf.variables||{},
      trigger:wf.trigger||{type:'manual',expression:''},
      stats:wf.stats||null
    });
  }).catch(function(){toast('Couldn’t open workflow','err');});
}
function openEditorDraft(wf){
  editorState={
    id:wf.id||null,name:wf.name||'Untitled workflow',
    steps:(wf.steps||[]).map(function(s){return Object.assign({},s);}),
    variables:Object.assign({},wf.variables||{}),
    trigger:wf.trigger||{type:'manual',expression:''},
    stats:wf.stats||null
  };
  $('edName').value=editorState.name;
  renderEditorMeta();renderEditorSteps();
  showView('editor');
  $('edSteps').parentNode.scrollTop=0;
}
$('edName').addEventListener('input',function(){editorState.name=this.value;});

function renderEditorMeta(){
  var m=$('edMeta');clear(m);
  var st=editorState.stats;
  m.appendChild(h('span',{},editorState.steps.length+' step'+(editorState.steps.length===1?'':'s')));
  if(st&&st.runs){
    m.appendChild(h('span',{'class':'dotsep',style:'color:var(--dim)'},'·'));
    m.appendChild(h('span',{},(st.successes||0)+'/'+st.runs+' successful runs'));
  }
  if(editorState.id==null){
    m.appendChild(h('span',{'class':'dotsep',style:'color:var(--dim)'},'·'));
    m.appendChild(h('span',{style:'color:var(--warn)'},'unsaved'));
  }
}

function renderEditorSteps(){
  var box=$('edSteps');clear(box);
  if(!editorState.steps.length){
    box.appendChild(h('div',{'class':'empty',style:'padding:26px 18px'},[
      h('div',{'class':'ico'},'\u{1F4DD}'),
      h('div',{'class':'eh'},'No steps yet'),
      h('div',{'class':'ep'},'Add a step below, or record one from a live page.')
    ]));
    return;
  }
  editorState.steps.forEach(function(s,i){box.appendChild(buildEditorStep(s,i));});
}

function buildEditorStep(s,i){
  var pf=primaryOf(s.type);
  var pval=s[pf]||'';
  var varName=wholeVar(pval);

  var card=h('div',{'class':'estep'});

  // head
  var ctrls=h('div',{'class':'ectrls'},[
    h('button',{'class':'iconbtn',title:'Move up',disabled:(i===0),onClick:function(){moveStep(i,-1);}},'↑'),
    h('button',{'class':'iconbtn',title:'Move down',disabled:(i===editorState.steps.length-1),onClick:function(){moveStep(i,1);}},'↓'),
    h('button',{'class':'iconbtn del',title:'Delete step',onClick:function(){deleteStep(i);}},'\u{1F5D1}')
  ]);
  card.appendChild(h('div',{'class':'ehead'},[
    h('div',{'class':'en'},i+1),
    h('div',{'class':'eem'},emojiFor(s.type)),
    h('div',{'class':'etype'},[labelFor(s.type),h('small',{},stepTitle(s))]),
    ctrls
  ]));

  // primary field OR variable editor
  if(varName==null){
    var isMulti=(s.type==='ai_decide'||s.type==='ai_extract');
    var inp=h(isMulti?'textarea':'input',{'class':'inp'+(s.type==='click'||s.type==='wait_for'||s.type==='extract'?' mono':''),
      value:pval,placeholder:fieldPlaceholder(s.type),spellcheck:'false',
      onInput:function(){s[pf]=this.value;updateHeadTitle(card,s);}});
    if(s.type==='wait'){inp.setAttribute('type','number');inp.setAttribute('min','0');}
    var fld=h('div',{'class':'efield'},[h('label',{},fieldLabel(s.type)),inp]);
    card.appendChild(fld);
    card.appendChild(h('div',{'class':'varrow'},[
      h('button',{'class':'vartog',onClick:function(){makeVariable(i);}},['✨',' Ask me this each run'])
    ]));
  }else{
    // variable UI
    card.appendChild(h('div',{'class':'varrow'},[
      h('span',{'class':'varpill'},['{ }',varName]),
      h('button',{'class':'vartog on',onClick:function(){unmakeVariable(i);}},['✓',' Asked each run'])
    ]));
    card.appendChild(h('div',{'class':'varedit'},[
      h('div',{'class':'efield'},[
        h('label',{},'Ask as'),
        h('input',{'class':'inp mono',value:varName,spellcheck:'false',
          onChange:function(){renameVariable(i,varName,this.value);}})
      ]),
      h('div',{'class':'efield'},[
        h('label',{},'Default'),
        h('input',{'class':'inp',value:(editorState.variables[varName]||''),placeholder:fieldPlaceholder(s.type),
          onInput:function(){editorState.variables[varName]=this.value;}})
      ])
    ]));
  }

  // store_as for extract / ai_extract / ai_decide
  if(hasStore(s.type)){
    card.appendChild(h('div',{'class':'efield'},[
      h('label',{},'Save answer as'),
      h('input',{'class':'inp mono',value:(s.store_as||''),placeholder:'e.g. price',spellcheck:'false',
        onInput:function(){s.store_as=this.value;}})
    ]));
  }

  // secondary context: selector target when the primary field is the value
  if(pf==='value'&&(s.type==='click'||s.type==='type'||s.type==='extract')&&s.target){
    card.appendChild(h('div',{'class':'efield'},[
      h('label',{},'On element (advanced)'),
      h('input',{'class':'inp mono',value:s.target,spellcheck:'false',onInput:function(){s.target=this.value;}})
    ]));
  }
  return card;
}
function updateHeadTitle(card,s){
  var et=card.querySelector('.etype small');if(et)et.textContent=stepTitle(s);
}
function moveStep(i,dir){
  var j=i+dir;if(j<0||j>=editorState.steps.length)return;
  var t=editorState.steps[i];editorState.steps[i]=editorState.steps[j];editorState.steps[j]=t;
  renderEditorSteps();renderEditorMeta();
}
function deleteStep(i){editorState.steps.splice(i,1);renderEditorSteps();renderEditorMeta();syncVariables();}
function makeVariable(i){
  var s=editorState.steps[i],pf=primaryOf(s.type);
  var base=slug(s.description||stepTitle(s)||labelFor(s.type));
  var name=base,n=2;
  while(editorState.variables.hasOwnProperty(name)&&editorState.variables[name]!==(s[pf]||'')){name=base+'_'+(n++);}
  editorState.variables[name]=s[pf]||'';
  s[pf]='{{'+name+'}}';
  renderEditorSteps();
}
function unmakeVariable(i){
  var s=editorState.steps[i],pf=primaryOf(s.type);
  var name=wholeVar(s[pf]);if(name==null)return;
  s[pf]=editorState.variables[name]||'';
  renderEditorSteps();syncVariables();
}
function renameVariable(i,oldName,raw){
  var newName=slug(raw);if(!newName||newName===oldName){renderEditorSteps();return;}
  if(editorState.variables.hasOwnProperty(newName)&&newName!==oldName){newName=newName+'_2';}
  editorState.variables[newName]=editorState.variables[oldName];
  delete editorState.variables[oldName];
  editorState.steps.forEach(function(s){
    ['target','value'].forEach(function(f){
      if(s[f])s[f]=s[f].split('{{'+oldName+'}}').join('{{'+newName+'}}');
    });
  });
  renderEditorSteps();
}
/* Recompute the variables map from all step values (keeps defaults). */
function syncVariables(){
  var found={};
  editorState.steps.forEach(function(s){
    ['target','value'].forEach(function(f){
      var v=s[f];if(!v)return;var m;VAR_RE.lastIndex=0;
      while((m=VAR_RE.exec(v))){var nm=m[1].trim();found[nm]=true;}
    });
  });
  var next={};
  Object.keys(found).forEach(function(nm){next[nm]=editorState.variables.hasOwnProperty(nm)?editorState.variables[nm]:'';});
  editorState.variables=next;
}

/* add-step menu */
var addMenuOpen=false;
var ADD_ITEMS=[
  {grp:'Popular'},
  {t:'ai_decide',label:'AI decision',desc:'ask a yes / no question'},
  {t:'wait',label:'Wait',desc:'pause a moment'},
  {t:'navigate',label:'Go to page'},
  {grp:'Actions'},
  {t:'click',label:'Click'},
  {t:'type',label:'Type text'},
  {t:'scroll',label:'Scroll'},
  {t:'keypress',label:'Press key'},
  {grp:'Data & checks'},
  {t:'extract',label:'Extract'},
  {t:'ai_extract',label:'AI extract'},
  {t:'assert',label:'Check'},
  {t:'notify',label:'Notify'}
];
function openAddMenu(){
  var menu=$('addMenu');clear(menu);
  ADD_ITEMS.forEach(function(it){
    if(it.grp){menu.appendChild(h('div',{'class':'grp'},it.grp));return;}
    menu.appendChild(h('div',{'class':'menu-item',onClick:function(){addStep(it.t);}},[
      h('span',{'class':'em'},emojiFor(it.t)),
      h('span',{},[it.label, it.desc?h('small',{style:'color:var(--faint);font-weight:400;margin-left:6px'},it.desc):null])
    ]));
  });
  menu.classList.add('open');addMenuOpen=true;
}
function closeAddMenu(){$('addMenu').classList.remove('open');addMenuOpen=false;}
$('addStepBtn').addEventListener('click',function(e){e.stopPropagation();addMenuOpen?closeAddMenu():openAddMenu();});
function defaultStep(t){
  var base={type:t,target:'',value:'',description:''};
  if(t==='wait')base.value='2';
  if(t==='ai_decide'){base.value='';base.store_as='decision';}
  if(t==='ai_extract'){base.value='';base.store_as='data';}
  if(t==='extract')base.store_as='data';
  if(t==='keypress')base.value='Enter';
  if(t==='notify')base.value='Done!';
  if(t==='navigate')base.target='https://';
  return base;
}
function addStep(t){
  closeAddMenu();
  editorState.steps.push(defaultStep(t));
  renderEditorSteps();renderEditorMeta();
  var box=$('edSteps');box.lastChild.scrollIntoView({behavior:'smooth',block:'center'});
}

/* editor bottom bar */
$('edSaveBtn').addEventListener('click',function(){
  saveEditor().then(function(id){
    if(id!=null){ showView('library'); loadLibrary(); }
  });
});
$('edRunBtn').addEventListener('click',function(){
  saveEditor(true).then(function(id){if(id!=null)runWorkflowFlow(id);});
});
$('edSchedBtn').addEventListener('click',function(){
  saveEditor(true).then(function(id){if(id!=null)openScheduleSheet(id,editorState.name,editorState.trigger);});
});
function collectWorkflow(){
  syncVariables();
  var w={name:(editorState.name||'Untitled workflow'),steps:editorState.steps,
    variables:editorState.variables,trigger:editorState.trigger||{type:'manual',expression:''}};
  if(editorState.id!=null)w.id=editorState.id;
  return w;
}
function saveEditor(quiet){
  var w=collectWorkflow();
  return sendWithPromise('saveWorkflow',w).then(function(r){
    if(r&&r.id!=null){editorState.id=r.id;renderEditorMeta();}
    if(!quiet)toast('Saved','ok');
    return editorState.id;
  }).catch(function(){toast('Couldn’t save','err');return null;});
}

/* ============================== RUN ============================== */
function runWorkflowFlow(id){
  sendWithPromise('getWorkflow',id).then(function(wf){
    var vars=wf.variables||{};
    if(Object.keys(vars).length){openRunSheet(id,wf.name,vars);}
    else{
      if(view==='editor'){showView('library');loadLibrary().then(function(){doRun(id,{});});}
      else doRun(id,{});
    }
  }).catch(function(){toast('Couldn’t start run','err');});
}
function doRun(id,values){
  activeRun={id:id,stepIndex:0,total:0,message:'Starting…',done:false,ok:null};
  paintRun();
  sendWithPromise('runWorkflow',id,values).catch(function(){
    activeRun={id:id,done:true,ok:false,message:'Failed to start'};paintRun();
  });
}
cr.addWebUIListener('run-progress',function(d){
  if(!d)return;
  if(!activeRun||activeRun.id!==d.id)activeRun={id:d.id};
  activeRun.stepIndex=d.stepIndex;activeRun.total=d.total;activeRun.ok=d.ok;
  activeRun.message=d.message||('Step '+(d.stepIndex+1)+' of '+d.total);activeRun.done=false;
  paintRun();
});
cr.addWebUIListener('run-complete',function(d){
  if(!d)return;
  activeRun={id:d.id,done:true,ok:!!d.ok,message:d.message||(d.ok?'All done':'Run failed'),
    stepIndex:activeRun?activeRun.stepIndex:0,total:activeRun?activeRun.total:0};
  paintRun();
  setTimeout(function(){
    activeRun=null;
    if(view==='library')loadLibrary();
    if(runSheet.open)paintRun();
  },2600);
});

function tickerNode(){
  var ar=activeRun;
  var pct=ar.total?Math.round(((ar.done?ar.total:ar.stepIndex)/ar.total)*100):(ar.done?100:8);
  if(ar.done){
    return h('div',{},[
      h('div',{'class':'run-done '+(ar.ok?'ok':'fail')},[(ar.ok?'✓':'✗'),' ',ar.message]),
      h('div',{'class':'pbar'},h('i',{style:'width:100%;background:'+(ar.ok?'var(--ok)':'var(--err)')}))
    ]);
  }
  return h('div',{},[
    h('div',{'class':'ticker'},[
      h('span',{'class':'spin'}),
      h('span',{'class':'tmsg'},ar.message),
      ar.total?h('span',{'class':'tct'},(Math.min(ar.stepIndex+1,ar.total))+'/'+ar.total):null,
      h('button',{'class':'tstop',title:'Stop this run',onClick:function(e){e.stopPropagation();cancelActiveRun();}},'■ Stop')
    ]),
    h('div',{'class':'pbar'},h('i',{style:'width:'+pct+'%'}))
  ]);
}
function cancelActiveRun(){
  if(!activeRun||activeRun.done)return;
  var id=activeRun.id,si=activeRun.stepIndex||0,tot=activeRun.total||0;
  activeRun={id:id,done:true,ok:false,message:'Stopping…',stepIndex:si,total:tot};paintRun();
  sendWithPromise('cancelRun',id).then(function(){
    activeRun={id:id,done:true,ok:false,message:'Canceled',stepIndex:si,total:tot};paintRun();
    setTimeout(function(){activeRun=null;if(view==='library')loadLibrary();if(runSheet.open)paintRun();},1600);
  }).catch(function(){toast('Couldn’t stop run','err');});
}
function paintRun(){
  // card ticker
  Object.keys(libraryCards).forEach(function(cid){
    var reg=libraryCards[cid].querySelector('.card-run');if(!reg)return;
    if(activeRun&&String(activeRun.id)===String(cid)){reg.classList.add('on');clear(reg);reg.appendChild(tickerNode());}
    else{reg.classList.remove('on');clear(reg);}
  });
  // run sheet ticker
  if(runSheet.open&&activeRun&&String(activeRun.id)===String(runSheet.id)){
    var live=$('runLive');
    if(live){clear(live);live.appendChild(h('div',{'class':'rl-h'},activeRun.done?'Result':'Running'));live.appendChild(tickerNode());}
    if(activeRun.done){
      var btn=$('runFootBtn');if(btn){btn.textContent='Close';btn.className='btn';btn.disabled=false;}
    }
  }
}

function openRunSheet(id,name,vars){
  runSheet={open:true,id:id,name:name||'',vars:vars};
  $('runTitle').textContent='Run — '+(name||'workflow');
  $('runSubT').textContent='Fill in what changes for this run, then go.';
  var body=$('runBody');clear(body);
  var keys=Object.keys(vars);
  if(!keys.length){
    body.appendChild(h('div',{'class':'novar'},'This workflow has no fill-ins — just run it.'));
  }else{
    keys.forEach(function(k){
      body.appendChild(h('div',{'class':'vfield'},[
        h('label',{},[k.replace(/_/g,' '),h('span',{'class':'vp'},k)]),
        h('input',{'class':'inp',id:'rv_'+k,value:(vars[k]||''),placeholder:'Value for '+k,spellcheck:'false'})
      ]));
    });
  }
  body.appendChild(h('div',{'class':'run-live',id:'runLive',style:'display:none'}));
  var foot=$('runFoot');clear(foot);
  foot.appendChild(h('button',{'class':'btn ghost',onClick:closeRunSheet},'Cancel'));
  foot.appendChild(h('button',{'class':'btn violet',id:'runFootBtn',style:'flex:1.5',onClick:runSheetGo},['▶',' Run now']));
  openScrim('runScrim');
}
function runSheetGo(){
  var btn=$('runFootBtn');
  if(btn.textContent==='Close'){closeRunSheet();return;}
  var values={};Object.keys(runSheet.vars).forEach(function(k){var el=$('rv_'+k);values[k]=el?el.value:'';});
  $('runLive').style.display='block';
  btn.disabled=true;btn.textContent='Running…';
  doRun(runSheet.id,values);
}
function closeRunSheet(){runSheet.open=false;closeScrim('runScrim');}

/* ============================== SCHEDULE ============================== */
function openScheduleSheet(id,name,trigger){
  scheduleCtx={id:id,mode:'manual'};
  $('schedSubT').textContent=(name?('“'+name+'”'):'This workflow')+' — run it automatically.';
  var tr=trigger||{type:'manual',expression:''};
  scheduleCtx.mode=tr.type||'manual';
  var iv=parseInterval(tr.type==='interval'?tr.expression:'');
  var daily=(tr.type==='daily'&&tr.expression)?tr.expression:'09:00';
  var cron=(tr.type==='cron'&&tr.expression)?tr.expression:'0 9 * * *';

  var body=$('schedBody');clear(body);

  function opt(mode,title,sub,inlineEls){
    var o=h('div',{'class':'opt'+(scheduleCtx.mode===mode?' sel':''),'data-mode':mode,onClick:function(){pickMode(mode);}},[
      h('div',{'class':'radio'}),
      h('div',{'class':'ob'},[h('div',{'class':'ot'},title),h('div',{'class':'os'},sub),inlineEls||null])
    ]);
    return o;
  }

  body.appendChild(opt('manual','Manual only','Runs only when you press Run.'));

  var numI=h('input',{'class':'inp num',id:'ivNum',type:'number',min:'1',value:iv.n,onInput:updateNext,onFocus:function(){pickMode('interval');}});
  var unitI=h('select',{'class':'inp',id:'ivUnit',onChange:updateNext,onFocus:function(){pickMode('interval');}},[
    h('option',{value:'m'},'minutes'),h('option',{value:'h'},'hours')
  ]);
  unitI.value=iv.unit;
  body.appendChild(opt('interval','Every so often','Repeat on a fixed timer.',
    h('div',{'class':'opt-inline'},['Every ',numI,unitI])));

  var timeI=h('input',{'class':'inp tm',id:'dailyTime',type:'time',value:daily,onInput:updateNext,onFocus:function(){pickMode('daily');}});
  body.appendChild(opt('daily','Daily at a time','Once a day, on the clock.',
    h('div',{'class':'opt-inline'},['At ',timeI])));

  var cronI=h('input',{'class':'inp mono',id:'cronExpr',value:cron,placeholder:'0 9 * * *',spellcheck:'false',onInput:updateNext,onFocus:function(){pickMode('cron');}});
  body.appendChild(opt('cron','Custom (cron)','For power users — e.g. 0 9 * * 1-5 = 9am weekdays.',
    h('div',{'class':'opt-inline'},[cronI])));

  body.appendChild(h('div',{'class':'next-hint',id:'nextHint'},''));

  openScrim('schedScrim');
  updateNext();
}
function pickMode(mode){
  scheduleCtx.mode=mode;
  var opts=$('schedBody').querySelectorAll('.opt');
  for(var i=0;i<opts.length;i++)opts[i].classList.toggle('sel',opts[i].getAttribute('data-mode')===mode);
  updateNext();
}
function parseInterval(expr){
  var m=/^(\d+)\s*([mh])?$/.exec(String(expr||'').trim());
  if(m)return {n:parseInt(m[1],10)||30,unit:m[2]||'m'};
  return {n:30,unit:'m'};
}
function currentTrigger(){
  var mode=scheduleCtx.mode;
  if(mode==='interval'){
    var n=Math.max(1,parseInt($('ivNum').value,10)||30);
    var u=$('ivUnit').value;
    return {type:'interval',expression:n+u};
  }
  if(mode==='daily')return {type:'daily',expression:$('dailyTime').value||'09:00'};
  if(mode==='cron')return {type:'cron',expression:($('cronExpr').value||'').trim()};
  return {type:'manual',expression:''};
}
function previewNext(tr){
  var now=new Date();
  if(tr.type==='interval'){
    var iv=parseInterval(tr.expression);
    var secs=iv.n*(iv.unit==='h'?3600:60);
    return Math.floor(now.getTime()/1000)+secs;
  }
  if(tr.type==='daily'){
    var p=(tr.expression||'09:00').split(':');
    var d=new Date(now);d.setHours(parseInt(p[0],10)||9,parseInt(p[1],10)||0,0,0);
    if(d.getTime()<=now.getTime())d.setDate(d.getDate()+1);
    return Math.floor(d.getTime()/1000);
  }
  return null;
}
function updateNext(){
  var tr=currentTrigger();
  var hint=$('nextHint');clear(hint);
  if(tr.type==='manual'){hint.style.display='none';return;}
  hint.style.display='flex';
  if(tr.type==='cron'){
    hint.appendChild(h('span',{},'⏰'));
    hint.appendChild(h('span',{},[' Runs on your cron schedule ',h('b',{},tr.expression||'—')]));
    return;
  }
  var nf=previewNext(tr);
  hint.appendChild(h('span',{},'⏰'));
  hint.appendChild(h('span',{},[' Next run ',h('b',{},nf?formatNext(nf):'—')]));
}
$('schedCancel').addEventListener('click',function(){closeScrim('schedScrim');});
$('schedSave').addEventListener('click',function(){
  var tr=currentTrigger();var btn=$('schedSave');btn.disabled=true;btn.textContent='Saving…';
  sendWithPromise('saveSchedule',scheduleCtx.id,tr).then(function(r){
    if(editorState&&editorState.id===scheduleCtx.id)editorState.trigger=tr;
    closeScrim('schedScrim');
    toast(tr.type==='manual'?'Schedule cleared':('Scheduled — next '+(r&&r.nextFire?formatNext(r.nextFire):'soon')),'ok');
    loadLibrary();
  }).catch(function(){toast('Couldn’t save schedule','err');})
    .then(function(){btn.disabled=false;btn.textContent='Save schedule';});
});

/* ============================== MINI MODAL ============================== */
function openMini(opts){
  var box=$('miniModal');clear(box);
  box.appendChild(h('div',{'class':'mm-t'},opts.title));
  if(opts.message)box.appendChild(h('div',{'class':'mm-s'},opts.message));
  var input=null;
  if(opts.hasInput){
    input=h('input',{'class':'inp',value:(opts.value||''),placeholder:(opts.placeholder||''),spellcheck:'false',
      onKeydown:function(e){if(e.key==='Enter'){e.preventDefault();confirm();}}});
    box.appendChild(input);
  }
  function confirm(){var val=input?input.value:null;closeScrim('miniScrim');opts.onConfirm(val);}
  box.appendChild(h('div',{'class':'mm-btns'},[
    h('button',{'class':'btn ghost',onClick:function(){closeScrim('miniScrim');}},'Cancel'),
    h('button',{'class':'btn '+(opts.danger?'danger':'violet'),onClick:confirm},opts.confirmText||'OK')
  ]));
  openScrim('miniScrim');
  if(input)setTimeout(function(){input.focus();input.select();},60);
}
function askRename(wf){
  openMini({title:'Rename workflow',hasInput:true,value:wf.name,placeholder:'Workflow name',confirmText:'Rename',
    onConfirm:function(name){
      name=(name||'').trim();if(!name)return;
      sendWithPromise('getWorkflow',wf.id).then(function(full){
        full.name=name;delete full.stats;
        return sendWithPromise('saveWorkflow',{id:full.id,name:name,steps:full.steps,variables:full.variables,trigger:full.trigger});
      }).then(function(){toast('Renamed','ok');loadLibrary();}).catch(function(){toast('Couldn’t rename','err');});
    }});
}
function askDuplicate(wf){
  openMini({title:'Duplicate workflow',hasInput:true,value:(wf.name||'Workflow')+' copy',placeholder:'New name',confirmText:'Duplicate',
    onConfirm:function(name){
      name=(name||'').trim()||((wf.name||'Workflow')+' copy');
      sendWithPromise('duplicateWorkflow',wf.id,name).then(function(){toast('Duplicated','ok');loadLibrary();})
        .catch(function(){toast('Couldn’t duplicate','err');});
    }});
}
function askDelete(wf){
  openMini({title:'Delete this workflow?',message:'“'+(wf.name||'Untitled')+'” and its schedule will be removed. This can’t be undone.',
    danger:true,confirmText:'Delete',
    onConfirm:function(){
      sendWithPromise('deleteWorkflow',wf.id).then(function(){toast('Deleted');loadLibrary();})
        .catch(function(){toast('Couldn’t delete','err');});
    }});
}

/* ============================== scrims ============================== */
function openScrim(id){$(id).classList.add('open');}
function closeScrim(id){$(id).classList.remove('open');if(id==='runScrim')runSheet.open=false;}
[['runScrim'],['schedScrim'],['miniScrim']].forEach(function(x){
  $(x[0]).addEventListener('click',function(e){if(e.target===this)closeScrim(x[0]);});
});
document.addEventListener('keydown',function(e){
  if(e.key==='Escape'){
    ['miniScrim','runScrim','schedScrim'].forEach(function(id){if($(id).classList.contains('open'))closeScrim(id);});
    closeCardMenu();closeAddMenu();
  }
});

/* ================================ boot ================================ */
document.addEventListener('DOMContentLoaded',function(){loadLibrary();});
if(document.readyState!=='loading')loadLibrary();
</script>
</body>
</html>
)HTML";
    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }
};

}  // namespace

MoltAIAgentUI::MoltAIAgentUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIAgentDataSource>());
  web_ui->AddMessageHandler(std::make_unique<AgentStudioHandler>());
}

MoltAIAgentUI::~MoltAIAgentUI() = default;
