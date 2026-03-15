// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

// Custom URL data source that serves inline HTML for the full-page AI chat.
// Uses the same chrome.send() / cr.addWebUiListener() IPC as the side panel,
// connected to BrowserAIRuntime via MoltAIChatHandler.
class MoltAIDataSource : public content::URLDataSource {
 public:
  MoltAIDataSource() = default;
  ~MoltAIDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltAIHost; }

  std::string GetMimeType(const GURL& url) override {
    return "text/html";
  }

  // Allow inline scripts and styles for our embedded HTML page
  std::string GetContentSecurityPolicy(
      const network::mojom::CSPDirectiveName directive) override {
    if (directive == network::mojom::CSPDirectiveName::ScriptSrc) {
      return "script-src chrome://resources 'self' 'unsafe-inline';";
    }
    if (directive == network::mojom::CSPDirectiveName::StyleSrc) {
      return "style-src 'self' 'unsafe-inline';";
    }
    if (directive == network::mojom::CSPDirectiveName::TrustedTypes) {
      return "";
    }
    if (directive ==
        network::mojom::CSPDirectiveName::RequireTrustedTypesFor) {
      return "";
    }
    return content::URLDataSource::GetContentSecurityPolicy(directive);
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    const std::string html = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>MoltBrowser AI</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0a0a0a;color:#e0e0e0;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:40px 20px}
.logo{font-size:36px;font-weight:700;background:linear-gradient(135deg,#6366f1,#8b5cf6,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:4px}
.subtitle{color:#888;margin-bottom:8px;font-size:14px}
.top-bar{display:flex;align-items:center;gap:12px;margin-bottom:10px;flex-wrap:wrap;justify-content:center}
.status-bar{display:flex;align-items:center;gap:8px;font-size:12px}
.status-dot{width:8px;height:8px;border-radius:50%;transition:background 0.3s}
.status-dot.ready{background:#4ade80}
.status-dot.loading{background:#fbbf24;animation:pulse 1s infinite}
.status-dot.error{background:#f87171}
.status-dot.offline{background:#888}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.status-text{color:#888;transition:color 0.3s}
.hw-info{color:#555;font-size:11px;display:flex;gap:10px}
.top-actions{display:flex;gap:8px;align-items:center}
.top-btn{padding:6px 14px;border-radius:8px;border:1px solid #333;background:#111;color:#aaa;font-size:12px;cursor:pointer;transition:all 0.2s}
.top-btn:hover{border-color:#6366f1;color:#e0e0e0}
.context-info{font-size:11px;color:#444}
.chat-container{width:100%;max-width:720px;flex:1;display:flex;flex-direction:column}
.messages{flex:1;overflow-y:auto;padding:20px 0}
.message{margin-bottom:20px;padding:16px 20px;border-radius:12px;line-height:1.6;font-size:15px;max-width:90%}
.message.user{background:#1a1a2e;border:1px solid #2a2a4a;margin-left:auto}
.message.ai{background:#111;border:1px solid #222}
.message .label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px;color:#6366f1}
.message.user .label{color:#8b5cf6}
.model-badge{display:inline-block;padding:2px 8px;border-radius:4px;background:#1a1a2e;color:#8b5cf6;font-size:11px;margin-bottom:8px}
.message .text{white-space:pre-wrap;word-wrap:break-word}
.message .text .cursor{display:inline-block;width:2px;height:16px;background:#6366f1;animation:blink 0.8s infinite;vertical-align:text-bottom;margin-left:1px}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
.input-area{display:flex;gap:12px;padding:20px 0;border-top:1px solid #222}
.input-area input{flex:1;padding:14px 20px;border-radius:12px;border:1px solid #333;background:#111;color:#e0e0e0;font-size:15px;outline:none;transition:border-color 0.2s}
.input-area input:focus{border-color:#6366f1}
.input-area button.send{padding:14px 28px;border-radius:12px;border:none;background:linear-gradient(135deg,#6366f1,#8b5cf6);color:white;font-size:15px;font-weight:600;cursor:pointer;transition:opacity 0.2s}
.input-area button.send:hover{opacity:0.85}
.input-area button.send:disabled{opacity:0.4;cursor:not-allowed}
.input-area button.cancel{padding:14px 16px;border-radius:12px;border:1px solid #f87171;background:transparent;color:#f87171;font-size:15px;cursor:pointer;display:none}
.input-area button.cancel.active{display:block}
/* Model Panel Overlay */
.model-overlay{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.7);z-index:100;display:none;align-items:center;justify-content:center}
.model-overlay.open{display:flex}
.model-dialog{background:#111;border:1px solid #333;border-radius:16px;width:90%;max-width:560px;max-height:80vh;overflow-y:auto;padding:24px}
.model-dialog h2{font-size:18px;font-weight:600;margin-bottom:16px;color:#e0e0e0}
.model-card{padding:14px;border:1px solid #222;border-radius:10px;margin-bottom:10px;background:#0d0d0d}
.model-card .name{font-size:14px;font-weight:600;color:#e0e0e0}
.model-card .meta{font-size:12px;color:#666;margin-top:2px}
.model-card .card-actions{margin-top:10px;display:flex;gap:8px;align-items:center}
.model-card .btn{padding:6px 16px;border-radius:8px;font-size:12px;cursor:pointer;border:1px solid #333;background:#111;color:#aaa;transition:all 0.2s}
.model-card .btn:hover{border-color:#6366f1;color:#e0e0e0}
.model-card .btn:disabled{opacity:0.4;cursor:not-allowed}
.model-card .btn.primary{background:#6366f1;border-color:#6366f1;color:#fff}
.model-card .btn.primary:hover{opacity:0.85}
.model-card .btn.danger{border-color:#f87171;color:#f87171}
.model-card .badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:11px;font-weight:600}
.model-card .badge.active{background:#1a2e1a;color:#4ade80}
.model-card .badge.downloaded{background:#1a1a2e;color:#6366f1}
.model-card .badge.unavailable{background:#1a1a1a;color:#666}
.model-card .progress-wrap{margin-top:8px;display:none}
.model-card .progress-wrap.active{display:block}
.model-card .progress-bar{height:6px;border-radius:3px;background:#222;overflow:hidden}
.model-card .progress-fill{height:100%;background:linear-gradient(90deg,#6366f1,#a855f7);transition:width 0.3s;width:0}
.model-card .progress-text{font-size:11px;color:#888;margin-top:4px}
</style>
</head>
<body>
<div class="logo">MoltBrowser AI</div>
<div class="subtitle">Local AI — Private by Design — Powered by llama.cpp</div>
<div class="hw-info" id="hwInfo"></div>
<div class="top-bar">
  <div class="status-bar">
    <div class="status-dot offline" id="statusDot"></div>
    <div class="status-text" id="statusText">Initializing...</div>
  </div>
  <div class="top-actions">
    <button class="top-btn" onclick="newChat()">New Chat</button>
    <button class="top-btn" onclick="toggleModelPanel()">Models</button>
  </div>
  <div class="context-info" id="contextInfo"></div>
</div>
<div class="chat-container">
  <div class="messages" id="messages">
    <div class="message ai">
      <div class="label">MoltBrowser AI</div>
      <div class="model-badge" id="modelBadge">Local LLM</div>
      <div class="text">Hello! I'm your local AI assistant. I run entirely on your device for complete privacy. Ask me anything!</div>
    </div>
  </div>
  <div class="input-area">
    <input type="text" id="prompt" placeholder="Ask MoltBrowser AI anything..." autofocus>
    <button class="cancel" id="cancelBtn" onclick="cancelGeneration()">Stop</button>
    <button class="send" id="sendBtn" onclick="doSend()">Send</button>
  </div>
</div>

<!-- Model Management Overlay -->
<div class="model-overlay" id="modelOverlay" onclick="if(event.target===this)toggleModelPanel()">
  <div class="model-dialog">
    <h2>Model Management</h2>
    <div id="modelList"></div>
    <div style="margin-top:16px;text-align:right">
      <button class="top-btn" onclick="toggleModelPanel()">Close</button>
    </div>
  </div>
</div>

<script>
// ============================================================
// MoltBrowser AI Full Page — WebUI JavaScript (Day 8)
// ============================================================

var isGenerating = false;
var currentAiEl = null;
var currentAiText = '';
var idCounter = 0;
var pendingCbs = {};
var conversationHistory = [];
var MAX_HISTORY = 16;

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
  var cb = pendingCbs[id]; if (cb) { delete pendingCbs[id]; ok ? cb.resolve(resp) : cb.reject(resp); }
};
var wuiListeners = {};
cr.addWebUiListener = function(ev, fn) { (wuiListeners[ev] = wuiListeners[ev] || []).push(fn); };
cr.webUIListenerCallback = function(ev) {
  var a = Array.prototype.slice.call(arguments, 1);
  (wuiListeners[ev] || []).forEach(function(fn) { fn.apply(null, a); });
};

function esc(t) { var d = document.createElement('div'); d.textContent = t; return d.innerHTML; }

function setStatus(cls, text) {
  document.getElementById('statusDot').className = 'status-dot ' + cls;
  document.getElementById('statusText').textContent = text;
  document.getElementById('statusText').style.color = cls === 'ready' ? '#4ade80' : cls === 'error' ? '#f87171' : '#888';
}

function addUserMsg(text) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message user';
  d.innerHTML = '<div class="label">You</div><div class="text">' + esc(text) + '</div>';
  m.appendChild(d); m.scrollTop = m.scrollHeight;
}

function startAiMsg() {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = '<div class="label">MoltBrowser AI</div><div class="model-badge">Local LLM</div><div class="text"><span class="cursor"></span></div>';
  m.appendChild(d); m.scrollTop = m.scrollHeight;
  currentAiEl = d.querySelector('.text');
  currentAiText = '';
}

function appendTok(tok) {
  if (!currentAiEl) return;
  currentAiText += tok;
  currentAiEl.innerHTML = esc(currentAiText) + '<span class="cursor"></span>';
  document.getElementById('messages').scrollTop = document.getElementById('messages').scrollHeight;
}

function finishAiMsg() {
  if (currentAiEl) currentAiEl.innerHTML = esc(currentAiText);
  currentAiEl = null; currentAiText = '';
}

function setGen(v) {
  isGenerating = v;
  document.getElementById('sendBtn').disabled = v;
  document.getElementById('prompt').disabled = v;
  document.getElementById('cancelBtn').className = 'cancel' + (v ? ' active' : '');
}

function updateContextInfo() {
  var el = document.getElementById('contextInfo');
  var n = conversationHistory.length;
  if (n === 0) { el.textContent = ''; return; }
  var chars = 0;
  for (var i = 0; i < n; i++) chars += conversationHistory[i].content.length;
  el.textContent = n + ' msgs \u00b7 ~' + Math.round(chars / 4) + ' tokens';
}

function trimHistory() {
  if (conversationHistory.length > MAX_HISTORY) {
    conversationHistory = conversationHistory.slice(conversationHistory.length - MAX_HISTORY);
  }
}

function doSend() {
  if (isGenerating) return;
  var inp = document.getElementById('prompt');
  var t = inp.value.trim(); if (!t) return;
  addUserMsg(t);
  conversationHistory.push({role: 'user', content: t});
  trimHistory();
  inp.value = '';
  setGen(true); startAiMsg();

  var prevHistory = '';
  if (conversationHistory.length > 1) {
    var prev = conversationHistory.slice(0, conversationHistory.length - 1);
    for (var i = 0; i < prev.length; i++) {
      var m = prev[i];
      prevHistory += (m.role === 'user' ? '<|user|>\n' : '<|assistant|>\n') + m.content + '</s>\n';
    }
  }

  sendWithPromise('sendPrompt', t, prevHistory).then(function(r) {
    var aiText = currentAiText.replace(/<\/s>\s*$/g, '').replace(/<\/s>/g, '').trim();
    if (aiText) conversationHistory.push({role: 'assistant', content: aiText});
    finishAiMsg(); setGen(false);
    updateContextInfo();
    if (!r.success && r.error) addErr(r.error);
  }).catch(function(e) { finishAiMsg(); setGen(false); updateContextInfo(); addErr('Error: ' + e); });
}

function addErr(text) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = '<div class="label" style="color:#f87171">Error</div><div class="text" style="border-color:#f87171;color:#f87171">' + esc(text) + '</div>';
  m.appendChild(d); m.scrollTop = m.scrollHeight;
}

function newChat() {
  conversationHistory = [];
  var m = document.getElementById('messages');
  m.innerHTML = '<div class="message ai"><div class="label">MoltBrowser AI</div>' +
    '<div class="model-badge">Local LLM</div>' +
    '<div class="text">New conversation started. How can I help?</div></div>';
  updateContextInfo();
}

function cancelGeneration() { chrome.send('cancelGeneration', []); }

// ---- Model Management ----

function toggleModelPanel() {
  var o = document.getElementById('modelOverlay');
  if (o.classList.contains('open')) {
    o.classList.remove('open');
  } else {
    o.classList.add('open');
    refreshModelList();
  }
}

function refreshModelList() {
  sendWithPromise('getModelStatus').then(function(info) {
    var list = document.getElementById('modelList');
    list.innerHTML = '';
    var models = info.models || [];
    models.sort(function(a, b) { return a.file_size_mb - b.file_size_mb; });

    models.forEach(function(m) {
      var card = document.createElement('div');
      card.className = 'model-card';
      card.id = 'model-' + m.model_id;

      var sizeMB = m.file_size_mb;
      var sizeStr = sizeMB > 1024 ? (sizeMB / 1024).toFixed(1) + ' GB' : sizeMB + ' MB';
      var statusBadge = '';
      var actionBtns = '';

      if (m.is_loaded) {
        statusBadge = '<span class="badge active">Active</span>';
      } else if (m.is_downloaded) {
        statusBadge = '<span class="badge downloaded">Ready</span>';
        actionBtns = '<button class="btn primary" onclick="loadModel(\'' + m.model_id + '\')">Load</button>' +
                     '<button class="btn danger" onclick="deleteModel(\'' + m.model_id + '\')">Delete</button>';
      } else {
        statusBadge = '<span class="badge unavailable">Not Downloaded</span>';
        actionBtns = '<button class="btn primary" onclick="downloadModel(\'' + m.model_id + '\')">Download (' + sizeStr + ')</button>';
      }

      card.innerHTML =
        '<div class="name">' + esc(m.display_name) + '</div>' +
        '<div class="meta">' + m.quantization + ' \u00b7 ' + sizeStr + '</div>' +
        '<div style="margin-top:6px">' + statusBadge + '</div>' +
        (actionBtns ? '<div class="card-actions">' + actionBtns + '</div>' : '') +
        '<div class="progress-wrap" id="pw-' + m.model_id + '">' +
          '<div class="progress-bar"><div class="progress-fill" id="pf-' + m.model_id + '"></div></div>' +
          '<div class="progress-text" id="pt-' + m.model_id + '"></div>' +
        '</div>';

      list.appendChild(card);
    });
  });
}

function downloadModel(modelId) {
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.add('active');
  sendWithPromise('downloadModel', modelId).then(function(r) {
    if (r.success) refreshModelList();
    else { addErr('Download failed: ' + (r.error || 'Unknown')); refreshModelList(); }
  }).catch(function(e) { addErr('Download error: ' + e); refreshModelList(); });
}

function loadModel(modelId) {
  setStatus('loading', 'Loading ' + modelId + '...');
  sendWithPromise('loadModel', modelId).then(function(r) {
    if (r.success) { setStatus('ready', 'Model Ready'); refreshModelList(); }
    else setStatus('error', 'Load failed');
  });
}

function deleteModel(modelId) {
  sendWithPromise('deleteModel', modelId).then(function(r) {
    if (r.success) refreshModelList();
  });
}

// ---- Event Listeners ----

cr.addWebUiListener('ai-token', function(tok, isDone) { if (tok) appendTok(tok); });
cr.addWebUiListener('model-status', function(st, detail) {
  if (st === 'loading') setStatus('loading', 'Loading model...');
  else if (st === 'ready') setStatus('ready', 'Model Ready');
  else if (st === 'error') setStatus('error', detail || 'Error');
});

cr.addWebUiListener('download-progress', function(modelId, current, total) {
  var fill = document.getElementById('pf-' + modelId);
  var ptext = document.getElementById('pt-' + modelId);
  var pw = document.getElementById('pw-' + modelId);
  if (fill && total > 0) {
    var pct = Math.round((current / total) * 100);
    fill.style.width = pct + '%';
    if (pw) pw.classList.add('active');
    if (ptext) ptext.textContent = pct + '% (' + Math.round(current / 1048576) + ' / ' + Math.round(total / 1048576) + ' MB)';
  }
});

cr.addWebUiListener('download-complete', function(modelId, success) {
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.remove('active');
  if (success) refreshModelList();
});

// Keyboard
document.getElementById('prompt').addEventListener('keydown', function(e) {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); doSend(); }
});

// Handle ?q= parameter from omnibox @ai prefix
var params = new URLSearchParams(window.location.search);
var iq = params.get('q');

// Init
(function() {
  setStatus('loading', 'Initializing...');
  sendWithPromise('initChat').then(function(info) {
    var hw = [];
    if (info.has_gpu) hw.push(info.gpu_backend.toUpperCase());
    hw.push(info.total_ram_gb + 'GB RAM');
    hw.push(info.cpu_cores + ' cores');
    document.getElementById('hwInfo').textContent = hw.join(' \u00b7 ');
    if (info.model_loaded) {
      setStatus('ready', 'Model Ready');
    } else {
      var dl = (info.models || []).filter(function(m) { return m.is_downloaded; });
      if (dl.length > 0) setStatus('offline', 'Model available \u2014 type to start');
      else setStatus('error', 'No models \u2014 click Models to download');
    }
    // Auto-send if we got a query from omnibox
    if (iq) { document.getElementById('prompt').value = decodeURIComponent(iq); doSend(); }
  }).catch(function() { setStatus('error', 'Failed to initialize'); });
})();
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltAIUI::MoltAIUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  // Add the HTML data source
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIDataSource>());

  // Add the message handler that bridges JS <-> BrowserAIRuntime
  Profile* profile = Profile::FromWebUI(web_ui);
  web_ui->AddMessageHandler(std::make_unique<MoltAIChatHandler>(profile));
}

MoltAIUI::~MoltAIUI() = default;
