// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_ui_js.h"
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
    const std::string html = std::string(R"HTML(<!DOCTYPE html>
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
.input-area button.mic{padding:10px 12px;border-radius:10px;border:1px solid #3a3a3a;background:transparent;color:#bbb;font-size:14px;cursor:pointer}
.input-area button.mic:hover{background:#2a2a2a;color:#fff}
.input-area button.mic.recording{background:#dc2626;color:#fff;border-color:#dc2626;animation:mic-pulse 1.2s ease-in-out infinite}
.input-area button.mic.transcribing{background:#3a3a3a;color:#9ec5ff;border-color:#3a86ff}
@keyframes mic-pulse{0%,100%{box-shadow:0 0 0 0 rgba(220,38,38,0.6)}50%{box-shadow:0 0 0 6px rgba(220,38,38,0.0)}}
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
.molt-action-confirm{margin:6px 0 6px 36px;padding:8px 10px;
  border-radius:8px;background:#1f1d2e;color:#e2dffb;
  border:1px solid #6366f1;font-size:11px;
  font-family:-apple-system,system-ui,sans-serif;
  display:flex;align-items:center;gap:10px;max-width:520px;}
.molt-action-confirm .ac-icon{font-size:16px;flex-shrink:0;}
.molt-action-confirm .ac-body{flex:1;min-width:0;}
.molt-action-confirm .ac-title{font-weight:600;font-size:10px;
  text-transform:uppercase;letter-spacing:0.5px;opacity:0.8;
  margin-bottom:2px;}
.molt-action-confirm .ac-label{overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap;font-family:ui-monospace,Menlo,monospace;}
.molt-action-confirm .ac-buttons{display:flex;gap:6px;flex-shrink:0;}
.molt-action-confirm button{padding:4px 10px;border-radius:5px;
  border:1px solid #444;background:transparent;color:#e2dffb;
  font-size:11px;cursor:pointer;font-family:inherit;}
.molt-action-confirm .ac-allow{background:#6366f1;border-color:#6366f1;
  color:#fff;}
.molt-action-confirm .ac-allow:hover{opacity:0.85;}
.molt-action-confirm .ac-deny:hover{background:#2a1010;border-color:#4a1a1a;
  color:#fca5a5;}
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
<div class="anon-banner" id="anonBanner" style="display:none">
  <span class="anon-icon">&#128274;</span>
  <span>Anonymous session — cookies and storage discarded on close.</span>
</div>
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
    var anon = document.getElementById('anonBanner');
    // Anonymous session banner — shown whenever the active tab lives
    // in an OTR profile. The native side sets is_anonymous_session
    // on every context push so this stays in sync with tab switches.
    if (anon) {
      anon.style.display = (ctx && ctx.is_anonymous_session) ? 'flex'
                                                              : 'none';
    }
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
    // Native side suffixes PDF titles with " (PDF)" so we can show a
    // distinct icon. Simple heuristic — also accept any .pdf URL.
    var isPdf = (/\(PDF\)$/.test(title)) ||
                /\.pdf(\?|#|$)/i.test(ctx.url);
    document.querySelector('#tabContext .tab-context-icon').innerHTML =
        isPdf ? '&#128196;' : '&#128279;';  // page glyph vs. chain link
    lbl.textContent = (isPdf ? 'Chatting with PDF: ' : 'Chatting about: ')
                       + title +
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
.anon-banner{display:flex;align-items:center;gap:8px;
  background:#3a2a5a;color:#dabfff;padding:6px 12px;
  font-size:11px;font-weight:500;border-bottom:1px solid #2a1a3a;}
.anon-icon{flex-shrink:0;}
.agent-inbox{background:rgba(58,134,255,0.08);border-bottom:1px solid #2a2a2a;
  padding:6px 12px;font-size:11px;display:flex;flex-direction:column;gap:4px;}
.agent-inbox-header{font-weight:600;color:#9ec5ff;opacity:0.85;
  font-size:10px;text-transform:uppercase;letter-spacing:0.5px;}
.agent-row{display:flex;align-items:center;gap:8px;
  padding:4px 6px;background:rgba(255,255,255,0.03);border-radius:4px;}
.agent-spinner{width:8px;height:8px;border-radius:50%;
  background:#3a86ff;animation:agent-pulse 1.5s ease-in-out infinite;
  flex-shrink:0;}
.agent-spinner.done-ok{background:#46d160;animation:none;}
.agent-spinner.done-err{background:#ff4d4d;animation:none;}
@keyframes agent-pulse{0%,100%{opacity:0.4;}50%{opacity:1;}}
.agent-name{font-weight:500;color:#e8e8e8;overflow:hidden;
  text-overflow:ellipsis;white-space:nowrap;flex:0 0 auto;max-width:40%;}
.agent-progress{color:#9ea0a4;flex:0 0 auto;font-variant-numeric:tabular-nums;}
.agent-note{color:#9ea0a4;overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap;flex:1 1 auto;font-style:italic;}
.profile-editor{display:flex;flex-direction:column;gap:6px;padding:8px;
  background:rgba(58,134,255,0.05);border-radius:6px;}
.profile-help{font-size:11px;color:#9ea0a4;margin-bottom:4px;line-height:1.5;}
.profile-help code{background:#1a1a2a;padding:1px 4px;border-radius:3px;}
.profile-row{display:flex;align-items:center;gap:8px;}
.profile-row label{flex:0 0 110px;font-size:11px;color:#a8a8b8;}
.profile-row input{flex:1;padding:4px 8px;font-size:12px;
  background:#0d0d14;color:#e8e8e8;border:1px solid #2a2a3a;border-radius:4px;}
.profile-row input:focus{outline:none;border-color:#3a86ff;}
.profile-actions{display:flex;gap:6px;margin-top:6px;}
.profile-actions button{padding:4px 12px;font-size:12px;border-radius:4px;
  background:#3a86ff;color:#fff;border:none;cursor:pointer;}
.profile-actions .profile-cancel{background:#2a2a3a;color:#a8a8b8;}
.history-summary{font-size:11px;color:#9ea0a4;margin-bottom:8px;
  padding-bottom:6px;border-bottom:1px solid #2a2a3a;}
.history-cluster{margin-bottom:6px;background:rgba(255,255,255,0.02);
  border-radius:6px;padding:6px 10px;}
.history-cluster summary{cursor:pointer;display:flex;justify-content:space-between;
  align-items:center;font-size:12px;list-style:none;}
.history-cluster summary::-webkit-details-marker{display:none;}
.hist-label{font-weight:600;color:#e8e8e8;}
.hist-count{font-size:10px;color:#9ea0a4;background:#1a1a2a;
  padding:2px 8px;border-radius:10px;}
.history-cluster ul{margin:6px 0 0 0;padding-left:0;list-style:none;}
.history-cluster li{padding:3px 0;font-size:11px;
  display:flex;justify-content:space-between;align-items:baseline;gap:8px;}
.history-cluster li a{color:#9ec5ff;text-decoration:none;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1;min-width:0;}
.history-cluster li a:hover{text-decoration:underline;}
.hist-meta{color:#6e7080;font-size:10px;flex-shrink:0;font-variant-numeric:tabular-nums;}
</style>
<div class="hw-bar" id="hwBar">
  <span id="hwGpu"></span>
  <span id="hwRam"></span>
  <span id="hwCores"></span>
</div>
<div class="context-bar" id="contextBar"></div>
<!-- Agent Inbox: live tray of currently-running background automations.
     Polled every 3s from JS; hidden when no runs are active. -->
<div class="agent-inbox" id="agentInbox" style="display:none;"></div>
<div class="messages" id="messages">
  <div class="message ai">
    <div class="sender">AI Assistant</div>
    <div class="text">Welcome! I'm your local AI assistant running entirely on this device.<br><br><b>AI:</b> <code>/pdf</code>, <code>/bookmark &lt;q&gt;</code>, <code>/ask-tabs &lt;q&gt;</code>, <code>/history</code>, <code>/cluster &lt;n&gt; &lt;q&gt;</code><br><b>Reader:</b> <code>/simplify</code>, <code>/eli5</code>, <code>/summarize</code>, <code>/tldr</code>, <code>/chapters</code> (YouTube)<br><b>Privacy:</b> <code>/trackers</code>, <code>/reputation</code>, <code>/hops</code>, <code>/tor status</code>, <code>/sandbox &lt;url&gt;</code>, <code>/js on|off</code><br><b>Actions:</b> <code>/triage list</code>, <code>/watch &lt;url&gt; &lt;selector&gt;</code>, <code>/receipt</code>, <code>/plan &lt;task&gt;</code>, <code>/fill</code>, <code>/fill ai</code>, <code>/paste</code>, <code>/click .button</code><br><b>Vault:</b> <code>/vault list</code>, <code>/vault fill</code>, <code>/vault generate</code><br><b>Translate:</b> <code>/translate [lang] &lt;text&gt;</code><br><b>Digest:</b> <code>/digest</code> (last 24h briefing)<br><br>Or just send a message.</div>
  </div>
</div>
<div class="actions" id="quickActions">
  <button onclick="quickAction('summarize')">Summarize</button>
  <button onclick="quickAction('extract')">Extract Data</button>
  <button onclick="quickAction('explain')">Explain</button>
  <button onclick="quickAction('translate')">Translate</button>
</div>
<div id="inputArea" class="input-area">
  <input type="text" id="chatInput" placeholder="Ask AI or describe a task to browse..." autofocus>
  <button class="mic" id="micBtn" onclick="toggleMic()" title="Hold or click to record (local Whisper)">🎙</button>
  <button class="cancel" id="cancelBtn" onclick="cancelGeneration()">Stop</button>
  <button class="send" id="sendBtn" onclick="sendMessage()">Send</button>
  <button class="send" id="agentBtn" title="Let AI autonomously browse and complete this task"
    style="background:linear-gradient(135deg,#4338ca,#7c3aed);margin-left:2px">Browse</button>
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
)HTML") + ::molt_ai::kMoltChatJS + R"HTML(
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
