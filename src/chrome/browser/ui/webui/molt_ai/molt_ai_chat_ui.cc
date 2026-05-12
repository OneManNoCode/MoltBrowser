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
/* Model chip — compact side panel variant */
.model-chip-wrap{position:relative;padding:6px 10px;border-bottom:1px solid #1a1a1a}
.model-chip{display:flex;align-items:center;gap:6px;padding:6px 10px;border-radius:16px;background:#1a1a2e;border:1px solid #2a2a3e;color:#e0e0e0;font-size:12px;font-weight:500;cursor:pointer;transition:all 0.2s;width:100%}
.model-chip:hover{border-color:#6366f1;background:#202036}
.model-chip .icon{font-size:13px}
.model-chip .name{flex:1;text-align:left;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.model-chip .chevron{font-size:9px;color:#888;transition:transform 0.2s}
.model-chip.open .chevron{transform:rotate(180deg)}
.model-chip-progress{position:absolute;left:14px;top:50%;transform:translateY(-50%);width:18px;height:18px;display:none}
.model-chip.downloading .icon{display:none}
.model-chip.downloading .model-chip-progress{display:block}
.model-chip-progress svg{transform:rotate(-90deg)}
.model-chip-progress circle{fill:none;stroke:#222;stroke-width:2}
.model-chip-progress .fg{stroke:#8b5cf6;stroke-dasharray:43.98;stroke-dashoffset:43.98;transition:stroke-dashoffset 0.3s}
.model-chip-progress .pct{position:absolute;top:0;left:0;width:18px;height:18px;display:flex;align-items:center;justify-content:center;font-size:7px;font-weight:700;color:#a855f7}
.model-chip-dropdown{position:absolute;top:calc(100% + 4px);left:10px;right:10px;background:#0d0d18;border:1px solid #2a2a3e;border-radius:10px;padding:4px;max-height:340px;overflow-y:auto;z-index:50;display:none;box-shadow:0 6px 30px rgba(0,0,0,0.5)}
.model-chip-dropdown.open{display:block}
.model-chip-item{display:flex;align-items:center;gap:8px;padding:8px 10px;border-radius:6px;cursor:pointer;transition:background 0.15s}
.model-chip-item:hover{background:#1a1a2e}
.model-chip-item.active{background:#1a2e1a}
.model-chip-item .mname{flex:1;font-size:12px;color:#e0e0e0}
.model-chip-item .msize{font-size:10px;color:#666}
.model-chip-item .mstatus{font-size:9px;padding:2px 6px;border-radius:8px;font-weight:600;text-transform:uppercase}
.model-chip-item .mstatus.active{background:#1a3a1a;color:#4ade80}
.model-chip-item .mstatus.downloaded{background:#1a1a3a;color:#8b5cf6}
.model-chip-item .mstatus.available{background:#1a1a1a;color:#666}
.model-chip-item .mstatus.downloading{background:#3a2e1a;color:#fbbf24}
/* First-Run Welcome */
.welcome-overlay{position:absolute;top:0;left:0;right:0;bottom:0;background:#0d0d0d;z-index:20;display:none;flex-direction:column;align-items:center;justify-content:center;padding:24px;text-align:center}
.welcome-overlay.open{display:flex}
.welcome-logo{font-size:24px;font-weight:700;background:linear-gradient(135deg,#6366f1,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:8px}
.welcome-text{color:#888;font-size:13px;max-width:320px;line-height:1.6;margin-bottom:16px}
.welcome-features{text-align:left;max-width:280px;margin-bottom:20px}
.welcome-features div{padding:6px 0;font-size:12px;color:#aaa;display:flex;align-items:center;gap:8px}
.welcome-features .feat-icon{color:#6366f1;font-size:14px}
.welcome-btn{padding:12px 32px;border-radius:10px;border:none;background:linear-gradient(135deg,#6366f1,#8b5cf6);color:white;font-size:14px;font-weight:600;cursor:pointer;transition:opacity 0.2s;margin-bottom:8px}
.welcome-btn:hover{opacity:0.85}
.welcome-skip{color:#666;font-size:11px;cursor:pointer;border:none;background:none;padding:4px}
.welcome-skip:hover{color:#aaa}
.welcome-progress{width:100%;max-width:280px;margin-top:12px;display:none}
.welcome-progress.active{display:block}
.welcome-pbar{height:6px;border-radius:3px;background:#222;overflow:hidden}
.welcome-pfill{height:100%;background:linear-gradient(90deg,#6366f1,#a855f7);transition:width 0.3s;width:0}
.welcome-ptext{font-size:11px;color:#888;margin-top:4px}
/* Code block copy button */
.code-wrap{position:relative;margin:8px 0}
.code-wrap pre{margin:0}
.code-copy{position:absolute;top:4px;right:4px;padding:2px 8px;border-radius:4px;border:1px solid #444;background:#222;color:#888;font-size:10px;cursor:pointer;opacity:0;transition:opacity 0.2s}
.code-wrap:hover .code-copy{opacity:1}
.code-copy:hover{color:#e0e0e0;border-color:#6366f1}
.code-copy.copied{color:#4ade80;border-color:#4ade80}
/* Message actions */
.msg-actions{display:flex;gap:4px;margin-top:6px;opacity:0;transition:opacity 0.2s}
.message:hover .msg-actions{opacity:1}
.molt-action-summary{margin-top:8px;padding:6px 10px;background:#1a2a3a;
  color:#7dd3fc;font-size:12px;border-radius:6px;border:1px solid #2a3a4a;
  display:inline-block;font-family:-apple-system,system-ui,sans-serif;}
.molt-action-summary span{margin-right:6px;}
.molt-action-result{margin:4px 0 4px 36px;padding:4px 10px;border-radius:6px;
  font-size:11px;font-family:-apple-system,system-ui,sans-serif;
  display:inline-block;}
.molt-action-result.ok{background:#0f2818;color:#86efac;border:1px solid #1a3a28;}
.molt-action-result.fail{background:#2a1010;color:#fca5a5;border:1px solid #4a1a1a;}
.molt-action-result .detail{opacity:0.7;font-size:10px;margin-left:6px;}
.msg-action{padding:2px 8px;border-radius:4px;border:1px solid #333;background:none;color:#666;font-size:10px;cursor:pointer}
.msg-action:hover{color:#e0e0e0;border-color:#6366f1}
/* Search bar */
.search-bar{padding:6px 16px;border-bottom:1px solid #1a1a1a;display:none}
.search-bar.open{display:flex}
.search-bar input{flex:1;padding:6px 10px;border-radius:6px;border:1px solid #333;background:#111;color:#e0e0e0;font-size:12px;outline:none}
.search-bar input:focus{border-color:#6366f1}
.search-bar .search-count{font-size:10px;color:#888;padding:6px 8px}
.search-bar .search-close{background:none;border:none;color:#888;cursor:pointer;font-size:14px;padding:4px}
.highlight{background:#6366f1;color:#fff;border-radius:2px;padding:0 2px}
</style>
</head>
<body>
<div class="header">
  <div class="title">MoltBrowser AI</div>
  <div class="status offline" id="statusIndicator">Initializing...</div>
  <div class="header-actions">
    <button class="icon-btn" onclick="newChat()" title="New Chat">New</button>
    <button class="icon-btn" onclick="toggleSearch()" title="Search (Cmd+F)">&#128269;</button>
    <button class="icon-btn" onclick="exportChat()" title="Export Chat">&#128190;</button>
    <button class="icon-btn" onclick="importChat()" title="Import Chat">&#128194;</button>
    <button class="icon-btn" onclick="toggleModelPanel()" title="Manage Models">Manage</button>
    <button class="icon-btn" onclick="window.open('molt://ai-settings/')" title="Settings">&#9881;</button>
  </div>
</div>
<!-- Always-visible model selector chip -->
<div class="model-chip-wrap">
  <button class="model-chip" id="modelChip" onclick="toggleModelDropdown(event)">
    <span class="icon">&#129302;</span>
    <span class="model-chip-progress" id="modelChipProgress">
      <svg width="18" height="18" viewBox="0 0 18 18">
        <circle cx="9" cy="9" r="7"></circle>
        <circle class="fg" id="modelChipProgressFg" cx="9" cy="9" r="7"></circle>
      </svg>
      <span class="pct" id="modelChipPct">0%</span>
    </span>
    <span class="name" id="modelChipName">No Model</span>
    <span class="chevron">&#9662;</span>
  </button>
  <div class="model-chip-dropdown" id="modelChipDropdown"></div>
</div>
<div class="search-bar" id="searchBar">
  <input type="text" id="searchInput" placeholder="Search messages..." oninput="doSearch()">
  <span class="search-count" id="searchCount"></span>
  <button class="search-close" onclick="toggleSearch()">&times;</button>
</div>
<!-- Tab context strip: shows the URL/title the chat is grounded in.
     Populated by the native AiChatSidePanelWebView via the
     window.__moltSetTabContext({url,title}) contract whenever the
     active tab changes. -->
<div class="tab-context" id="tabContext" style="display:none">
  <span class="tab-context-icon">&#128279;</span>
  <span class="tab-context-label" id="tabContextLabel"></span>
</div>
<script>
  // Tab-context contract. Native side calls this on every active-tab
  // change. The same payload is also stashed at
  // window.__moltLastTabContext for late-binding (race-free init).
  window.__moltSetTabContext = function(ctx) {
    window.__moltCurrentTabContext = ctx || null;
    var bar = document.getElementById('tabContext');
    var lbl = document.getElementById('tabContextLabel');
    if (!bar || !lbl) return;
    if (!ctx || !ctx.url ||
        ctx.url.indexOf('chrome://') === 0 ||
        ctx.url.indexOf('molt://')   === 0 ||
        ctx.url === 'about:blank') {
      bar.style.display = 'none';
      return;
    }
    var host = ctx.url;
    try { host = new URL(ctx.url).host; } catch (e) {}
    var title = ctx.title || host;
    lbl.textContent = 'Chatting about: ' + title +
        (host && host !== title ? '  \u2022  ' + host : '');
    lbl.title = ctx.url;
    bar.style.display = 'flex';
  };
  if (window.__moltLastTabContext)
    window.__moltSetTabContext(window.__moltLastTabContext);
</script>
<style>
.tab-context{display:flex;align-items:center;gap:8px;
  padding:6px 14px;font-size:11px;color:#8a8a98;
  background:#0d0d14;border-bottom:1px solid #1a1a2a;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}
.tab-context-icon{flex-shrink:0;}
.tab-context-label{overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap;cursor:default;}
</style>
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

<!-- First-Run Welcome Overlay -->
<div class="welcome-overlay" id="welcomeOverlay">
  <div class="welcome-logo">Welcome to MoltBrowser AI</div>
  <div class="welcome-text">Your private AI assistant that runs entirely on this device. No data leaves your computer.</div>
  <div class="welcome-features">
    <div><span class="feat-icon">&#9889;</span> Powered by Metal GPU acceleration</div>
    <div><span class="feat-icon">&#128274;</span> 100% local — no cloud, no tracking</div>
    <div><span class="feat-icon">&#128172;</span> Summarize pages, answer questions, write code</div>
    <div><span class="feat-icon">&#129302;</span> Multiple AI models to choose from</div>
  </div>
  <button class="welcome-btn" id="welcomeDownloadBtn" onclick="startFirstRunDownload()">Download TinyLlama (638 MB)</button>
  <button class="welcome-skip" onclick="skipFirstRun()">Skip for now</button>
  <div class="welcome-progress" id="welcomeProgress">
    <div class="welcome-pbar"><div class="welcome-pfill" id="welcomePfill"></div></div>
    <div class="welcome-ptext" id="welcomePtext">Starting download...</div>
  </div>
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
var codeBlockId = 0;
function renderMarkdown(text) {
  // Escape HTML first
  var s = esc(text);
  // Code blocks (``` ... ```) with copy button
  s = s.replace(/```(\w*)\n([\s\S]*?)```/g, function(m, lang, code) {
    var id = 'cb-' + (++codeBlockId);
    var langLabel = lang ? '<span style="position:absolute;top:4px;left:8px;font-size:9px;color:#666;text-transform:uppercase">' + lang + '</span>' : '';
    return '<div class="code-wrap">' + langLabel +
      '<button class="code-copy" onclick="copyCode(\'' + id + '\',this)">Copy</button>' +
      '<pre id="' + id + '" style="background:#1a1a2e;padding:' + (lang ? '22px 10px 10px' : '10px') + ';border-radius:6px;overflow-x:auto;font-size:12px;border:1px solid #2a2a4a"><code>' + code.trim() + '</code></pre></div>';
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
  // P3: reset streaming-action parser state for the new response.
  streamLastParsedIdx = 0;
  return d;
}

function appendToken(token) {
  if (!currentAiMessageEl) return;
  currentAiText += token;
  // P3 streaming dispatch: as each token chunk arrives, scan for
  // newly-complete [[ACTION ...]] markers past our last parse index
  // and enqueue them. The dispatcher drains the queue sequentially
  // so a chain of "type → click → wait-for → click" runs in order
  // even though the LLM streams them.
  scanStreamForActions();
  // Render with action tokens stripped so the user never sees them.
  var visible = currentAiText.replace(ACTION_TOKEN_REGEX, '').trim();
  currentAiMessageEl.innerHTML = renderMarkdown(visible) + '<span class="cursor"></span>';
  var m = document.getElementById('messages');
  m.scrollTop = m.scrollHeight;
}

function finishAiMessage() {
  if (currentAiMessageEl) {
    // P3 streaming: tokens were already dispatched in appendToken.
    // Run one final scan in case the last token landed without a
    // followup append (LLM finishes exactly on `]]`).
    scanStreamForActions();
    var visibleText = currentAiText.replace(ACTION_TOKEN_REGEX, '').trim();
    currentAiMessageEl.innerHTML = renderMarkdown(visibleText);
    // Add copy response button
    var actions = document.createElement('div');
    actions.className = 'msg-actions';
    actions.innerHTML = '<button class="msg-action" onclick="copyResponse(this)">Copy response</button>';
    currentAiMessageEl.parentNode.appendChild(actions);
  }
  currentAiMessageEl = null;
  currentAiText = '';
}

// --------------------------------------------------------------
// P3: LLM action-token parser with streaming + sequential dispatch.
//
// Contract — the system prompt teaches the LLM these verbs:
//   [[ACTION click:<sel>]]
//   [[ACTION type:<sel>|<text>]]
//   [[ACTION select:<sel>|<value>]]
//   [[ACTION hover:<sel>]]
//   [[ACTION right-click:<sel>]]
//   [[ACTION drag:<source-sel>|<target-sel>]]
//   [[ACTION scroll:<pixels>]]
//   [[ACTION navigate:<url>]]
//   [[ACTION wait:<ms>]]
//   [[ACTION wait-for:<sel>|<timeout-ms>]]
//
// Lifecycle per response:
//   - startAiMessage resets streamLastParsedIdx to 0.
//   - appendToken calls scanStreamForActions, which exec()s the
//     regex past streamLastParsedIdx. Each complete match (i.e. the
//     trailing `]]` has landed in currentAiText) is parsed and
//     enqueued. The cursor moves to the end of the match.
//   - actionQueue / drainActionQueue runs in the background as an
//     async function: shift one, await runMoltAction, append a
//     [system-message] row with ✓ or ✗, then loop. This serializes
//     long chains like "type ... wait-for ... click".
//   - finishAiMessage runs one last scan and strips tokens from the
//     visible text. The bare prose is what the user sees.
// --------------------------------------------------------------
var ACTION_TOKEN_REGEX = /\[\[ACTION\s+(\w+):([^\]]+)\]\]/g;
var streamLastParsedIdx = 0;
var actionQueue = [];
var actionQueueDraining = false;

function parseActionPayload(verb, args) {
  verb = verb.toLowerCase();
  args = (args || '').trim();
  if (verb === 'click' || verb === 'hover' || verb === 'right-click') {
    return {type: verb, selector: args};
  }
  if (verb === 'type' || verb === 'select' || verb === 'drag') {
    var bar = args.indexOf('|');
    if (bar < 0) return null;
    return {type: verb, selector: args.slice(0, bar).trim(),
            value: args.slice(bar + 1)};
  }
  if (verb === 'scroll') return {type: 'scroll', value: args};
  if (verb === 'wait') return {type: 'wait', value: args || '1000'};
  if (verb === 'wait-for' || verb === 'waitfor') {
    var bar2 = args.indexOf('|');
    if (bar2 < 0) return {type: 'wait-for', selector: args, value: '5000'};
    return {type: 'wait-for', selector: args.slice(0, bar2).trim(),
            value: args.slice(bar2 + 1)};
  }
  if (verb === 'navigate' || verb === 'nav' || verb === 'goto') {
    var url = args.trim();
    if (url && !/^https?:\/\//i.test(url)) url = 'https://' + url;
    return {type: 'navigate', value: url};
  }
  return null;
}

function scanStreamForActions() {
  if (!currentAiText) return;
  ACTION_TOKEN_REGEX.lastIndex = streamLastParsedIdx;
  var m;
  while ((m = ACTION_TOKEN_REGEX.exec(currentAiText)) !== null) {
    var action = parseActionPayload(m[1], m[2]);
    if (action) enqueueAction(action);
    streamLastParsedIdx = m.index + m[0].length;
  }
}

function enqueueAction(action) {
  actionQueue.push(action);
  if (!actionQueueDraining) drainActionQueue();
}

function drainActionQueue() {
  if (actionQueueDraining) return;
  actionQueueDraining = true;
  var loop = function() {
    if (actionQueue.length === 0) {
      actionQueueDraining = false;
      return;
    }
    var action = actionQueue.shift();
    var label = action.type +
                (action.selector ? ' ' + action.selector : '') +
                ((action.value && action.type !== 'wait-for')
                    ? ' = ' + action.value : '');
    sendWithPromise('runMoltAction', action).then(function(r) {
      appendActionResult(r && r.success, label,
                          r ? (r.message || r.error || '') : '');
    }).catch(function(err) {
      appendActionResult(false, label, String(err || 'failed'));
    }).then(loop, loop);
  };
  loop();
}

function appendActionResult(ok, label, detail) {
  var m = document.getElementById('messages');
  if (!m) return;
  var d = document.createElement('div');
  d.className = 'molt-action-result ' + (ok ? 'ok' : 'fail');
  d.innerHTML = (ok ? '&#10003; ' : '&#10007; ') + esc(label) +
                (detail ? ' <span class="detail">(' + esc(detail) + ')</span>' : '');
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
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

// --------------------------------------------------------------
// Slash-command bridge to the active tab.
// Recognized forms (case-insensitive command, free-form remainder):
//   /click <selector>
//   /type <selector> <value...>
//   /scroll [pixels]            (default 600)
//   /navigate <url>
// Returns true if the text was a recognized action and was dispatched;
// the caller should skip the LLM path in that case.
// --------------------------------------------------------------
function tryDispatchActionCommand(text) {
  var m = text.match(/^\s*\/(click|type|select|hover|right-click|rclick|drag|scroll|navigate|nav|goto|wait|wait-for|waitfor)\b\s*(.*)$/i);
  if (!m) return false;
  var cmd = m[1].toLowerCase();
  var rest = (m[2] || '').trim();
  var action = null;
  // Normalize aliases.
  if (cmd === 'rclick') cmd = 'right-click';
  if (cmd === 'waitfor') cmd = 'wait-for';
  if (cmd === 'click' || cmd === 'hover' || cmd === 'right-click') {
    if (!rest) {
      addErrorMessage('Usage: /' + cmd + ' <css-selector>');
      return true;
    }
    action = {type: cmd, selector: rest};
  } else if (cmd === 'type' || cmd === 'select' || cmd === 'drag') {
    var sp = rest.indexOf(' ');
    if (sp < 0) {
      addErrorMessage('Usage: /' + cmd + ' <selector> <' +
                      (cmd === 'drag' ? 'target-selector' : 'value...') + '>');
      return true;
    }
    action = {type: cmd, selector: rest.slice(0, sp),
              value: rest.slice(sp + 1)};
  } else if (cmd === 'scroll') {
    action = {type: 'scroll', value: rest || '600'};
  } else if (cmd === 'navigate' || cmd === 'nav' || cmd === 'goto') {
    if (!rest) {
      addErrorMessage('Usage: /navigate <url>');
      return true;
    }
    if (!/^https?:\/\//i.test(rest)) rest = 'https://' + rest;
    action = {type: 'navigate', value: rest};
  } else if (cmd === 'wait') {
    action = {type: 'wait', value: rest || '1000'};
  } else if (cmd === 'wait-for') {
    var sp2 = rest.indexOf(' ');
    if (!rest) {
      addErrorMessage('Usage: /wait-for <selector> [timeout-ms]');
      return true;
    }
    if (sp2 < 0) {
      action = {type: 'wait-for', selector: rest, value: '5000'};
    } else {
      action = {type: 'wait-for', selector: rest.slice(0, sp2),
                value: rest.slice(sp2 + 1)};
    }
  }
  if (!action) return false;

  addUserMessage(text);
  // Show a transient "running" status as an AI message we'll update.
  startAiMessage();
  appendToAiMessage('Running ' + cmd + '...');

  sendWithPromise('runMoltAction', action).then(function(r) {
    var msg = r.success
        ? '\u2713 ' + (r.message || (cmd + ' ok'))
        : '\u2717 ' + (r.message || r.error || cmd + ' failed');
    currentAiText = msg;
    finishAiMessage();
    setGenerating(false);
  }).catch(function(err) {
    currentAiText = '\u2717 ' + (err || 'action failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// P3: page text chunking + relevance ranking.
//
// Splits captured innerText into ~chunkSize-char chunks at sentence
// boundaries so chunks read naturally (LLMs reason better over
// complete sentences than mid-word slices). The ranker scores each
// chunk by unique-token overlap with the query — fast, deterministic,
// no embedding needed. Stop-words are ignored to make the scoring
// behave on common queries like "what does the page say about X".
// --------------------------------------------------------------
var MOLT_STOP_WORDS = {
  'a':1,'an':1,'the':1,'and':1,'or':1,'but':1,'if':1,'of':1,'to':1,
  'in':1,'on':1,'at':1,'is':1,'are':1,'was':1,'were':1,'be':1,'been':1,
  'do':1,'does':1,'did':1,'has':1,'have':1,'had':1,'this':1,'that':1,
  'it':1,'its':1,'for':1,'with':1,'as':1,'by':1,'from':1,'into':1,
  'what':1,'how':1,'why':1,'when':1,'where':1,'who':1,'which':1,
  'about':1,'me':1,'my':1,'i':1,'you':1,'your':1,'we':1,'our':1
};

function chunkPageText(text, chunkSize) {
  if (!text) return [];
  chunkSize = chunkSize || 600;
  // Sentence-ish split: period/!/? followed by whitespace, also
  // line breaks. Lookbehind is supported in every Chromium-era JS.
  var sentences = text.split(/(?<=[.!?])\s+|\n+/);
  var chunks = [];
  var current = '';
  for (var i = 0; i < sentences.length; i++) {
    var s = sentences[i].trim();
    if (!s) continue;
    if ((current.length + s.length + 1) > chunkSize && current) {
      chunks.push(current);
      current = s;
    } else {
      current += (current ? ' ' : '') + s;
    }
  }
  if (current) chunks.push(current);
  return chunks;
}

function rankChunksByQuery(chunks, query, topK) {
  topK = topK || 5;
  if (!chunks.length) return [];
  var qTokens = (query || '').toLowerCase().match(/[a-z0-9]{2,}/g) || [];
  var qSet = {};
  qTokens.forEach(function(t){ if (!MOLT_STOP_WORDS[t]) qSet[t] = true; });
  // If the query gave us no useful keywords (e.g. "summarize this"),
  // fall back to the head of the document — first paragraph is
  // usually the most informative.
  if (!Object.keys(qSet).length) return chunks.slice(0, topK);
  var scored = chunks.map(function(c, idx){
    var tokens = c.toLowerCase().match(/[a-z0-9]{2,}/g) || [];
    var hits = 0;
    var seen = {};
    for (var i = 0; i < tokens.length; i++) {
      if (qSet[tokens[i]] && !seen[tokens[i]]) {
        hits++;
        seen[tokens[i]] = true;
      }
    }
    return {chunk: c, score: hits, idx: idx};
  });
  scored.sort(function(a,b){ return b.score - a.score || a.idx - b.idx; });
  var hits = scored.filter(function(s){ return s.score > 0; })
                   .slice(0, topK);
  // If nothing matched at all, fall back to head — still useful.
  if (!hits.length) return chunks.slice(0, topK);
  return hits.map(function(s){ return s.chunk; });
}

function sendMessage() {
  if (isGenerating) return;
  var input = document.getElementById('prompt');
  var text = input.value.trim();
  if (!text) return;

  // Slash-command shortcut: any `/click /type /scroll /navigate` line
  // bypasses the LLM and runs as a one-shot automation action on the
  // active tab. Lets the user drive page actions in plain language
  // without waiting on inference. The LLM-emit-actions path is a
  // follow-up that wraps the same runMoltAction IPC.
  if (text.charAt(0) === '/' && tryDispatchActionCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }

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

  // P3: build a richer page+memory context:
  //   - Active page: chunk the captured innerText (up to 50KB now)
  //     into ~600-char sentence-aware chunks, rank by keyword overlap
  //     with the user's query, take top-5. This beats blind head-
  //     truncation when the answer is deep in a long article.
  //   - Personal Vector Memory: query MemoryService for top-3 hits
  //     from the user's full browsing history so cross-page recall
  //     ("what was that REIT article I read last week") works.
  sendWithPromise('getPageContext').then(function(ctx) {
    var pageCtx = '';
    if (ctx && ctx.has_context) {
      pageCtx = ctx.title + ' (' + ctx.url + ')';
    }
    var sp = window.__moltCurrentTabContext;
    if (sp && sp.text && sp.text.length > 0) {
      var chunks = chunkPageText(sp.text, 600);
      var top = rankChunksByQuery(chunks, text, 5);
      if (top.length) {
        pageCtx = (pageCtx ? (pageCtx + '\n\n') : '') +
                  'Active page content (most relevant excerpts):\n' +
                  top.map(function(c){ return '- ' + c; }).join('\n');
      }
    }
    // Personal Vector Memory grounding. Failures here are non-fatal
    // — we still want to send the prompt even if memory is empty/off.
    return sendWithPromise('queryMemory', text, 3).then(function(mem) {
      if (mem && mem.hits && mem.hits.length) {
        pageCtx += (pageCtx ? '\n\n' : '') + 'Relevant past reading:\n' +
            mem.hits.map(function(h){
              var snip = (h.snippet || '').replace(/\s+/g, ' ').slice(0, 200);
              return '- ' + (h.title || h.url) + ' (' + h.url + '): ' + snip;
            }).join('\n');
      }
      return sendWithPromise('sendPrompt', text, historyForPrompt, pageCtx);
    }, function() {
      // queryMemory failed (no service yet, etc.) — proceed without it.
      return sendWithPromise('sendPrompt', text, historyForPrompt, pageCtx);
    });
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

  // Disable download buttons and show cancel
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

function cancelDownload() {
  chrome.send('cancelDownload', []);
}

function loadModel(modelId) {
  setStatus('loading', 'Loading ' + modelId + '...');
  sendWithPromise('loadModel', modelId).then(function(r) {
    if (r.success) {
      setStatus('ready', 'Model Ready');
      refreshModelList();
      sendWithPromise('getModelStatus').then(function(rr) {
        allModels = rr.models || [];
        refreshModelChip();
      });
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

// ---- Model Chip (compact side panel selector) ----
var allModels = [];
var activeModelId = null;
var downloadingModelId = null;

function refreshModelChip() {
  var nameEl = document.getElementById('modelChipName');
  if (!nameEl) return;
  var active = allModels.find(function(m){ return m.is_loaded; });
  if (active) {
    activeModelId = active.model_id;
    nameEl.textContent = active.display_name || active.model_id;
  } else {
    var first = allModels.find(function(m){ return m.is_downloaded; });
    nameEl.textContent = first ? ((first.display_name || first.model_id) + ' (tap to load)') : 'Choose Model';
  }
}

function toggleModelDropdown(ev) {
  if (ev) ev.stopPropagation();
  var chip = document.getElementById('modelChip');
  var dd = document.getElementById('modelChipDropdown');
  if (!chip || !dd) return;
  var willOpen = !dd.classList.contains('open');
  dd.classList.toggle('open');
  chip.classList.toggle('open');
  if (willOpen) renderModelChipDropdown();
}

function renderModelChipDropdown() {
  var dd = document.getElementById('modelChipDropdown');
  if (!dd) return;
  if (allModels.length === 0) {
    dd.innerHTML = '<div style="padding:12px;color:#666;font-size:11px;text-align:center">Loading...</div>';
    sendWithPromise('getModelStatus').then(function(r){
      allModels = r.models || [];
      renderModelChipDropdown();
      refreshModelChip();
    });
    return;
  }
  dd.innerHTML = '';
  allModels.forEach(function(m) {
    var item = document.createElement('div');
    item.className = 'model-chip-item';
    var isActive = !!m.is_loaded;
    if (isActive) item.className += ' active';
    var statusClass, statusText;
    if (isActive) { statusClass = 'active'; statusText = 'Active'; }
    else if (downloadingModelId === m.model_id) { statusClass = 'downloading'; statusText = '\u2026'; }
    else if (m.is_downloaded) { statusClass = 'downloaded'; statusText = 'Ready'; }
    else { statusClass = 'available'; statusText = 'Get'; }
    var sizeMB = m.file_size_mb || 0;
    item.innerHTML =
      '<div style="flex:1;min-width:0">' +
        '<div class="mname">' + (m.display_name || m.model_id) + '</div>' +
        '<div class="msize">' + sizeMB + ' MB</div>' +
      '</div>' +
      '<span class="mstatus ' + statusClass + '">' + statusText + '</span>';
    item.onclick = function() {
      if (isActive) { toggleModelDropdown(); return; }
      if (m.is_downloaded) {
        loadModel(m.model_id);
      } else {
        downloadingModelId = m.model_id;
        downloadModel(m.model_id);
        document.getElementById('modelChip').classList.add('downloading');
      }
      toggleModelDropdown();
    };
    dd.appendChild(item);
  });
}

function updateModelChipProgress(percent) {
  var fg = document.getElementById('modelChipProgressFg');
  var pct = document.getElementById('modelChipPct');
  if (fg) {
    var circumference = 2 * Math.PI * 7; // r=7 in side panel
    var offset = circumference - (percent / 100) * circumference;
    fg.style.strokeDasharray = circumference;
    fg.style.strokeDashoffset = offset;
  }
  if (pct) pct.textContent = Math.round(percent) + '%';
}

// Close dropdown on outside click
document.addEventListener('click', function(e) {
  var wrap = document.querySelector('.model-chip-wrap');
  if (wrap && !wrap.contains(e.target)) {
    var dd = document.getElementById('modelChipDropdown');
    var chip = document.getElementById('modelChip');
    if (dd) dd.classList.remove('open');
    if (chip) chip.classList.remove('open');
  }
});

// Init chip
sendWithPromise('getModelStatus').then(function(r){
  allModels = r.models || [];
  refreshModelChip();
});

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

// Download progress with speed and ETA
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
      var mbps = (speed / 1048576).toFixed(1);
      info += ' \u00b7 ' + mbps + ' MB/s';
      if (eta > 0) {
        var mins = Math.floor(eta / 60);
        var secs = Math.round(eta % 60);
        info += ' \u00b7 ' + (mins > 0 ? mins + 'm ' : '') + secs + 's left';
      }
    }
    if (ptext) ptext.textContent = info;
  }
  // Update model chip progress
  if (downloadingModelId === modelId && total > 0) {
    var pct2 = (current / total) * 100;
    updateModelChipProgress(pct2);
    var nameEl = document.getElementById('modelChipName');
    var info2 = allModels.find(function(m){return m.model_id === modelId;});
    if (nameEl && info2) nameEl.textContent = 'Downloading ' + (info2.display_name || modelId);
  }
});

// Download complete
cr.addWebUiListener('download-complete', function(modelId, success) {
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.remove('active');
  // Reset chip download state
  if (downloadingModelId === modelId) {
    downloadingModelId = null;
    var chip = document.getElementById('modelChip');
    if (chip) chip.classList.remove('downloading');
    if (success) {
      sendWithPromise('getModelStatus').then(function(r){
        allModels = r.models || [];
        loadModel(modelId);
        refreshModelChip();
      });
    }
  }
  if (success) {
    refreshModelList();
    // Close welcome overlay if it was open
    var wo = document.getElementById('welcomeOverlay');
    if (wo.classList.contains('open')) {
      wo.classList.remove('open');
      setStatus('offline', 'Model ready \u2014 send a message to start');
    }
  }
});

// Keyboard shortcut
document.getElementById('prompt').addEventListener('keydown', function(e) {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    sendMessage();
  }
});

// ---- First-Run Experience ----

var isFirstRun = false;

function startFirstRunDownload() {
  var btn = document.getElementById('welcomeDownloadBtn');
  btn.disabled = true;
  btn.textContent = 'Downloading...';
  var wp = document.getElementById('welcomeProgress');
  wp.classList.add('active');
  downloadModel('tinyllama-1.1b');
}

function skipFirstRun() {
  document.getElementById('welcomeOverlay').classList.remove('open');
}

// Hook download progress into welcome overlay too
cr.addWebUiListener('download-progress', function(modelId, current, total, speed, eta) {
  var wo = document.getElementById('welcomeOverlay');
  if (wo.classList.contains('open') && total > 0) {
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

// ---- Copy Code / Response ----

function copyCode(id, btn) {
  var pre = document.getElementById(id);
  if (!pre) return;
  var text = pre.textContent;
  navigator.clipboard.writeText(text).then(function() {
    btn.textContent = 'Copied!';
    btn.classList.add('copied');
    setTimeout(function() { btn.textContent = 'Copy'; btn.classList.remove('copied'); }, 1500);
  });
}

function copyResponse(btn) {
  var msg = btn.closest('.message');
  if (!msg) return;
  var textEl = msg.querySelector('.text');
  if (!textEl) return;
  navigator.clipboard.writeText(textEl.innerText).then(function() {
    btn.textContent = 'Copied!';
    setTimeout(function() { btn.textContent = 'Copy response'; }, 1500);
  });
}

// ---- Search Conversation ----

function toggleSearch() {
  var bar = document.getElementById('searchBar');
  if (bar.classList.contains('open')) {
    bar.classList.remove('open');
    clearSearch();
  } else {
    bar.classList.add('open');
    document.getElementById('searchInput').focus();
  }
}

function doSearch() {
  clearSearch();
  var query = document.getElementById('searchInput').value.trim().toLowerCase();
  if (!query) { document.getElementById('searchCount').textContent = ''; return; }
  var msgs = document.querySelectorAll('#messages .message .text');
  var count = 0;
  for (var i = 0; i < msgs.length; i++) {
    var el = msgs[i];
    var html = el.innerHTML;
    var text = el.textContent.toLowerCase();
    if (text.indexOf(query) >= 0) {
      count++;
      // Highlight matches
      var re = new RegExp('(' + query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
      el.innerHTML = el.innerHTML.replace(re, '<span class="highlight">$1</span>');
      if (count === 1) el.scrollIntoView({behavior:'smooth', block:'center'});
    }
  }
  document.getElementById('searchCount').textContent = count + ' match' + (count !== 1 ? 'es' : '');
}

function clearSearch() {
  var highlights = document.querySelectorAll('.highlight');
  for (var i = 0; i < highlights.length; i++) {
    var span = highlights[i];
    span.replaceWith(span.textContent);
  }
}

// ---- Import Chat History ----

function importChat() {
  var input = document.createElement('input');
  input.type = 'file';
  input.accept = '.json';
  input.onchange = function(e) {
    var file = e.target.files[0];
    if (!file) return;
    var reader = new FileReader();
    reader.onload = function(ev) {
      try {
        var data = JSON.parse(ev.target.result);
        if (data.messages && Array.isArray(data.messages)) {
          conversationHistory = data.messages;
          // Rebuild message display
          var m = document.getElementById('messages');
          m.innerHTML = '';
          for (var i = 0; i < data.messages.length; i++) {
            var msg = data.messages[i];
            if (msg.role === 'user') {
              addUserMessage(msg.content);
            } else {
              var d = document.createElement('div');
              d.className = 'message ai';
              d.innerHTML = '<div class="sender">AI Assistant</div><div class="text">' + renderMarkdown(msg.content) + '</div>' +
                '<div class="msg-actions"><button class="msg-action" onclick="copyResponse(this)">Copy response</button></div>';
              m.appendChild(d);
            }
          }
          m.scrollTop = m.scrollHeight;
          updateContextBar();
          addSystemMessage('Imported ' + data.messages.length + ' messages from ' + file.name);
        } else {
          addErrorMessage('Invalid chat export file');
        }
      } catch (err) {
        addErrorMessage('Failed to parse file: ' + err.message);
      }
    };
    reader.readAsText(file);
  };
  input.click();
}

// ---- Export Chat History ----

function exportChat() {
  if (conversationHistory.length === 0) {
    addErrorMessage('No conversation to export');
    return;
  }
  var data = JSON.stringify({
    exported_at: new Date().toISOString(),
    messages: conversationHistory
  }, null, 2);
  sendWithPromise('exportHistory', data).then(function(r) {
    if (r.success) {
      addSystemMessage('Chat exported to: ' + r.filename);
    } else {
      addErrorMessage('Export failed');
    }
  });
}

function addSystemMessage(text) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = '<div class="sender" style="color:#4ade80">System</div><div class="text" style="border-color:#1a2e1a;color:#4ade80;font-size:12px">' + esc(text) + '</div>';
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
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
    quickAction('summarize');
  } else if (e.key === 'E' || e.key === 'e') {
    e.preventDefault();
    quickAction('explain');
  } else if (e.key === 'X' || e.key === 'x') {
    e.preventDefault();
    exportChat();
  } else if (e.key === 'N' || e.key === 'n') {
    e.preventDefault();
    newChat();
  }
});

// Cmd/Ctrl+F for search
document.addEventListener('keydown', function(e) {
  if ((e.metaKey || e.ctrlKey) && (e.key === 'f' || e.key === 'F') && !e.shiftKey) {
    e.preventDefault();
    toggleSearch();
  }
  if (e.key === 'Escape') {
    var bar = document.getElementById('searchBar');
    if (bar.classList.contains('open')) toggleSearch();
  }
});

// ---- Initialization ----

(function init() {
  setStatus('loading', 'Initializing...');

  sendWithPromise('initChat').then(function(info) {
    // Apply settings from backend
    if (info.max_history_messages) MAX_HISTORY_MESSAGES = info.max_history_messages;

    // Update hardware bar
    if (info.has_gpu) {
      document.getElementById('hwGpu').textContent = info.gpu_backend.toUpperCase();
    }
    document.getElementById('hwRam').textContent = info.total_ram_gb + 'GB RAM';
    document.getElementById('hwCores').textContent = info.cpu_cores + ' cores';

    // Check model status
    if (info.model_loaded) {
      setStatus('ready', 'Model Ready');
    } else if (info.is_first_run) {
      // Show first-run welcome overlay
      isFirstRun = true;
      setStatus('offline', 'Setup required');
      document.getElementById('welcomeOverlay').classList.add('open');
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
