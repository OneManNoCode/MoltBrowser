// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://molt-ai-agent/ — Agent Testing & Automation Interface
// Provides a UI to test autonomous browser agent capabilities:
//   - Natural language goal input
//   - Manual action controls (click, scroll, navigate, type)
//   - Live execution log with step-by-step results
//   - Security settings (domain whitelist, max iterations)

#include "chrome/browser/ui/webui/molt_ai/molt_ai_agent_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"

namespace {

class MoltAIAgentDataSource : public content::URLDataSource {
 public:
  MoltAIAgentDataSource() = default;
  ~MoltAIAgentDataSource() override = default;

  std::string GetSource() override { return "molt-ai-agent"; }

  std::string GetMimeType(const GURL& url) override { return "text/html"; }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    std::string html = R"HTML(
<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>MoltBrowser Agent</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  background:#0a0a0f;color:#e0e0e8;height:100vh;display:flex;flex-direction:column;}
.header{background:linear-gradient(135deg,#1a1a2e 0%,#16213e 100%);
  padding:16px 24px;display:flex;align-items:center;gap:16px;
  border-bottom:1px solid rgba(255,255,255,0.08);}
.header h1{font-size:20px;font-weight:600;}
.header h1 span{color:#ff4444;font-weight:700;}
.badge{background:#ff4444;color:#fff;font-size:10px;padding:2px 8px;
  border-radius:10px;font-weight:600;text-transform:uppercase;}
.main{display:flex;flex:1;overflow:hidden;}
.panel-left{width:380px;border-right:1px solid rgba(255,255,255,0.08);
  display:flex;flex-direction:column;background:#0d0d14;}
.panel-right{flex:1;display:flex;flex-direction:column;background:#0a0a0f;}

/* Goal Input */
.goal-section{padding:20px;}
.goal-section h3{font-size:13px;color:#888;text-transform:uppercase;
  letter-spacing:1px;margin-bottom:8px;}
.goal-input{width:100%;height:100px;background:#14141e;border:1px solid #2a2a3a;
  border-radius:8px;padding:12px;color:#e0e0e8;font-size:14px;resize:none;
  font-family:inherit;}
.goal-input:focus{outline:none;border-color:#ff4444;}
.goal-input::placeholder{color:#555;}

/* Action Buttons */
.actions{padding:0 20px 16px;display:flex;flex-wrap:wrap;gap:8px;}
.action-btn{background:#1a1a2e;border:1px solid #2a2a3a;color:#ccc;
  padding:8px 14px;border-radius:6px;cursor:pointer;font-size:12px;
  font-weight:500;transition:all 0.2s;}
.action-btn:hover{background:#2a2a3e;border-color:#ff4444;color:#fff;}
.action-btn.primary{background:#ff4444;border-color:#ff4444;color:#fff;
  font-weight:600;}
.action-btn.primary:hover{background:#ff5555;}
.action-btn.danger{border-color:#ff4444;color:#ff4444;}
.action-btn.danger:hover{background:#ff4444;color:#fff;}

/* Settings */
.settings{padding:16px 20px;border-top:1px solid rgba(255,255,255,0.06);}
.setting-row{display:flex;justify-content:space-between;align-items:center;
  margin-bottom:10px;}
.setting-row label{font-size:12px;color:#888;}
.setting-row input,.setting-row select{background:#14141e;border:1px solid #2a2a3a;
  color:#e0e0e8;padding:4px 8px;border-radius:4px;font-size:12px;width:120px;}
.setting-row input:focus,.setting-row select:focus{outline:none;border-color:#ff4444;}

/* Execution Log */
.log-header{padding:16px 24px;display:flex;justify-content:space-between;
  align-items:center;border-bottom:1px solid rgba(255,255,255,0.06);}
.log-header h3{font-size:14px;font-weight:600;}
.status-badge{font-size:11px;padding:3px 10px;border-radius:12px;font-weight:600;}
.status-idle{background:#1a1a2e;color:#666;}
.status-running{background:#1a3a1a;color:#4f4;}
.status-complete{background:#1a3a1a;color:#4f4;}
.status-failed{background:#3a1a1a;color:#f44;}
.log-area{flex:1;overflow-y:auto;padding:16px 24px;}

/* Step Entry */
.step{background:#0d0d14;border:1px solid #1a1a2a;border-radius:8px;
  margin-bottom:12px;overflow:hidden;}
.step-header{display:flex;align-items:center;gap:10px;padding:10px 14px;
  background:#14141e;}
.step-num{background:#ff4444;color:#fff;font-size:11px;font-weight:700;
  padding:2px 8px;border-radius:10px;min-width:24px;text-align:center;}
.step-action{font-weight:600;font-size:13px;color:#e0e0e8;flex:1;}
.step-time{font-size:11px;color:#666;}
.step-result{font-size:11px;padding:2px 8px;border-radius:10px;font-weight:600;}
.step-result.ok{background:#1a3a1a;color:#4f4;}
.step-result.fail{background:#3a1a1a;color:#f44;}
.step-body{padding:10px 14px;font-size:12px;line-height:1.6;}
.step-body .label{color:#888;font-weight:600;margin-right:6px;}
.step-body .value{color:#bbb;}
.step-body div{margin-bottom:4px;}

/* Manual Action Input */
.manual-action{padding:16px 20px;border-top:1px solid rgba(255,255,255,0.06);}
.manual-action h3{font-size:13px;color:#888;text-transform:uppercase;
  letter-spacing:1px;margin-bottom:8px;}
.manual-row{display:flex;gap:8px;margin-bottom:8px;}
.manual-row input{flex:1;background:#14141e;border:1px solid #2a2a3a;
  color:#e0e0e8;padding:6px 10px;border-radius:6px;font-size:12px;}
.manual-row input:focus{outline:none;border-color:#ff4444;}
.manual-row select{background:#14141e;border:1px solid #2a2a3a;
  color:#e0e0e8;padding:6px 10px;border-radius:6px;font-size:12px;}

/* Empty state */
.empty-state{display:flex;flex-direction:column;align-items:center;
  justify-content:center;height:100%;color:#444;text-align:center;padding:40px;}
.empty-state .icon{font-size:48px;margin-bottom:16px;opacity:0.5;}
.empty-state p{font-size:14px;max-width:300px;line-height:1.6;}
</style>
</head>
<body>

<div class="header">
  <h1><span>Molt</span>Agent</h1>
  <span class="badge">Beta</span>
  <div style="flex:1"></div>
  <a href="chrome://molt-ai/" style="color:#888;text-decoration:none;font-size:13px;">AI Chat</a>
  <a href="chrome://molt-ai-settings/" style="color:#888;text-decoration:none;font-size:13px;">Settings</a>
</div>

<div class="main">
  <div class="panel-left">
    <div class="goal-section">
      <h3>Agent Goal</h3>
      <textarea class="goal-input" id="goalInput"
        placeholder="Describe what you want the agent to do...&#10;&#10;Examples:&#10;- Navigate to nike.com and find running shoes&#10;- Search for 'best laptops 2026' on Google&#10;- Fill out the contact form on example.com"></textarea>
    </div>
    <div class="actions">
      <button class="action-btn primary" onclick="runAgent()">&#9654; Run Agent</button>
      <button class="action-btn danger" onclick="cancelAgent()" id="cancelBtn" disabled>&#9632; Cancel</button>
      <button class="action-btn" onclick="clearLog()">Clear Log</button>
    </div>

    <div class="manual-action">
      <h3>Manual Actions</h3>
      <div class="manual-row">
        <select id="actionType">
          <option value="click">Click</option>
          <option value="scroll">Scroll</option>
          <option value="navigate">Navigate</option>
          <option value="type_text">Type Text</option>
          <option value="fill_form">Fill Form</option>
          <option value="extract_data">Extract</option>
          <option value="wait">Wait</option>
        </select>
      </div>
      <div class="manual-row">
        <input type="text" id="actionTarget" placeholder="CSS selector or URL">
      </div>
      <div class="manual-row">
        <input type="text" id="actionValue" placeholder="Value (text, direction, etc.)">
      </div>
      <div class="manual-row">
        <button class="action-btn" onclick="runManualAction()" style="width:100%">Execute Action</button>
      </div>
    </div>

    <div class="settings">
      <div class="setting-row">
        <label>Max Iterations</label>
        <input type="number" id="maxIter" value="20" min="1" max="50">
      </div>
      <div class="setting-row">
        <label>Timeout (sec)</label>
        <input type="number" id="timeout" value="120" min="10" max="600">
      </div>
      <div class="setting-row">
        <label>Domain Whitelist</label>
        <input type="text" id="domains" placeholder="*.com (all)">
      </div>
    </div>
  </div>

  <div class="panel-right">
    <div class="log-header">
      <h3>Execution Log</h3>
      <span class="status-badge status-idle" id="statusBadge">Idle</span>
    </div>
    <div class="log-area" id="logArea">
      <div class="empty-state" id="emptyState">
        <div class="icon">&#129302;</div>
        <p>Enter a goal and click "Run Agent" to start autonomous browser automation, or use manual actions to test individual commands.</p>
      </div>
    </div>
  </div>
</div>

<script>
var stepCount = 0;
var isRunning = false;

function setStatus(status, text) {
  var badge = document.getElementById('statusBadge');
  badge.className = 'status-badge status-' + status;
  badge.textContent = text;
}

function addStep(step) {
  var empty = document.getElementById('emptyState');
  if (empty) empty.style.display = 'none';

  var div = document.createElement('div');
  div.className = 'step';

  var actionLabels = {
    click: 'CLICK', scroll: 'SCROLL', navigate: 'NAVIGATE',
    fill_form: 'FILL FORM', extract_data: 'EXTRACT', open_tab: 'OPEN TAB',
    close_tab: 'CLOSE TAB', wait: 'WAIT', type_text: 'TYPE',
    done: 'DONE', error: 'ERROR'
  };

  div.innerHTML =
    '<div class="step-header">' +
      '<span class="step-num">' + step.number + '</span>' +
      '<span class="step-action">' + (actionLabels[step.action] || step.action) +
        (step.target ? ': ' + esc(step.target.substring(0, 60)) : '') + '</span>' +
      '<span class="step-result ' + (step.success ? 'ok' : 'fail') + '">' +
        (step.success ? 'OK' : 'FAIL') + '</span>' +
      '<span class="step-time">' + step.time_ms + 'ms</span>' +
    '</div>' +
    '<div class="step-body">' +
      (step.reason ? '<div><span class="label">Reason:</span><span class="value">' + esc(step.reason) + '</span></div>' : '') +
      (step.observation ? '<div><span class="label">Observed:</span><span class="value">' + esc(step.observation) + '</span></div>' : '') +
      (step.value ? '<div><span class="label">Value:</span><span class="value">' + esc(step.value) + '</span></div>' : '') +
    '</div>';

  document.getElementById('logArea').appendChild(div);
  div.scrollIntoView({behavior: 'smooth'});
}

function esc(s) {
  var d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

function runAgent() {
  var goal = document.getElementById('goalInput').value.trim();
  if (!goal) { alert('Please enter a goal'); return; }

  stepCount = 0;
  isRunning = true;
  setStatus('running', 'Running...');
  document.getElementById('cancelBtn').disabled = false;
  document.getElementById('logArea').innerHTML = '';

  chrome.send('runAgent', [goal,
    parseInt(document.getElementById('maxIter').value) || 20,
    parseInt(document.getElementById('timeout').value) * 1000 || 120000,
    document.getElementById('domains').value || '*'
  ]);
}

function cancelAgent() {
  chrome.send('cancelAgent');
  isRunning = false;
  setStatus('idle', 'Cancelled');
  document.getElementById('cancelBtn').disabled = true;
}

function runManualAction() {
  var type = document.getElementById('actionType').value;
  var target = document.getElementById('actionTarget').value;
  var value = document.getElementById('actionValue').value;
  chrome.send('runAction', [type, target, value]);
}

function clearLog() {
  document.getElementById('logArea').innerHTML =
    '<div class="empty-state" id="emptyState">' +
    '<div class="icon">&#129302;</div>' +
    '<p>Enter a goal and click "Run Agent" to start.</p></div>';
  stepCount = 0;
  setStatus('idle', 'Idle');
}

// Listen for agent step results from C++
cr.addWebUIListener('agent-step', function(step) {
  stepCount++;
  step.number = stepCount;
  addStep(step);
});

cr.addWebUIListener('agent-complete', function(result) {
  isRunning = false;
  document.getElementById('cancelBtn').disabled = true;
  if (result.status === 'completed') {
    setStatus('complete', 'Complete (' + result.steps + ' steps)');
    if (result.answer) {
      addStep({
        number: stepCount + 1, action: 'done', target: '',
        value: result.answer, reason: 'Task completed',
        observation: '', success: true, time_ms: 0
      });
    }
  } else {
    setStatus('failed', 'Failed: ' + (result.error || 'Unknown error'));
  }
});

cr.addWebUIListener('action-result', function(result) {
  stepCount++;
  addStep({
    number: stepCount,
    action: result.action,
    target: result.target,
    value: result.value,
    reason: 'Manual action',
    observation: result.observation || '',
    success: result.success,
    time_ms: result.time_ms || 0
  });
});
</script>
</body></html>
)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldServeMimeTypeAsContentTypeHeader() override { return true; }
};

}  // namespace

MoltAIAgentUI::MoltAIAgentUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIAgentDataSource>());
}

MoltAIAgentUI::~MoltAIAgentUI() = default;
