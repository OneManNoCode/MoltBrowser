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
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_background_browser.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace {

using molt_ai::automation::AutomationStorage;
using molt_ai::automation::Script;
using molt_ai::automation::Step;
using molt_ai::automation::StepType;

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
  }

 private:
  AutomationStorage storage_;

  // Convert a Script to a UI-friendly dict (same shape its JSON has, but
  // also pre-renders some computed fields like success_rate).
  base::DictValue ScriptToDict(const Script& s) {
    base::DictValue d = s.ToJSON();
    int rate = (s.stats.runs == 0)
                   ? -1
                   : (100 * s.stats.successes / s.stats.runs);
    d.Set("success_rate", rate);
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
    <button class="btn"        onclick="addSample()">+ Sample script</button>
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

  var stepsBlock = (s.steps || []).map(function(step, i){
    return '<div class="step-row">' +
              '<span class="step-num">' + (i + 1) + '.</span>' +
              stepShortDesc(step) +
            '</div>';
  }).join('') || '<div class="var-empty">No steps recorded.</div>';

  var lastRun = s.stats && s.stats.last_run_unix
      ? new Date(s.stats.last_run_unix * 1000).toLocaleString()
      : 'never';
  var runs = (s.stats && s.stats.runs) || 0;
  var ok   = (s.stats && s.stats.successes) || 0;

  return ''
    + '<div class="detail-section">'
    +   '<h4>Variables (used by {{name}} placeholders inside steps)</h4>'
    +   varsBlock
    + '</div>'
    + '<div class="detail-section">'
    +   '<h4>Steps (' + (s.steps ? s.steps.length : 0) + ')</h4>'
    +   '<div class="steps-list">' + stepsBlock + '</div>'
    + '</div>'
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
    card.innerHTML =
      '<div class="script-summary">' +
        '<span class="dot ' + dotClass(s) + '"></span>' +
        '<div class="info">' +
          '<div class="name">' + esc(s.name) + '</div>' +
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

refresh();
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
