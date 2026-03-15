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
/* First-Run Welcome */
.welcome-overlay{position:fixed;top:0;left:0;right:0;bottom:0;background:#0a0a0a;z-index:200;display:none;flex-direction:column;align-items:center;justify-content:center;padding:40px;text-align:center}
.welcome-overlay.open{display:flex}
.welcome-title{font-size:32px;font-weight:700;background:linear-gradient(135deg,#6366f1,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:12px}
.welcome-desc{color:#888;font-size:15px;max-width:480px;line-height:1.7;margin-bottom:24px}
.welcome-features{display:grid;grid-template-columns:1fr 1fr;gap:12px;max-width:440px;margin-bottom:28px}
.welcome-feat{text-align:left;padding:12px 16px;background:#111;border:1px solid #222;border-radius:10px}
.welcome-feat .fi{font-size:18px;margin-bottom:4px}
.welcome-feat .ft{font-size:13px;font-weight:600;color:#e0e0e0}
.welcome-feat .fd{font-size:11px;color:#666;margin-top:2px}
.welcome-btn{padding:14px 40px;border-radius:12px;border:none;background:linear-gradient(135deg,#6366f1,#8b5cf6);color:white;font-size:16px;font-weight:600;cursor:pointer;transition:opacity 0.2s;margin-bottom:10px}
.welcome-btn:hover{opacity:0.85}
.welcome-btn:disabled{opacity:0.5;cursor:not-allowed}
.welcome-skip{color:#666;font-size:12px;cursor:pointer;border:none;background:none;padding:6px}
.welcome-skip:hover{color:#aaa}
.welcome-progress{width:100%;max-width:400px;margin-top:16px;display:none}
.welcome-progress.active{display:block}
.welcome-pbar{height:8px;border-radius:4px;background:#222;overflow:hidden}
.welcome-pfill{height:100%;background:linear-gradient(90deg,#6366f1,#a855f7);transition:width 0.3s;width:0}
.welcome-ptext{font-size:12px;color:#888;margin-top:6px}
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
    <button class="top-btn" onclick="exportChat()">Export</button>
    <button class="top-btn" onclick="toggleModelPanel()">Models</button>
    <button class="top-btn" onclick="window.location='chrome://molt-ai-settings/'">Settings</button>
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

<!-- First-Run Welcome -->
<div class="welcome-overlay" id="welcomeOverlay">
  <div class="welcome-title">Welcome to MoltBrowser AI</div>
  <div class="welcome-desc">Your AI assistant runs 100% locally on your device. No cloud servers, no data collection, complete privacy. Let's get started by downloading a model.</div>
  <div class="welcome-features">
    <div class="welcome-feat"><div class="fi">&#9889;</div><div class="ft">Metal GPU Accelerated</div><div class="fd">Fast inference on Apple Silicon</div></div>
    <div class="welcome-feat"><div class="fi">&#128274;</div><div class="ft">Fully Private</div><div class="fd">Nothing leaves your device</div></div>
    <div class="welcome-feat"><div class="fi">&#128172;</div><div class="ft">Smart Chat</div><div class="fd">Summarize, explain, write code</div></div>
    <div class="welcome-feat"><div class="fi">&#127760;</div><div class="ft">Page-Aware</div><div class="fd">AI sees what you're browsing</div></div>
  </div>
  <button class="welcome-btn" id="welcomeDownloadBtn" onclick="startFirstRunDownload()">Download TinyLlama 1.1B (638 MB)</button>
  <button class="welcome-skip" onclick="skipFirstRun()">I'll set up later</button>
  <div class="welcome-progress" id="welcomeProgress">
    <div class="welcome-pbar"><div class="welcome-pfill" id="welcomePfill"></div></div>
    <div class="welcome-ptext" id="welcomePtext">Starting download...</div>
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

// ---- Markdown Rendering ----
function renderMarkdown(text) {
  var s = esc(text);
  s = s.replace(/```(\w*)\n([\s\S]*?)```/g, function(m, lang, code) {
    return '<pre style="background:#1a1a2e;padding:12px;border-radius:8px;overflow-x:auto;margin:10px 0;font-size:13px;border:1px solid #2a2a4a"><code>' + code.trim() + '</code></pre>';
  });
  s = s.replace(/`([^`\n]+)`/g, '<code style="background:#1a1a2e;padding:2px 6px;border-radius:4px;font-size:13px;color:#a78bfa">$1</code>');
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong style="color:#e0e0e0">$1</strong>');
  s = s.replace(/(?<!\*)\*([^*\n]+)\*(?!\*)/g, '<em>$1</em>');
  s = s.replace(/^### (.+)$/gm, '<div style="font-size:15px;font-weight:700;margin:12px 0 4px;color:#a78bfa">$1</div>');
  s = s.replace(/^## (.+)$/gm, '<div style="font-size:17px;font-weight:700;margin:14px 0 6px;color:#8b5cf6">$1</div>');
  s = s.replace(/^# (.+)$/gm, '<div style="font-size:19px;font-weight:700;margin:16px 0 8px;color:#6366f1">$1</div>');
  s = s.replace(/^[\-\*] (.+)$/gm, '<div style="padding-left:20px;position:relative"><span style="position:absolute;left:6px;color:#6366f1">\u2022</span>$1</div>');
  s = s.replace(/^(\d+)\. (.+)$/gm, '<div style="padding-left:24px;position:relative"><span style="position:absolute;left:0;color:#6366f1;font-size:13px">$1.</span>$2</div>');
  s = s.replace(/\n/g, '<br>');
  return s;
}

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
  currentAiEl.innerHTML = renderMarkdown(currentAiText) + '<span class="cursor"></span>';
  document.getElementById('messages').scrollTop = document.getElementById('messages').scrollHeight;
}

function finishAiMsg() {
  if (currentAiEl) currentAiEl.innerHTML = renderMarkdown(currentAiText);
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

  // Show cancel button
  var card = document.getElementById('model-' + modelId);
  if (card) {
    var btns = card.querySelectorAll('.card-actions .btn');
    for (var i = 0; i < btns.length; i++) btns[i].style.display = 'none';
    var cancelDiv = document.createElement('div');
    cancelDiv.className = 'card-actions';
    cancelDiv.id = 'cancel-wrap-' + modelId;
    cancelDiv.innerHTML = '<button class="btn danger" onclick="cancelDownload()">Cancel Download</button>';
    card.querySelector('.card-actions').parentNode.appendChild(cancelDiv);
  }

  sendWithPromise('downloadModel', modelId).then(function(r) {
    if (r.success) refreshModelList();
    else { addErr('Download failed: ' + (r.error || 'Unknown')); refreshModelList(); }
  }).catch(function(e) { addErr('Download error: ' + e); refreshModelList(); });
}

function cancelDownload() {
  chrome.send('cancelDownload', []);
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

cr.addWebUiListener('download-progress', function(modelId, current, total, speed, eta) {
  var fill = document.getElementById('pf-' + modelId);
  var ptext = document.getElementById('pt-' + modelId);
  var pw = document.getElementById('pw-' + modelId);
  if (fill && total > 0) {
    var pct = Math.round((current / total) * 100);
    fill.style.width = pct + '%';
    if (pw) pw.classList.add('active');
    var info = pct + '% (' + Math.round(current / 1048576) + ' / ' + Math.round(total / 1048576) + ' MB)';
    if (speed > 0) {
      info += ' \u00b7 ' + (speed / 1048576).toFixed(1) + ' MB/s';
      if (eta > 0) {
        var mins = Math.floor(eta / 60);
        var secs = Math.round(eta % 60);
        info += ' \u00b7 ' + (mins > 0 ? mins + 'm ' : '') + secs + 's left';
      }
    }
    if (ptext) ptext.textContent = info;
  }
});

cr.addWebUiListener('download-complete', function(modelId, success) {
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.remove('active');
  if (success) {
    refreshModelList();
    // Close welcome overlay if it was open
    var wo = document.getElementById('welcomeOverlay');
    if (wo && wo.classList.contains('open')) {
      wo.classList.remove('open');
      setStatus('offline', 'Model ready \u2014 type to start');
    }
  }
});

// Keyboard
document.getElementById('prompt').addEventListener('keydown', function(e) {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); doSend(); }
});

// ---- First-Run ----
function startFirstRunDownload() {
  var btn = document.getElementById('welcomeDownloadBtn');
  btn.disabled = true;
  btn.textContent = 'Downloading...';
  document.getElementById('welcomeProgress').classList.add('active');
  downloadModel('tinyllama-1.1b');
}

function skipFirstRun() {
  document.getElementById('welcomeOverlay').classList.remove('open');
}

// Hook welcome overlay into download progress
cr.addWebUiListener('download-progress', function(modelId, current, total, speed, eta) {
  var wo = document.getElementById('welcomeOverlay');
  if (wo && wo.classList.contains('open') && total > 0) {
    var pct = Math.round((current / total) * 100);
    document.getElementById('welcomePfill').style.width = pct + '%';
    var info = pct + '% \u2014 ' + Math.round(current / 1048576) + ' / ' +
      Math.round(total / 1048576) + ' MB';
    if (speed > 0) {
      info += ' \u00b7 ' + (speed / 1048576).toFixed(1) + ' MB/s';
      if (eta > 0) {
        var mins = Math.floor(eta / 60);
        var secs = Math.round(eta % 60);
        info += ' \u00b7 ' + (mins > 0 ? mins + 'm ' : '') + secs + 's left';
      }
    }
    document.getElementById('welcomePtext').textContent = info;
  }
});

// ---- Export Chat History ----

function exportChat() {
  if (conversationHistory.length === 0) {
    addErr('No conversation to export');
    return;
  }
  var data = JSON.stringify({
    exported_at: new Date().toISOString(),
    messages: conversationHistory
  }, null, 2);
  sendWithPromise('exportHistory', data).then(function(r) {
    if (r.success) {
      addSysMsg('Chat exported to: ' + r.filename);
    } else {
      addErr('Export failed');
    }
  });
}

function addSysMsg(text) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = '<div class="label" style="color:#4ade80">System</div><div class="text" style="border-color:#1a2e1a;color:#4ade80;font-size:13px">' + esc(text) + '</div>';
  m.appendChild(d); m.scrollTop = m.scrollHeight;
}

// ---- Keyboard Shortcuts ----
// Cmd/Ctrl+Shift+S = Summarize page
// Cmd/Ctrl+Shift+E = Explain page
// Cmd/Ctrl+Shift+X = Export chat
// Cmd/Ctrl+Shift+N = New chat

document.addEventListener('keydown', function(e) {
  var mod = e.metaKey || e.ctrlKey;
  if (!mod || !e.shiftKey) return;
  if (e.key === 'S' || e.key === 's') {
    e.preventDefault();
    doQuickAction('summarize');
  } else if (e.key === 'E' || e.key === 'e') {
    e.preventDefault();
    doQuickAction('explain');
  } else if (e.key === 'X' || e.key === 'x') {
    e.preventDefault();
    exportChat();
  } else if (e.key === 'N' || e.key === 'n') {
    e.preventDefault();
    newChat();
  }
});

function doQuickAction(action) {
  if (isGenerating) return;
  if (action === 'summarize') {
    document.getElementById('prompt').value = 'Summarize this page';
    doSend();
  } else if (action === 'explain') {
    document.getElementById('prompt').value = 'Explain this page in simple terms';
    doSend();
  }
}

// Handle ?q= parameter from omnibox @ai prefix
var params = new URLSearchParams(window.location.search);
var iq = params.get('q');

// Init
(function() {
  setStatus('loading', 'Initializing...');
  sendWithPromise('initChat').then(function(info) {
    // Apply settings
    if (info.max_history_messages) MAX_HISTORY = info.max_history_messages;

    var hw = [];
    if (info.has_gpu) hw.push(info.gpu_backend.toUpperCase());
    hw.push(info.total_ram_gb + 'GB RAM');
    hw.push(info.cpu_cores + ' cores');
    document.getElementById('hwInfo').textContent = hw.join(' \u00b7 ');
    if (info.model_loaded) {
      setStatus('ready', 'Model Ready');
    } else if (info.is_first_run) {
      setStatus('offline', 'Setup required');
      document.getElementById('welcomeOverlay').classList.add('open');
    } else {
      var dl = (info.models || []).filter(function(m) { return m.is_downloaded; });
      if (dl.length > 0) setStatus('offline', 'Model available \u2014 type to start');
      else setStatus('error', 'No models \u2014 click Models to download');
    }
    // Auto-send if we got a query from omnibox (and not first run)
    if (iq && !info.is_first_run) {
      document.getElementById('prompt').value = decodeURIComponent(iq);
      doSend();
    }
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
