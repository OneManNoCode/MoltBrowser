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
.status-bar{display:flex;align-items:center;gap:8px;margin-bottom:30px;font-size:12px}
.status-dot{width:8px;height:8px;border-radius:50%;transition:background 0.3s}
.status-dot.ready{background:#4ade80}
.status-dot.loading{background:#fbbf24;animation:pulse 1s infinite}
.status-dot.error{background:#f87171}
.status-dot.offline{background:#888}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.status-text{color:#888;transition:color 0.3s}
.hw-info{color:#555;font-size:11px;display:flex;gap:10px;margin-bottom:10px}
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
</style>
</head>
<body>
<div class="logo">MoltBrowser AI</div>
<div class="subtitle">Local AI — Private by Design — Powered by llama.cpp</div>
<div class="hw-info" id="hwInfo"></div>
<div class="status-bar">
  <div class="status-dot offline" id="statusDot"></div>
  <div class="status-text" id="statusText">Initializing...</div>
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
<script>
// ============================================================
// MoltBrowser AI Full Page — WebUI JavaScript
// Same IPC pattern as side panel chat
// ============================================================

var isGenerating = false;
var currentAiEl = null;
var currentAiText = '';
var idCounter = 0;
var pendingCbs = {};

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

function doSend() {
  if (isGenerating) return;
  var inp = document.getElementById('prompt');
  var t = inp.value.trim(); if (!t) return;
  addUserMsg(t); inp.value = '';
  setGen(true); startAiMsg();
  sendWithPromise('sendPrompt', t).then(function(r) {
    finishAiMsg(); setGen(false);
    if (!r.success && r.error) addErr(r.error);
  }).catch(function(e) { finishAiMsg(); setGen(false); addErr('Error: ' + e); });
}

function addErr(text) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = '<div class="label" style="color:#f87171">Error</div><div class="text" style="border-color:#f87171;color:#f87171">' + esc(text) + '</div>';
  m.appendChild(d); m.scrollTop = m.scrollHeight;
}

function cancelGeneration() { chrome.send('cancelGeneration', []); }

// Streaming tokens
cr.addWebUiListener('ai-token', function(tok, isDone) { if (tok) appendTok(tok); });
cr.addWebUiListener('model-status', function(st, detail) {
  if (st === 'loading') setStatus('loading', 'Loading model...');
  else if (st === 'ready') setStatus('ready', 'Model Ready');
  else if (st === 'error') setStatus('error', detail || 'Error');
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
    document.getElementById('hwInfo').textContent = hw.join(' · ');
    if (info.model_loaded) {
      setStatus('ready', 'Model Ready');
    } else {
      var dl = (info.models || []).filter(function(m) { return m.is_downloaded; });
      if (dl.length > 0) setStatus('offline', 'Model available — type to start');
      else setStatus('error', 'No models downloaded');
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

  // Add the message handler that bridges JS ↔ BrowserAIRuntime
  Profile* profile = Profile::FromWebUI(web_ui);
  web_ui->AddMessageHandler(std::make_unique<MoltAIChatHandler>(profile));
}

MoltAIUI::~MoltAIUI() = default;
