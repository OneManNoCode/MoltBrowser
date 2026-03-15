// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

// Custom URL data source that serves inline HTML for the AI chat sidebar.
// This version uses chrome.send() and cr.addWebUiListener() for real
// communication with BrowserAIRuntime via MoltAIChatHandler.
class MoltAIChatDataSource : public content::URLDataSource {
 public:
  MoltAIChatDataSource() = default;
  ~MoltAIChatDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltAIChatHost; }

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
<title>AI Chat</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d0d0d;color:#e0e0e0;height:100vh;display:flex;flex-direction:column;position:relative}
.header{padding:12px 16px;border-bottom:1px solid #222;display:flex;align-items:center;gap:10px}
.header .title{font-size:14px;font-weight:600;background:linear-gradient(135deg,#6366f1,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.header .status{font-size:11px;display:flex;align-items:center;gap:4px;transition:color 0.3s}
.header .status::before{content:'';width:6px;height:6px;border-radius:50%;transition:background 0.3s}
.header .status.ready{color:#4ade80}
.header .status.ready::before{background:#4ade80}
.header .status.loading{color:#fbbf24}
.header .status.loading::before{background:#fbbf24;animation:pulse 1s infinite}
.header .status.error{color:#f87171}
.header .status.error::before{background:#f87171}
.header .status.offline{color:#888}
.header .status.offline::before{background:#888}
.header-actions{margin-left:auto;display:flex;gap:6px;align-items:center}
.icon-btn{background:none;border:1px solid #333;border-radius:6px;color:#888;padding:4px 8px;font-size:11px;cursor:pointer;transition:all 0.2s}
.icon-btn:hover{border-color:#6366f1;color:#e0e0e0}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.hw-bar{padding:6px 16px;background:#0a0a0a;border-bottom:1px solid #1a1a1a;font-size:10px;color:#555;display:flex;gap:12px}
.hw-bar span{display:flex;align-items:center;gap:3px}
.context-bar{padding:4px 16px;font-size:10px;color:#444;text-align:right;border-bottom:1px solid #111}
.messages{flex:1;overflow-y:auto;padding:16px}
.message{margin-bottom:16px;font-size:13px;line-height:1.6}
.message .sender{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:4px;color:#6366f1}
.message.user .sender{color:#8b5cf6}
.message .text{padding:10px 14px;border-radius:10px;background:#111;border:1px solid #1a1a1a;white-space:pre-wrap;word-wrap:break-word}
.message.user .text{background:#1a1a2e;border-color:#2a2a4a}
.message .text .cursor{display:inline-block;width:2px;height:14px;background:#6366f1;animation:blink 0.8s infinite;vertical-align:text-bottom;margin-left:1px}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
.actions{padding:8px 16px;display:flex;gap:6px;flex-wrap:wrap}
.actions button{padding:6px 12px;border-radius:8px;border:1px solid #333;background:#111;color:#aaa;font-size:12px;cursor:pointer;transition:all 0.2s}
.actions button:hover{border-color:#6366f1;color:#e0e0e0}
.actions button:disabled{opacity:0.4;cursor:not-allowed}
.input-area{padding:12px 16px;border-top:1px solid #222;display:flex;gap:8px}
.input-area input{flex:1;padding:10px 14px;border-radius:10px;border:1px solid #333;background:#111;color:#e0e0e0;font-size:13px;outline:none}
.input-area input:focus{border-color:#6366f1}
.input-area button.send{padding:10px 16px;border-radius:10px;border:none;background:#6366f1;color:white;font-size:13px;cursor:pointer;transition:opacity 0.2s}
.input-area button.send:hover{opacity:0.85}
.input-area button.send:disabled{opacity:0.4;cursor:not-allowed}
.input-area button.cancel{padding:10px 12px;border-radius:10px;border:1px solid #f87171;background:transparent;color:#f87171;font-size:13px;cursor:pointer;display:none}
.input-area button.cancel.active{display:block}
/* Model Panel */
.model-panel{position:absolute;top:0;left:0;right:0;bottom:0;background:#0d0d0d;z-index:10;display:none;flex-direction:column;overflow-y:auto}
.model-panel.open{display:flex}
.model-panel-header{padding:12px 16px;border-bottom:1px solid #222;display:flex;align-items:center;justify-content:space-between}
.model-panel-header h3{font-size:14px;font-weight:600;color:#e0e0e0}
.model-card{padding:12px 16px;border-bottom:1px solid #1a1a1a}
.model-card .name{font-size:13px;font-weight:600;color:#e0e0e0}
.model-card .meta{font-size:11px;color:#666;margin-top:2px}
.model-card .card-actions{margin-top:8px;display:flex;gap:6px;align-items:center}
.model-card .btn{padding:4px 12px;border-radius:6px;font-size:11px;cursor:pointer;border:1px solid #333;background:#111;color:#aaa;transition:all 0.2s}
.model-card .btn:hover{border-color:#6366f1;color:#e0e0e0}
.model-card .btn:disabled{opacity:0.4;cursor:not-allowed}
.model-card .btn.primary{background:#6366f1;border-color:#6366f1;color:#fff}
.model-card .btn.primary:hover{opacity:0.85}
.model-card .btn.danger{border-color:#f87171;color:#f87171}
.model-card .btn.danger:hover{background:#2a1111}
.model-card .badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:10px;font-weight:600}
.model-card .badge.active{background:#1a2e1a;color:#4ade80}
.model-card .badge.downloaded{background:#1a1a2e;color:#6366f1}
.model-card .badge.unavailable{background:#1a1a1a;color:#666}
.model-card .progress-wrap{margin-top:6px;display:none}
.model-card .progress-wrap.active{display:block}
.model-card .progress-bar{height:4px;border-radius:2px;background:#222;overflow:hidden}
.model-card .progress-fill{height:100%;background:linear-gradient(90deg,#6366f1,#a855f7);transition:width 0.3s;width:0}
.model-card .progress-text{font-size:10px;color:#888;margin-top:2px}
</style>
</head>
<body>
<div class="header">
  <div class="title">MoltBrowser AI</div>
  <div class="status offline" id="statusIndicator">Initializing...</div>
  <div class="header-actions">
    <button class="icon-btn" onclick="newChat()" title="New Chat">New</button>
    <button class="icon-btn" onclick="toggleModelPanel()" title="Models">Models</button>
    <button class="icon-btn" onclick="window.open('chrome://molt-ai-settings/')" title="Settings">&#9881;</button>
  </div>
</div>
<div class="hw-bar" id="hwBar">
  <span id="hwGpu"></span>
  <span id="hwRam"></span>
  <span id="hwCores"></span>
</div>
<div class="context-bar" id="contextBar"></div>
<div class="messages" id="messages">
  <div class="message ai">
    <div class="sender">AI Assistant</div>
    <div class="text">Welcome! I'm your local AI assistant running entirely on this device. Send a message to begin.</div>
  </div>
</div>
<div class="actions" id="quickActions">
  <button onclick="quickAction('summarize')">Summarize</button>
  <button onclick="quickAction('extract')">Extract Data</button>
  <button onclick="quickAction('explain')">Explain</button>
  <button onclick="quickAction('translate')">Translate</button>
</div>
<div class="input-area">
  <input type="text" id="prompt" placeholder="Ask MoltBrowser AI..." autofocus>
  <button class="cancel" id="cancelBtn" onclick="cancelGeneration()">Stop</button>
  <button class="send" id="sendBtn" onclick="sendMessage()">Send</button>
</div>

<!-- Model Management Panel -->
<div class="model-panel" id="modelPanel">
  <div class="model-panel-header">
    <h3>Model Management</h3>
    <button class="icon-btn" onclick="toggleModelPanel()">Close</button>
  </div>
  <div id="modelList"></div>
</div>

<script>
// ============================================================
// MoltBrowser AI Chat — WebUI JavaScript (Day 8)
// Model management, page content extraction, context management
// ============================================================

var isGenerating = false;
var currentAiMessageEl = null;
var currentAiText = '';
var promptIdCounter = 0;

// Conversation history for multi-turn chat
var conversationHistory = [];
var MAX_HISTORY_MESSAGES = 16; // 8 user + 8 assistant exchanges

// cr.sendWithPromise polyfill
var pendingCallbacks = {};

function sendWithPromise(method) {
  var args = Array.prototype.slice.call(arguments, 1);
  var id = method + '_' + (++promptIdCounter);
  return new Promise(function(resolve, reject) {
    pendingCallbacks[id] = {resolve: resolve, reject: reject};
    chrome.send(method, [id].concat(args));
  });
}

window.cr = window.cr || {};
cr.webUIResponse = function(id, success, response) {
  var cb = pendingCallbacks[id];
  if (cb) {
    delete pendingCallbacks[id];
    if (success) cb.resolve(response);
    else cb.reject(response);
  }
};

var webUIListeners = {};
cr.addWebUiListener = function(eventName, callback) {
  if (!webUIListeners[eventName]) webUIListeners[eventName] = [];
  webUIListeners[eventName].push(callback);
};
cr.webUIListenerCallback = function(event) {
  var args = Array.prototype.slice.call(arguments, 1);
  var listeners = webUIListeners[event];
  if (listeners) {
    for (var i = 0; i < listeners.length; i++) {
      listeners[i].apply(null, args);
    }
  }
};

// ---- UI Helpers ----

function esc(t) {
  var d = document.createElement('div');
  d.textContent = t;
  return d.innerHTML;
}

// ---- Markdown Rendering ----
function renderMarkdown(text) {
  // Escape HTML first
  var s = esc(text);
  // Code blocks (``` ... ```)
  s = s.replace(/```(\w*)\n([\s\S]*?)```/g, function(m, lang, code) {
    return '<pre style="background:#1a1a2e;padding:10px;border-radius:6px;overflow-x:auto;margin:8px 0;font-size:12px;border:1px solid #2a2a4a"><code>' + code.trim() + '</code></pre>';
  });
  // Inline code (`...`)
  s = s.replace(/`([^`\n]+)`/g, '<code style="background:#1a1a2e;padding:2px 6px;border-radius:4px;font-size:12px;color:#a78bfa">$1</code>');
  // Bold (**...**)
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong style="color:#e0e0e0">$1</strong>');
  // Italic (*...*)
  s = s.replace(/(?<!\*)\*([^*\n]+)\*(?!\*)/g, '<em>$1</em>');
  // Headers (### ... , ## ... , # ...)
  s = s.replace(/^### (.+)$/gm, '<div style="font-size:14px;font-weight:700;margin:10px 0 4px;color:#a78bfa">$1</div>');
  s = s.replace(/^## (.+)$/gm, '<div style="font-size:15px;font-weight:700;margin:12px 0 4px;color:#8b5cf6">$1</div>');
  s = s.replace(/^# (.+)$/gm, '<div style="font-size:16px;font-weight:700;margin:14px 0 6px;color:#6366f1">$1</div>');
  // Unordered lists (- item or * item)
  s = s.replace(/^[\-\*] (.+)$/gm, '<div style="padding-left:16px;position:relative"><span style="position:absolute;left:4px;color:#6366f1">\u2022</span>$1</div>');
  // Ordered lists (1. item)
  s = s.replace(/^(\d+)\. (.+)$/gm, '<div style="padding-left:20px;position:relative"><span style="position:absolute;left:0;color:#6366f1;font-size:12px">$1.</span>$2</div>');
  // Line breaks
  s = s.replace(/\n/g, '<br>');
  return s;
}

function setStatus(state, text) {
  var el = document.getElementById('statusIndicator');
  el.className = 'status ' + state;
  el.textContent = text;
}

function addUserMessage(text) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message user';
  d.innerHTML = '<div class="sender">You</div><div class="text">' + esc(text) + '</div>';
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
}

function startAiMessage() {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = '<div class="sender">AI Assistant</div><div class="text"><span class="cursor"></span></div>';
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
  currentAiMessageEl = d.querySelector('.text');
  currentAiText = '';
  return d;
}

function appendToken(token) {
  if (!currentAiMessageEl) return;
  currentAiText += token;
  currentAiMessageEl.innerHTML = renderMarkdown(currentAiText) + '<span class="cursor"></span>';
  var m = document.getElementById('messages');
  m.scrollTop = m.scrollHeight;
}

function finishAiMessage() {
  if (currentAiMessageEl) {
    currentAiMessageEl.innerHTML = renderMarkdown(currentAiText);
  }
  currentAiMessageEl = null;
  currentAiText = '';
}

function setGenerating(val) {
  isGenerating = val;
  document.getElementById('sendBtn').disabled = val;
  document.getElementById('prompt').disabled = val;
  document.getElementById('cancelBtn').className = 'cancel' + (val ? ' active' : '');
  var btns = document.querySelectorAll('#quickActions button');
  for (var i = 0; i < btns.length; i++) btns[i].disabled = val;
}

function updateContextBar() {
  var el = document.getElementById('contextBar');
  if (!el) return;
  var n = conversationHistory.length;
  if (n === 0) {
    el.textContent = '';
  } else {
    var chars = 0;
    for (var i = 0; i < n; i++) chars += conversationHistory[i].content.length;
    el.textContent = n + ' msgs \u00b7 ~' + Math.round(chars / 4) + ' tokens';
  }
}

// ---- Context Window Management ----

function trimHistory() {
  if (conversationHistory.length > MAX_HISTORY_MESSAGES) {
    conversationHistory = conversationHistory.slice(
        conversationHistory.length - MAX_HISTORY_MESSAGES);
  }
}

function buildHistoryString() {
  var s = '';
  for (var i = 0; i < conversationHistory.length; i++) {
    var msg = conversationHistory[i];
    if (msg.role === 'user') {
      s += '<|user|>\n' + msg.content + '</s>\n';
    } else {
      s += '<|assistant|>\n' + msg.content + '</s>\n';
    }
  }
  return s;
}

// ---- Core Functions ----

function sendMessage() {
  if (isGenerating) return;
  var input = document.getElementById('prompt');
  var text = input.value.trim();
  if (!text) return;

  addUserMessage(text);
  conversationHistory.push({role: 'user', content: text});
  trimHistory();
  input.value = '';
  setGenerating(true);
  startAiMessage();

  // Build history from all messages except the last user message
  var historyForPrompt = '';
  if (conversationHistory.length > 1) {
    var prev = conversationHistory.slice(0, conversationHistory.length - 1);
    for (var i = 0; i < prev.length; i++) {
      var m = prev[i];
      if (m.role === 'user') historyForPrompt += '<|user|>\n' + m.content + '</s>\n';
      else historyForPrompt += '<|assistant|>\n' + m.content + '</s>\n';
    }
  }

  // Fetch page context then send prompt
  sendWithPromise('getPageContext').then(function(ctx) {
    var pageCtx = '';
    if (ctx && ctx.has_context) {
      pageCtx = ctx.title + ' (' + ctx.url + ')';
    }
    return sendWithPromise('sendPrompt', text, historyForPrompt, pageCtx);
  }).then(function(result) {
    var aiText = currentAiText.replace(/<\/s>\s*$/g, '').replace(/<\/s>/g, '').trim();
    if (aiText) conversationHistory.push({role: 'assistant', content: aiText});
    finishAiMessage();
    setGenerating(false);
    updateContextBar();
    if (!result.success && result.error) {
      addErrorMessage(result.error);
    }
  }).catch(function(err) {
    finishAiMessage();
    setGenerating(false);
    updateContextBar();
    addErrorMessage('Error: ' + (err || 'Unknown error'));
  });
}

function addErrorMessage(text) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = '<div class="sender" style="color:#f87171">Error</div><div class="text" style="border-color:#f87171;color:#f87171">' + esc(text) + '</div>';
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
}

// ---- Quick Actions with Page Content ----

function quickAction(action) {
  if (isGenerating) return;
  if (action === 'summarize') {
    // Extract actual page content for better summarization
    sendWithPromise('getPageContent').then(function(result) {
      var prompt;
      if (result.has_content) {
        prompt = 'Summarize this page:\n\nTitle: ' +
                 (result.title || 'Unknown') + '\nURL: ' + (result.url || '') +
                 '\n\nContent:\n' + result.content.substring(0, 3000);
      } else {
        prompt = 'Summarize this page';
      }
      document.getElementById('prompt').value = prompt;
      sendMessage();
    }).catch(function() {
      document.getElementById('prompt').value = 'Summarize this page';
      sendMessage();
    });
  } else if (action === 'extract') {
    sendWithPromise('getPageContent').then(function(result) {
      var prompt;
      if (result.has_content) {
        prompt = 'Extract the key data, facts, and figures from this content:\n\n' +
                 result.content.substring(0, 3000);
      } else {
        prompt = 'Extract key data from this page';
      }
      document.getElementById('prompt').value = prompt;
      sendMessage();
    }).catch(function() {
      document.getElementById('prompt').value = 'Extract key data from this page';
      sendMessage();
    });
  } else if (action === 'explain') {
    sendWithPromise('getPageContent').then(function(result) {
      var prompt;
      if (result.has_content) {
        prompt = 'Explain this simply in plain language:\n\n' +
                 result.content.substring(0, 2000);
      } else {
        prompt = 'Explain this page simply';
      }
      document.getElementById('prompt').value = prompt;
      sendMessage();
    }).catch(function() {
      document.getElementById('prompt').value = 'Explain this page simply';
      sendMessage();
    });
  } else {
    document.getElementById('prompt').value = 'Translate this page to English';
    sendMessage();
  }
}

// ---- New Chat ----

function newChat() {
  conversationHistory = [];
  var m = document.getElementById('messages');
  m.innerHTML = '<div class="message ai"><div class="sender">AI Assistant</div>' +
    '<div class="text">New conversation started. How can I help?</div></div>';
  updateContextBar();
}

// ---- Cancel ----

function cancelGeneration() {
  chrome.send('cancelGeneration', []);
}

// ---- Model Management ----

function toggleModelPanel() {
  var p = document.getElementById('modelPanel');
  if (p.classList.contains('open')) {
    p.classList.remove('open');
  } else {
    p.classList.add('open');
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
        actionBtns = '';
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
        '<div style="margin-top:4px">' + statusBadge + '</div>' +
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
  // Show progress immediately
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.add('active');

  sendWithPromise('downloadModel', modelId).then(function(r) {
    if (r.success) {
      refreshModelList();
    } else {
      addErrorMessage('Download failed: ' + (r.error || 'Unknown error'));
      refreshModelList();
    }
  }).catch(function(e) {
    addErrorMessage('Download error: ' + e);
    refreshModelList();
  });
}

function loadModel(modelId) {
  setStatus('loading', 'Loading ' + modelId + '...');
  sendWithPromise('loadModel', modelId).then(function(r) {
    if (r.success) {
      setStatus('ready', 'Model Ready');
      refreshModelList();
    } else {
      setStatus('error', 'Load failed');
    }
  });
}

function deleteModel(modelId) {
  sendWithPromise('deleteModel', modelId).then(function(r) {
    if (r.success) {
      refreshModelList();
    }
  });
}

// ---- Event Listeners ----

// Token streaming
cr.addWebUiListener('ai-token', function(token, isDone) {
  if (token && token !== '') {
    appendToken(token);
  }
});

// Model status changes
cr.addWebUiListener('model-status', function(status, detail) {
  if (status === 'loading') {
    setStatus('loading', 'Loading model...');
  } else if (status === 'ready') {
    setStatus('ready', 'Model Ready');
  } else if (status === 'error') {
    setStatus('error', 'Error: ' + (detail || 'Unknown'));
  }
});

// Download progress
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

// Download complete
cr.addWebUiListener('download-complete', function(modelId, success) {
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.remove('active');
  if (success) refreshModelList();
});

// Keyboard shortcut
document.getElementById('prompt').addEventListener('keydown', function(e) {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    sendMessage();
  }
});

// ---- Initialization ----

(function init() {
  setStatus('loading', 'Initializing...');

  sendWithPromise('initChat').then(function(info) {
    // Update hardware bar
    if (info.has_gpu) {
      document.getElementById('hwGpu').textContent = info.gpu_backend.toUpperCase();
    }
    document.getElementById('hwRam').textContent = info.total_ram_gb + 'GB RAM';
    document.getElementById('hwCores').textContent = info.cpu_cores + ' cores';

    // Check model status
    if (info.model_loaded) {
      setStatus('ready', 'Model Ready');
    } else {
      var downloaded = (info.models || []).filter(function(m) { return m.is_downloaded; });
      if (downloaded.length > 0) {
        setStatus('offline', 'Model available \u2014 send a message to start');
      } else {
        setStatus('error', 'No models \u2014 click Models to download');
      }
    }
  }).catch(function() {
    setStatus('error', 'Failed to initialize');
  });
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

MoltAIChatUI::MoltAIChatUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  // Add the HTML data source
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltAIChatDataSource>());

  // Add the message handler that bridges JS <-> BrowserAIRuntime
  Profile* profile = Profile::FromWebUI(web_ui);
  web_ui->AddMessageHandler(std::make_unique<MoltAIChatHandler>(profile));
}

MoltAIChatUI::~MoltAIChatUI() = default;
