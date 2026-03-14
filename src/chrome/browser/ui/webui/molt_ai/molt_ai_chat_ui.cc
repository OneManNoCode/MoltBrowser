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
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d0d0d;color:#e0e0e0;height:100vh;display:flex;flex-direction:column}
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
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.hw-bar{padding:6px 16px;background:#0a0a0a;border-bottom:1px solid #1a1a1a;font-size:10px;color:#555;display:flex;gap:12px}
.hw-bar span{display:flex;align-items:center;gap:3px}
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
</style>
</head>
<body>
<div class="header">
  <div class="title">MoltBrowser AI</div>
  <div class="status offline" id="statusIndicator">Initializing...</div>
</div>
<div class="hw-bar" id="hwBar">
  <span id="hwGpu"></span>
  <span id="hwRam"></span>
  <span id="hwCores"></span>
</div>
<div class="messages" id="messages">
  <div class="message ai">
    <div class="sender">AI Assistant</div>
    <div class="text">Welcome! I'm your local AI assistant running entirely on this device. Send a message to begin.</div>
  </div>
</div>
<div class="actions" id="quickActions">
  <button onclick="quickAction('Summarize this page')">Summarize</button>
  <button onclick="quickAction('Extract key data')">Extract Data</button>
  <button onclick="quickAction('Explain this simply')">Explain</button>
  <button onclick="quickAction('Translate to English')">Translate</button>
</div>
<div class="input-area">
  <input type="text" id="prompt" placeholder="Ask MoltBrowser AI..." autofocus>
  <button class="cancel" id="cancelBtn" onclick="cancelGeneration()">Stop</button>
  <button class="send" id="sendBtn" onclick="sendMessage()">Send</button>
</div>
<script>
// ============================================================
// MoltBrowser AI Chat — WebUI JavaScript
// Communicates with MoltAIChatHandler via chrome.send()
// and cr.addWebUiListener() / cr.webUIListenerCallback()
// ============================================================

var isGenerating = false;
var currentAiMessageEl = null;
var currentAiText = '';
var promptIdCounter = 0;

// cr.sendWithPromise polyfill for chrome:// pages
// Maps callback IDs to promise resolvers
var pendingCallbacks = {};

function sendWithPromise(method) {
  var args = Array.prototype.slice.call(arguments, 1);
  var id = method + '_' + (++promptIdCounter);
  return new Promise(function(resolve, reject) {
    pendingCallbacks[id] = {resolve: resolve, reject: reject};
    chrome.send(method, [id].concat(args));
  });
}

// cr.webUIResponse — called by C++ ResolveJavascriptCallback
window.cr = window.cr || {};
cr.webUIResponse = function(id, success, response) {
  var cb = pendingCallbacks[id];
  if (cb) {
    delete pendingCallbacks[id];
    if (success) {
      cb.resolve(response);
    } else {
      cb.reject(response);
    }
  }
};

// cr.webUIListenerCallback — called by C++ FireWebUIListener
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
  // Re-render with cursor at end
  currentAiMessageEl.innerHTML = esc(currentAiText) + '<span class="cursor"></span>';
  var m = document.getElementById('messages');
  m.scrollTop = m.scrollHeight;
}

function finishAiMessage() {
  if (currentAiMessageEl) {
    // Remove cursor
    currentAiMessageEl.innerHTML = esc(currentAiText);
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

// ---- Core Functions ----

function sendMessage() {
  if (isGenerating) return;
  var input = document.getElementById('prompt');
  var text = input.value.trim();
  if (!text) return;

  addUserMessage(text);
  input.value = '';
  setGenerating(true);
  startAiMessage();

  sendWithPromise('sendPrompt', text).then(function(result) {
    finishAiMessage();
    setGenerating(false);
    if (!result.success && result.error) {
      addErrorMessage(result.error);
    }
  }).catch(function(err) {
    finishAiMessage();
    setGenerating(false);
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

function quickAction(text) {
  document.getElementById('prompt').value = text;
  sendMessage();
}

function cancelGeneration() {
  chrome.send('cancelGeneration', []);
}

// ---- Event Listeners ----

// Token streaming from BrowserAIRuntime
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

    // Check if any model is already loaded
    if (info.model_loaded) {
      setStatus('ready', 'Model Ready');
    } else {
      // Check for downloaded models
      var downloaded = (info.models || []).filter(function(m) { return m.is_downloaded; });
      if (downloaded.length > 0) {
        setStatus('offline', 'Model available — send a message to start');
      } else {
        setStatus('error', 'No models downloaded');
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

  // Add the message handler that bridges JS ↔ BrowserAIRuntime
  Profile* profile = Profile::FromWebUI(web_ui);
  web_ui->AddMessageHandler(std::make_unique<MoltAIChatHandler>(profile));
}

MoltAIChatUI::~MoltAIChatUI() = default;
