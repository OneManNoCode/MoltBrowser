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
<div class="input-area">
  <input type="text" id="prompt" placeholder="Ask MoltBrowser AI..." autofocus>
  <button class="mic" id="micBtn" onclick="toggleMic()" title="Hold or click to record (local Whisper)">🎙</button>
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

// Action types that CAN modify state on the active tab. Each one
// requires a one-tap user confirmation before dispatch. Read-only
// or strictly local actions (scroll, wait, wait-for) auto-run.
//
// This is the defense against prompt injection in any third-party
// content the LLM consumes (PDF text, page captures, YouTube
// transcripts, bookmark titles, vault notes) emitting
// [[ACTION navigate:phishing.example]] / [[ACTION click:#delete]] /
// [[ACTION type:#pw|secret]] and having it run silently. Code-review
// finding HIGH #2 from 2026-05-20.
var DANGEROUS_ACTION_TYPES = {
  click: 1, type: 1, select: 1, hover: 1, 'right-click': 1,
  drag: 1, navigate: 1
};

function _renderActionLabel(action) {
  return action.type +
         (action.selector ? ' ' + action.selector : '') +
         ((action.value && action.type !== 'wait-for')
             ? ' = ' + action.value : '');
}

function _renderConfirmChip(action, onAllow, onDeny) {
  var msgs = document.getElementById('messages');
  if (!msgs) { onDeny(); return; }
  var d = document.createElement('div');
  d.className = 'molt-action-confirm';
  var label = _renderActionLabel(action);
  // Stylesheet for this chip is added inline so it can't be skinned
  // out by site CSS injection into our message area.
  d.innerHTML =
    '<span class="ac-icon">⚡</span>' +
    '<div class="ac-body">' +
      '<div class="ac-title">AI proposes an action</div>' +
      '<div class="ac-label">' + esc(label) + '</div>' +
    '</div>' +
    '<div class="ac-buttons">' +
      '<button class="ac-deny">Deny</button>' +
      '<button class="ac-allow">Allow</button>' +
    '</div>';
  msgs.appendChild(d);
  msgs.scrollTop = msgs.scrollHeight;
  // Single-use handlers; remove the chip on either path so the chat
  // stays clean.
  var settle = function(handler) {
    return function(){
      try { d.parentNode && d.parentNode.removeChild(d); } catch(e) {}
      handler();
    };
  };
  d.querySelector('.ac-allow').onclick = settle(onAllow);
  d.querySelector('.ac-deny').onclick = settle(onDeny);
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
    var label = _renderActionLabel(action);
    var dispatch = function() {
      sendWithPromise('runMoltAction', action).then(function(r) {
        appendActionResult(r && r.success, label,
                            r ? (r.message || r.error || '') : '');
      }).catch(function(err) {
        appendActionResult(false, label, String(err || 'failed'));
      }).then(loop, loop);
    };
    if (DANGEROUS_ACTION_TYPES[action.type]) {
      _renderConfirmChip(action, dispatch, function(){
        appendActionResult(false, label, 'denied by user');
        loop();
      });
    } else {
      dispatch();
    }
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
// --------------------------------------------------------------
// PDF chat: /pdf [url]
// With no arg, uses the active tab URL if it ends in .pdf. Otherwise
// fetches the supplied URL. The extracted text is stored as the next
// user turn's pre-pended context so any follow-up question grounds
// in the PDF.
// --------------------------------------------------------------
var pdfContext = null;  // {url, host, text} — primed by /pdf, consumed once
function tryDispatchPdfCommand(text) {
  var m = text.match(/^\s*\/pdf\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  var url = rest;
  if (!url) {
    // Use the active tab if it looks like a PDF.
    var tabUrl = (window.__moltLastTabContext &&
                  window.__moltLastTabContext.url) || '';
    if (/\.pdf(\?|#|$)/i.test(tabUrl)) url = tabUrl;
  }
  if (!url) {
    addErrorMessage('Usage: /pdf <url>  (or open a .pdf tab first)');
    return true;
  }
  if (!/^https?:\/\//i.test(url) && !/^file:\/\//i.test(url)) {
    url = 'https://' + url;
  }
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Fetching ' + url + '...');
  sendWithPromise('extractPdfText', url).then(function(r) {
    if (!r.success) {
      currentAiText = '✗ ' + (r.error || 'PDF extraction failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var host = '';
    try { host = new URL(r.url).host; } catch (e) {}
    pdfContext = {url: r.url, host: host, text: r.text};
    var preview = (r.text || '').slice(0, 280);
    currentAiText = '✓ Loaded ' + r.char_count + ' chars from ' +
                    (host || r.url) + '. Ask me anything about it.\n\n' +
                    '"' + preview + (r.char_count > 280 ? '…' : '') + '"';
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Smart bookmarks: /bookmark <query> | /bm <query>
// Keyword-ranks the user's bookmarks and shows top hits with one-click
// open links.
// --------------------------------------------------------------
function tryDispatchBookmarksCommand(text) {
  var m = text.match(/^\s*\/(bookmark|bm)\b\s*(.*)$/i);
  if (!m) return false;
  var query = (m[2] || '').trim();
  if (!query) {
    addErrorMessage('Usage: /bookmark <query>');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Searching bookmarks...');
  sendWithPromise('searchBookmarks', query, 10).then(function(r) {
    var hits = (r && r.hits) || [];
    if (!hits.length) {
      currentAiText = 'No bookmark matched "' + query + '".';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    function esc(s) { return (s + '').replace(/&/g,'&amp;')
                                     .replace(/</g,'&lt;')
                                     .replace(/>/g,'&gt;')
                                     .replace(/"/g,'&quot;'); }
    var lines = ['Top bookmark matches:'];
    hits.forEach(function(h, i) {
      lines.push((i + 1) + '. ' + esc(h.title) +
                 ' — ' + esc(h.host) +
                 ' (score ' + h.score + ')' +
                 (h.parent ? '  · ' + esc(h.parent) : '') +
                 '\n   ' + esc(h.url));
    });
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Cross-tab Q&A: /ask-tabs <question> | /ask <question>
// Pulls innerText from every tab in this window, chunks it, ranks
// chunks against the question, and asks the LLM with the top-K
// chunks as grounding context. The standard chunk-ranker is reused.
// --------------------------------------------------------------
function tryDispatchAskTabsCommand(text) {
  var m = text.match(/^\s*\/(ask-tabs|asktabs|ask)\b\s*(.*)$/i);
  if (!m) return false;
  var question = (m[2] || '').trim();
  if (!question) {
    addErrorMessage('Usage: /ask-tabs <question>');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Reading all open tabs...');
  sendWithPromise('extractAllTabsText', 4000).then(function(r) {
    var tabs = (r && r.tabs) || [];
    if (!tabs.length) {
      currentAiText = 'No tabs to read.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    // Build a combined text labeled per tab.
    var labeled = '';
    tabs.forEach(function(t) {
      if (!t.text) return;
      labeled += '\n\n[' + (t.title || t.url) + ']\n' + t.text;
    });
    // Chunk + rank with existing helpers.
    var chunks = chunkPageText(labeled, 600);
    var top = rankChunksByQuery(chunks, question, 6);
    var ctx = top.join('\n\n---\n\n');
    var prompt = 'Question: ' + question;
    var historyForPrompt = '';
    var pageCtx =
      'Cross-tab Q&A (' + tabs.length + ' tabs). Use only these snippets:\n' +
      ctx;
    currentAiText = '';
    // Restart the AI message bubble since we'll stream into it.
    startAiMessage();
    sendWithPromise('sendPrompt', prompt, historyForPrompt, pageCtx)
        .then(function(result) {
          finishAiMessage();
          setGenerating(false);
          if (result && !result.success && result.error) {
            addErrorMessage(result.error);
          }
        }).catch(function() {
          finishAiMessage();
          setGenerating(false);
        });
  });
  return true;
}

// --------------------------------------------------------------
// Sandbox tab: /sandbox <url>
// Opens the URL in an off-the-record window. Cookies and storage
// from that session vanish when the window closes.
// --------------------------------------------------------------
function tryDispatchSandboxCommand(text) {
  var m = text.match(/^\s*\/sandbox\b\s*(.*)$/i);
  if (!m) return false;
  var url = (m[1] || '').trim();
  if (!url) {
    var tabUrl = (window.__moltLastTabContext &&
                  window.__moltLastTabContext.url) || '';
    if (tabUrl) url = tabUrl;
  }
  if (!url) {
    addErrorMessage('Usage: /sandbox <url>');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  sendWithPromise('openSandboxTab', url).then(function(r) {
    currentAiText = r.success
        ? '✓ Opened ' + r.url + ' in a sandbox window. ' +
          'Cookies/storage will be discarded on close.'
        : '✗ ' + (r.error || 'sandbox open failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Privacy heatmap: /trackers
// Asks the active tab for a breakdown of third-party resources it
// loaded, grouped by ads / analytics / cdn / other. The JS probe is
// implemented in the C++ handler — we only render the result here.
// --------------------------------------------------------------
function tryDispatchTrackersCommand(text) {
  if (!/^\s*\/trackers\b/i.test(text)) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Scanning active tab for third-party resources...');
  sendWithPromise('getTrackerBreakdown').then(function(r) {
    if (!r || r.error) {
      currentAiText = '✗ ' + (r && r.error || 'tracker scan failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var c = r.counts || {};
    var total = r.total_third_party || 0;
    var lines = [
      'Privacy heatmap for ' + (r.origin || 'this tab') + ':',
      '  First-party resources: ' + (c.first_party || 0),
      '  Third-party total:     ' + total +
        '   (across ' + (r.unique_third_party_hosts || 0) +
        ' unique hosts)',
      '    · ads:       ' + (c.ads || 0),
      '    · analytics: ' + (c.analytics || 0),
      '    · cdn:       ' + (c.cdn || 0),
      '    · other:     ' + (c.other || 0)
    ];
    if (r.top_hosts && r.top_hosts.length) {
      lines.push('');
      lines.push('Top third-party hosts:');
      r.top_hosts.forEach(function(h){
        lines.push('  [' + h.category + '] ' + h.host + ' × ' + h.count);
      });
    }
    if (total === 0) {
      lines.push('');
      lines.push('Nothing third-party loaded — clean page.');
    }
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Domain reputation: /reputation [host] | /rep [host]
// Uses Personal Vector Memory data to tell the user how many times
// they've visited this host, how much they've read, when first and
// last. Plus a tiny conservative risk-flag heuristic for sketchy
// TLDs / raw-IP hosts.
// --------------------------------------------------------------
function tryDispatchReputationCommand(text) {
  var m = text.match(/^\s*\/(reputation|rep)\b\s*(.*)$/i);
  if (!m) return false;
  var hostArg = (m[2] || '').trim();
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Looking up domain reputation...');
  sendWithPromise('getDomainReputation', hostArg).then(function(r) {
    if (!r || r.error) {
      currentAiText = '✗ ' + (r && r.error || 'reputation lookup failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    function fmt(unix) {
      if (!unix) return '—';
      var d = new Date(unix * 1000);
      return d.toLocaleString();
    }
    var lines = ['Domain reputation for ' + (r.host || 'unknown') + ':'];
    if (r.first_visit_on_device) {
      lines.push('  • First visit on this device.');
    } else {
      lines.push('  • Visited ' + r.visit_count + ' time' +
                 (r.visit_count === 1 ? '' : 's') + ' before.');
      lines.push('  • First seen: ' + fmt(r.first_visit_unix));
      lines.push('  • Last seen:  ' + fmt(r.last_visit_unix));
      lines.push('  • Total content read: ' +
                 (r.total_words_read || 0).toLocaleString() + ' words');
    }
    if (r.risky_tld) {
      lines.push('  ⚠ TLD often associated with low-cost / abuse-heavy ' +
                 'registrations — be careful with credentials.');
    }
    if (r.is_ip_host) {
      lines.push('  ⚠ Raw IP address as host. Legitimate sites almost ' +
                 'always have a domain name.');
    }
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Connection path: /hops [url]
// v0: shows the URL structure, OTR session status, and a 3-hop
// you → ISP → destination diagram. Honest about not yet routing
// through Tor — that's the next batch.
// --------------------------------------------------------------
function tryDispatchHopsCommand(text) {
  var m = text.match(/^\s*\/hops\b\s*(.*)$/i);
  if (!m) return false;
  var url = (m[1] || '').trim();
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Tracing connection path...');
  // We fan out two queries in parallel: the local connection-path
  // template and the live Tor circuit data. If Tor is up, we fold its
  // relays into the hop diagram. This is the same response shape
  // Phase B.2 will hit when routing through Tor — the visualizer
  // doesn't need to change when routing arrives.
  Promise.all([
    sendWithPromise('getConnectionPath', url),
    sendWithPromise('getTorCircuits').catch(function(){ return null; })
  ]).then(function(arr) {
    var r = arr[0];
    var torR = arr[1];
    if (!r || r.error) {
      currentAiText = '✗ ' + (r && r.error || 'connection path failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var lines = ['Connection path to ' + r.host + ':'];
    lines.push('');
    var hops = r.hops || [];
    // Pick a built circuit's relays to splice in between "Your ISP"
    // and the destination. The visualizer still labels these as
    // "would-route-through" until Phase B.2 actually pipes traffic.
    var torHops = null;
    if (torR && torR.circuits && torR.circuits.length) {
      for (var i = 0; i < torR.circuits.length; i++) {
        var c = torR.circuits[i];
        if (c.state === 'BUILT' && c.hops && c.hops.length >= 2) {
          torHops = c.hops;
          break;
        }
      }
    }
    if (torHops) {
      // Replace the middle "Your ISP" hop with the Tor relay chain.
      var newHops = [hops[0]];
      torHops.forEach(function(h, idx) {
        var role = idx === 0 ? 'guard'
                  : (idx === torHops.length - 1 ? 'exit' : 'middle');
        var name = h.nickname || h.fingerprint.slice(0, 8) + '…';
        var flag = h.country ? ccToFlag(h.country) + ' ' : '';
        var country = h.country ? ' (' + h.country + ')' : '';
        newHops.push({
          label: flag + 'Tor ' + role + ': ' + name + country,
          detail: (h.ip ? h.ip + '  ·  ' : '') + h.fingerprint
        });
      });
      newHops.push(hops[hops.length - 1]);
      hops = newHops;
    }
    for (var i2 = 0; i2 < hops.length; i2++) {
      var h2 = hops[i2];
      var marker = (i2 === 0) ? '●'
                  : (i2 === hops.length - 1 ? '○' : '─');
      lines.push('  ' + marker + ' ' + h2.label +
                 (h2.detail ? '  — ' + h2.detail : ''));
      if (i2 < hops.length - 1) lines.push('  │');
    }
    lines.push('');
    lines.push('Scheme: ' + r.scheme +
               (r.is_https ? '  (TLS-encrypted)' : '  (PLAINTEXT)'));
    if (r.is_anonymous_session) {
      lines.push('Session: ✓ Anonymous (sandbox tab).');
    }
    if (torHops) {
      lines.push('Tor:     ✓ live circuit shown above (relays from your ' +
                 'local Tor instance). Routing your tabs through them ' +
                 'lands in Phase B.2.');
    } else if (r.notes) {
      lines.push('');
      lines.push('Note: ' + r.notes);
    }
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Tor (Phase B.1): /tor [status|circuit|circuits|help]
// Talks to a locally-running Tor over its control port (127.0.0.1:9051).
// Honest about scope: this batch ships the visualizer + the protocol
// integration. Routing through Tor lands in Phase B.2 — until then,
// /tor status shows whether local Tor is running and /tor circuit
// shows the live circuit list (guard / middle / exit hops with their
// nicknames).
// --------------------------------------------------------------
function renderTorCircuits(circuits) {
  if (!circuits || !circuits.length) {
    return 'No circuits yet — Tor is still bootstrapping. Try again in a few seconds.';
  }
  var lines = ['Live Tor circuits (' + circuits.length + '):'];
  circuits.forEach(function(c) {
    var hopStr = '';
    if (c.hops && c.hops.length) {
      hopStr = c.hops.map(function(h){
        var name = h.nickname ? h.nickname
                              : h.fingerprint.slice(0, 8) + '…';
        var flag = h.country ? ccToFlag(h.country) + ' ' : '';
        return flag + name +
               (h.country ? ' (' + h.country + ')' : '');
      }).join('  →  ');
    } else {
      hopStr = '(no path yet)';
    }
    lines.push('  [' + c.id + ' ' + c.state +
               (c.purpose ? ' · ' + c.purpose : '') + ']  ' + hopStr);
  });
  return lines.join('\n');
}

// ISO 3166-1 alpha-2 → emoji flag. Tor's GETINFO ip-to-country
// returns two-letter codes; we render each by mapping each letter to
// the corresponding Regional Indicator Symbol (U+1F1E6..U+1F1FF).
function ccToFlag(cc) {
  if (!cc || cc.length !== 2) return '';
  var A = 0x1F1E6;
  var hi = cc.toUpperCase().charCodeAt(0) - 65 + A;
  var lo = cc.toUpperCase().charCodeAt(1) - 65 + A;
  if (hi < A || lo < A) return '';
  return String.fromCodePoint(hi) + String.fromCodePoint(lo);
}

function tryDispatchTorCommand(text) {
  var m = text.match(/^\s*\/tor\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  var parts = rest.split(/\s+/);
  var sub = (parts[0] || 'status').toLowerCase();
  addUserMessage(text);
  startAiMessage();
  if (sub === 'help') {
    currentAiText =
      'Tor commands:\n' +
      '  /tor status            — is Tor available? running? bundled or system?\n' +
      '  /tor circuit           — live circuit hops with country flags\n' +
      '  /tor launch            — start the bundled tor (no install needed)\n' +
      '  /tor open <url>        — open URL in OTR session routed through Tor\n' +
      '  /tor close             — stop the managed tor child process\n' +
      '\n' +
      'MoltBrowser ships with tor bundled inside the app. Run\n' +
      '/tor launch to start it, /tor open <url> to use it.\n' +
      'No external installation required.';
    finishAiMessage();
    setGenerating(false);
    return true;
  }
  if (sub === 'launch') {
    appendToAiMessage('Looking for tor binary and launching... ' +
                       '(bootstrap may take 15-30s)');
    sendWithPromise('launchTor').then(function(r) {
      if (r.success) {
        currentAiText = '✓ Tor launched.\n' +
                        '  Binary: ' + (r.binary_path || '?') + '\n' +
                        '  PID:    ' + (r.pid || '?') + '\n\n' +
                        'Use /tor open <url> to route a tab through it.';
      } else {
        currentAiText = '✗ ' + (r.error || 'launch failed');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }
  if (sub === 'close' || sub === 'stop' || sub === 'kill') {
    sendWithPromise('stopTor').then(function() {
      currentAiText = '✓ Sent SIGTERM to managed tor child. ' +
                      'External tor (if any) is unaffected.';
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }
  if (sub === 'open') {
    var url = parts.slice(1).join(' ').trim();
    if (!url) {
      var tabUrl = (window.__moltLastTabContext &&
                    window.__moltLastTabContext.url) || '';
      if (tabUrl) url = tabUrl;
    }
    if (!url) {
      currentAiText = 'Usage: /tor open <url>';
      finishAiMessage();
      setGenerating(false);
      return true;
    }
    appendToAiMessage('Configuring SOCKS5 proxy on OTR profile and ' +
                       'opening ' + url + '...');
    sendWithPromise('openTorTab', url).then(function(r) {
      if (r.success) {
        currentAiText = '✓ Opened ' + r.url + ' in an OTR window ' +
                        'routed through ' + r.proxy + '.\n\n' +
                        'All tabs in that OTR window now use Tor. ' +
                        'Close the window to revert.';
      } else {
        currentAiText = '✗ ' + (r.error || 'open-tor-tab failed');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }
  if (sub === 'circuit' || sub === 'circuits') {
    appendToAiMessage('Reading circuit list from Tor control port...');
    sendWithPromise('getTorCircuits').then(function(r) {
      currentAiText = renderTorCircuits(r && r.circuits);
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }
  // status (default)
  appendToAiMessage('Probing local Tor on 127.0.0.1:9051...');
  sendWithPromise('getTorStatus').then(function(r) {
    if (!r.running) {
      var src = r.binary_source || 'none';
      var srcLine =
        src === 'bundled' ? '  Binary: ✓ bundled (' + r.binary_path + ')'
        : src === 'system' ? '  Binary: ⚠ system tor (' + r.binary_path + ')'
        : '  Binary: ✗ none found';
      currentAiText = '✗ Tor is not running yet.\n\n' +
                      srcLine + '\n\n' +
                      (r.install_hint || '');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var src = r.binary_source || 'unknown';
    var srcLabel =
      src === 'bundled' ? '✓ bundled (no install required)'
      : src === 'system' ? '⚠ system tor'
      : 'unknown';
    var lines = [
      '✓ Tor is running.',
      '  Source:       ' + srcLabel,
      '  Binary:       ' + (r.binary_path || '?'),
      '  Version:      ' + (r.version || 'unknown'),
      '  Control port: ' + r.control_port,
      '  SOCKS port:   ' + r.socks_port,
    ];
    // Pull circuits for a one-shot snapshot under the status.
    sendWithPromise('getTorCircuits').then(function(c) {
      lines.push('');
      lines.push(renderTorCircuits(c && c.circuits));
      lines.push('');
      lines.push('Routing your tabs through these circuits lands in ' +
                 'Phase B.2. Today: visualization layer.');
      currentAiText = lines.join('\n');
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Selective JS toggle: /js on | /js off [host]
// HostContentSettingsMap toggle. Reloads active tab so the change
// takes effect right now.
// --------------------------------------------------------------
function tryDispatchJsCommand(text) {
  var m = text.match(/^\s*\/js\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  var parts = rest.split(/\s+/);
  var enabled;
  if (parts[0] === 'on' || parts[0] === 'enable') enabled = true;
  else if (parts[0] === 'off' || parts[0] === 'disable') enabled = false;
  else {
    addErrorMessage('Usage: /js on | /js off  [host]');
    return true;
  }
  var host = parts[1] || '';
  if (!host) {
    var tabUrl = (window.__moltLastTabContext &&
                  window.__moltLastTabContext.url) || '';
    try { host = new URL(tabUrl).host; } catch (e) {}
  }
  if (!host) {
    addErrorMessage('No host. Try /js ' + parts[0] + ' example.com');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  sendWithPromise('setJsForDomain', host, enabled).then(function(r) {
    currentAiText = r.success
        ? '✓ JavaScript ' + (enabled ? 'enabled' : 'disabled') +
          ' for ' + host + '. Active tab reloaded.'
        : '✗ ' + (r.error || 'JS toggle failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Tab Triage: /triage [close-inactive | close-domain <host> | list]
// /triage list  -> show all tabs in this window with a snippet.
// /triage close-inactive  -> close every non-active tab.
// /triage close-domain x.com  -> close every tab whose host == x.com.
// /triage bookmark-inactive -> bookmark every non-active tab to "Other".
// --------------------------------------------------------------
function tryDispatchTriageCommand(text) {
  var m = text.match(/^\s*\/triage\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Scanning open tabs...');
  sendWithPromise('listTabsInWindow').then(function(r) {
    var tabs = (r && r.tabs) || [];
    if (!rest || rest === 'list') {
      var lines = ['Open tabs in this window (' + tabs.length + '):'];
      for (var i = 0; i < tabs.length; i++) {
        var t = tabs[i];
        var prefix = (t.active ? '\u25CF ' : '  ') +
                     (t.pinned ? '\u{1F4CC} ' : '');
        lines.push(prefix + '[' + t.index + '] ' + (t.snippet || t.url));
      }
      currentAiText = lines.join('\n');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    // Pick indices to act on.
    var picked = [];
    var action = 'close';
    var parts = rest.split(/\s+/);
    var verb = parts[0];
    if (verb === 'close-inactive') {
      action = 'close';
      tabs.forEach(function(t){ if (!t.active && !t.pinned) picked.push(t.index); });
    } else if (verb === 'bookmark-inactive') {
      action = 'bookmark';
      tabs.forEach(function(t){ if (!t.active) picked.push(t.index); });
    } else if (verb === 'pin-active') {
      action = 'pin';
      tabs.forEach(function(t){ if (t.active) picked.push(t.index); });
    } else if (verb === 'close-domain' && parts[1]) {
      action = 'close';
      var host = parts[1].toLowerCase();
      tabs.forEach(function(t){
        try {
          var u = new URL(t.url);
          if (u.host.toLowerCase().indexOf(host) !== -1) picked.push(t.index);
        } catch (e) {}
      });
    } else {
      currentAiText = 'Usage:\n' +
                      '  /triage list\n' +
                      '  /triage close-inactive\n' +
                      '  /triage close-domain <host>\n' +
                      '  /triage bookmark-inactive\n' +
                      '  /triage pin-active';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    if (!picked.length) {
      currentAiText = 'No tabs matched.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    sendWithPromise('triageActOnTabs',
                    {action: action, indices: picked}).then(function(rr) {
      currentAiText = rr.success
          ? '\u2713 ' + action + 'd ' + rr.affected_count + ' tab' +
            (rr.affected_count === 1 ? '' : 's')
          : '\u2717 ' + (rr.error || 'triage failed');
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Page Watcher: /watch <url> <selector> [interval-seconds] [name...]
// Creates a scheduled INTERVAL script that opens the URL, extracts the
// selector text, and fires an OS notification with the current value.
// --------------------------------------------------------------
function tryDispatchWatchCommand(text) {
  var m = text.match(/^\s*\/watch\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  if (!rest) {
    addErrorMessage('Usage: /watch <url> <css-selector> ' +
                    '[interval-seconds] [name]');
    return true;
  }
  // First token = url, second = selector, third (optional) = seconds,
  // remainder = display name.
  var parts = rest.split(/\s+/);
  if (parts.length < 2) {
    addErrorMessage('Usage: /watch <url> <css-selector> ' +
                    '[interval-seconds] [name]');
    return true;
  }
  var url = parts[0];
  if (!/^https?:\/\//i.test(url)) url = 'https://' + url;
  var sel = parts[1];
  var seconds = 900;
  var nameStart = 2;
  if (parts.length >= 3 && /^\d+$/.test(parts[2])) {
    seconds = parseInt(parts[2], 10);
    nameStart = 3;
  }
  var name = parts.slice(nameStart).join(' ') || '';
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Creating watcher...');
  sendWithPromise('createWatcher',
                  {url: url, selector: sel,
                   interval_seconds: seconds,
                   name: name}).then(function(r) {
    if (r.success) {
      currentAiText = '\u2713 Watching ' + url + '\n' +
                      'Selector: ' + sel + '\n' +
                      'Every ' + r.interval_seconds + 's\n' +
                      'Script id: ' + r.script_id;
    } else {
      currentAiText = '\u2717 ' + (r.error || 'watcher creation failed');
    }
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Form Filler: /fill  (autofill the active tab from the saved profile)
//              /profile  (open the encrypted profile editor)
// --------------------------------------------------------------
function tryDispatchFillCommand(text) {
  if (/^\s*\/profile\b/i.test(text)) {
    addUserMessage(text);
    openProfileEditor();
    setGenerating(false);
    return true;
  }
  if (!/^\s*\/fill\b/i.test(text)) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Filling form...');
  sendWithPromise('runFormFill').then(function(r) {
    if (r.success) {
      currentAiText = '\u2713 Filled ' + r.filled + ' field' +
                      (r.filled === 1 ? '' : 's') +
                      ' (of ' + r.total + ' on this page)';
    } else {
      currentAiText = '\u2717 ' + (r.error || 'fill failed') +
                      '\n\nType /profile to set up your saved profile.';
    }
    finishAiMessage();
    setGenerating(false);
  }).catch(function(e) {
    currentAiText = '\u2717 ' + (e || 'fill failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// Render an inline profile editor in the messages area. Loads the
// encrypted profile via getMoltProfile and saves it back via
// saveMoltProfile. Keeps the WebUI surface area small — no separate
// settings page, no extension UI.
function openProfileEditor() {
  startAiMessage();
  appendToAiMessage('Loading your saved profile...');
  sendWithPromise('getMoltProfile').then(function(r) {
    var p = (r && r.profile) || {};
    var fields = [
      ['full_name',    'Full name'],
      ['first_name',   'First name'],
      ['last_name',    'Last name'],
      ['email',        'Email'],
      ['phone',        'Phone'],
      ['address_line1','Address line 1'],
      ['address_line2','Address line 2'],
      ['city',         'City'],
      ['state',        'State / Province'],
      ['zip',          'Postal code'],
      ['country',      'Country'],
      ['company',      'Company'],
      ['job_title',    'Job title'],
      ['website',      'Website']
    ];
    function esc(s){return (s+'').replace(/&/g,'&amp;').replace(/</g,'&lt;')
                                  .replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
    var rows = fields.map(function(f){
      return '<div class="profile-row">' +
             '<label>' + f[1] + '</label>' +
             '<input data-key="' + f[0] + '" type="text" value="' +
                esc(p[f[0]] || '') + '">' +
             '</div>';
    }).join('');
    currentAiText = '__PROFILE_EDITOR__';
    finishAiMessage();
    // Replace the last AI message with the editor markup.
    var msgs = document.querySelectorAll('#messages .message.ai');
    var last = msgs[msgs.length - 1];
    if (!last) return;
    last.querySelector('.text').innerHTML =
        '<div class="profile-editor">' +
        '<div class="profile-help">Encrypted at rest via your OS keychain. ' +
        'Used by <code>/fill</code> to autofill forms on http(s) pages.</div>' +
        rows +
        '<div class="profile-actions">' +
        '<button class="profile-save">Save profile</button>' +
        '<button class="profile-cancel">Cancel</button>' +
        '</div></div>';
    last.querySelector('.profile-save').addEventListener('click', function(){
      var dict = {};
      last.querySelectorAll('input[data-key]').forEach(function(el){
        var v = el.value.trim();
        if (v) dict[el.getAttribute('data-key')] = v;
      });
      sendWithPromise('saveMoltProfile', dict).then(function(rr){
        var text = rr.success
            ? '\u2713 Profile saved ('
              + Object.keys(dict).length + ' field' +
              (Object.keys(dict).length === 1 ? '' : 's') + ')'
            : '\u2717 ' + (rr.error || 'save failed');
        last.querySelector('.text').textContent = text;
      });
    });
    last.querySelector('.profile-cancel').addEventListener('click', function(){
      last.querySelector('.text').textContent = 'Profile editor closed.';
    });
  });
}

// --------------------------------------------------------------
// AI-grouped history: /history [limit]
// Loads up to N (default 200, max 2000) recent documents from
// Personal Vector Memory and groups them by simple keyword overlap
// of their titles. Each cluster renders as a collapsible card
// with title-derived topic + count + list of pages.
//
// We do the clustering here in JS so the user can re-cluster on
// filter (by host, by date range) without an IPC round-trip.
// --------------------------------------------------------------
// --------------------------------------------------------------
// Reader 2.0: /simplify, /eli5, /summarize, /tldr
// Take the active page's captured innerText, wrap it in a per-mode
// framing prompt, and stream the LLM's rewrite into chat. All four
// modes share the same plumbing — only the prompt template differs.
// --------------------------------------------------------------
function tryDispatchReaderCommand(text) {
  var m = text.match(/^\s*\/(simplify|eli5|summarize|tldr)\b\s*(.*)$/i);
  if (!m) return false;
  var mode = m[1].toLowerCase();
  var extra = (m[2] || '').trim();
  var sp = window.__moltCurrentTabContext;
  if (!sp || !sp.text || !sp.text.length) {
    addErrorMessage('Open an http(s) page first — Reader needs page content.');
    return true;
  }
  // Cap input so we don't spend half the context window on boilerplate.
  // 12k chars ≈ ~3k tokens, leaves room for the generation budget on
  // a 4k-context bundled model.
  var pageText = sp.text.length > 12000 ? sp.text.slice(0, 12000) : sp.text;
  var framings = {
    simplify:
      'Rewrite the following web page in plain, accessible language ' +
      'without losing accuracy. Use short sentences. Keep numbers, ' +
      'names, and dates exact.' +
      (extra ? ' Focus especially on: ' + extra + '.' : ''),
    eli5:
      'Explain this web page like the reader is five years old. Use ' +
      'playful, concrete analogies. Avoid jargon entirely. Keep it ' +
      'short — under 200 words.' +
      (extra ? ' The reader is most curious about: ' + extra + '.' : ''),
    summarize:
      'Summarize this web page in 3-5 tight bullet points. Each bullet ' +
      'is one sentence and captures one fact, claim, or conclusion.' +
      (extra ? ' Bias the summary toward: ' + extra + '.' : ''),
    tldr:
      'Give a single-sentence TL;DR of this web page that a busy ' +
      'reader could absorb in two seconds.'
  };
  var prompt = framings[mode] +
               '\n\nPage title: ' + (sp.title || '(untitled)') +
               '\n\nPage content:\n' + pageText;
  addUserMessage(text);
  startAiMessage();
  sendWithPromise('sendPrompt', prompt, /*history=*/'', /*ctx=*/'').then(
      function(result) {
        finishAiMessage();
        setGenerating(false);
        if (result && !result.success && result.error) {
          addErrorMessage(result.error);
        }
      }).catch(function() {
        finishAiMessage();
        setGenerating(false);
      });
  return true;
}

// --------------------------------------------------------------
// Receipt extractor: /receipt
// LLM extracts a strict JSON {merchant,date,total,currency,items} from
// the active page (which should be a checkout confirmation or invoice
// view). We parse it client-side and append to a local CSV ledger
// via the new appendReceipt IPC. The ledger lives at
// ~/.moltbrowser/ledger.csv — plaintext so you can pull it into a
// spreadsheet without any tooling.
// --------------------------------------------------------------
function tryDispatchReceiptCommand(text) {
  if (!/^\s*\/receipt\b/i.test(text)) return false;
  var sp = window.__moltCurrentTabContext;
  if (!sp || !sp.text || !sp.text.length) {
    addErrorMessage('Open a receipt / invoice / order-confirmation page first.');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  var schema =
    '{"merchant":"<string>","date":"YYYY-MM-DD",' +
    '"total":<number>,"currency":"<ISO 4217 code, default USD>",' +
    '"items":[{"name":"<string>","qty":<number>,"price":<number>}]}';
  var prompt =
    'You are a receipt extractor. Read the page below and respond with ' +
    'ONE JSON object matching this schema exactly:\n' + schema + '\n\n' +
    'Rules:\n' +
    '- If the page is not a receipt or invoice, respond with: ' +
    '{"error":"not a receipt"}\n' +
    '- Date must be ISO YYYY-MM-DD. If only the month and year are ' +
    'visible, use the first day.\n' +
    '- total is the grand total INCLUDING tax and shipping.\n' +
    '- If you can\'t find an item-level breakdown, return items: [].\n' +
    '- Respond with JSON only, no prose, no markdown fences.\n\n' +
    'Page title: ' + (sp.title || '(untitled)') +
    '\nPage URL: ' + (sp.url || '') +
    '\n\nPage content:\n' +
    (sp.text.length > 10000 ? sp.text.slice(0, 10000) : sp.text);
  sendWithPromise('sendPrompt', prompt, '', '').then(function() {
    // Grab whatever the LLM streamed into the current AI message bubble.
    // currentAiText holds the accumulated text.
    var jsonStr = currentAiText;
    // Best-effort JSON extraction: take the first balanced { ... } run.
    var first = jsonStr.indexOf('{');
    var last = jsonStr.lastIndexOf('}');
    if (first < 0 || last <= first) {
      currentAiText += '\n\n✗ Could not find JSON in the LLM response.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var slice = jsonStr.substring(first, last + 1);
    var data;
    try { data = JSON.parse(slice); }
    catch (e) {
      currentAiText += '\n\n✗ Invalid JSON: ' + e;
      finishAiMessage();
      setGenerating(false);
      return;
    }
    if (data.error) {
      currentAiText = '✗ ' + data.error;
      finishAiMessage();
      setGenerating(false);
      return;
    }
    sendWithPromise('appendReceipt', data, sp.url || '').then(function(r) {
      if (r.success) {
        currentAiText = '✓ Saved receipt: ' +
            (data.merchant || '?') + ' ' +
            (data.date || '') + ' ' +
            (data.currency || 'USD') + ' ' +
            (typeof data.total === 'number' ? data.total.toFixed(2)
                                            : data.total) + '\n' +
            'Ledger: ' + r.path;
      } else {
        currentAiText = '✗ ' + (r.error || 'save failed');
      }
      finishAiMessage();
      setGenerating(false);
    });
  }).catch(function() {
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// YouTube chapterizer: /chapters
// Injects a probe into the active tab that pulls the visible
// transcript (YouTube renders one in ytd-transcript-segment-renderer
// when the user has opened the transcript panel). We then ask the
// LLM to group those timestamped lines into chapters.
//
// If the transcript panel isn't open, we ask the user to open it
// rather than try to click "..." → "Show transcript" via brittle
// DOM scraping (that path changes every couple months).
// --------------------------------------------------------------
function tryDispatchChaptersCommand(text) {
  if (!/^\s*\/chapters\b/i.test(text)) return false;
  var sp = window.__moltCurrentTabContext;
  if (!sp || !sp.url || sp.url.indexOf('youtube.com') < 0) {
    addErrorMessage('Open a YouTube video first.');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Reading transcript from the YouTube tab...');
  // We don't ship a generic JS-eval IPC (deliberately — it would be
  // a perpetual RCE-into-active-tab landmine the moment a future
  // commit accidentally wires the script through unescaped). Instead
  // we rely on the page-context capture the side panel does on every
  // tab switch: when the transcript panel is open YouTube renders
  // timestamped lines into the DOM that show up in sp.text. We then
  // filter for "MM:SS" prefixes. Code-review LOW #13 (removed dead
  // `eval_for_chapters` branch that referenced a never-implemented
  // action type).
  (Promise.resolve(null))
      .then(function(r) {
        var transcriptText = '';
        if (!transcriptText) {
          // Backup heuristic: look in page-context capture for "::" or
          // numeric timestamp prefixes which YouTube's transcript uses.
          if (sp.text) {
            var lines = sp.text.split('\n').filter(function(L){
              return /^\s*\d{1,2}:\d{2}/.test(L);
            });
            if (lines.length >= 5) transcriptText = lines.join('\n');
          }
        }
        if (!transcriptText) {
          currentAiText =
            '✗ No transcript found. Click the "..." menu under the ' +
            'video, choose "Show transcript", then re-run /chapters.';
          finishAiMessage();
          setGenerating(false);
          return;
        }
        currentAiText = '';
        var prompt =
          'Below are timestamped transcript lines from a YouTube video. ' +
          'Group them into 5-10 chapters. For each chapter, output one ' +
          'line of the form:\n' +
          '  MM:SS  Chapter title\n' +
          'Use the earliest timestamp in that chapter\'s range. Keep ' +
          'titles short (under 8 words). Output chapter lines only, ' +
          'one per line, no preamble.\n\n' +
          'Transcript:\n' + transcriptText.slice(0, 12000);
        sendWithPromise('sendPrompt', prompt, '', '').then(function() {
          finishAiMessage();
          setGenerating(false);
        });
      });
  return true;
}

// --------------------------------------------------------------
// Cluster Q&A: /cluster <n> <question>
// Chat against just the docs in cluster N from the most recent
// /history run. The docs (URL + title) are pre-pended to the prompt
// as grounding so the answer stays inside that topic.
// --------------------------------------------------------------
function tryDispatchClusterCommand(text) {
  var m = text.match(/^\s*\/cluster\b\s*(\d*)\s*(.*)$/i);
  if (!m) return false;
  var idx = parseInt(m[1] || '0', 10);
  var question = (m[2] || '').trim();
  var cls = window.__moltLastHistoryClusters;
  if (!cls || !cls.length) {
    addErrorMessage('Run /history first to build the cluster index.');
    return true;
  }
  if (isNaN(idx) || idx < 1 || idx > cls.length) {
    addErrorMessage('Usage: /cluster <n> <question>  (n between 1 and ' +
                    cls.length + ')');
    return true;
  }
  var cluster = cls[idx - 1];
  if (!question) {
    // No question — just dump the cluster's pages for the user.
    addUserMessage(text);
    startAiMessage();
    var lines = ['Cluster ' + idx + ' — ' + (cluster.label || '(untitled)') +
                 '  (' + cluster.docs.length + ' pages):'];
    cluster.docs.slice(0, 30).forEach(function(d){
      lines.push('  • ' + (d.title || d.host || d.url));
    });
    if (cluster.docs.length > 30) {
      lines.push('  … (' + (cluster.docs.length - 30) + ' more)');
    }
    lines.push('');
    lines.push('To chat against this cluster: /cluster ' + idx +
               ' <your question>');
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  // Build a small grounding block from the cluster's docs. We only
  // include url + title (no snippets) — the LLM uses titles as a
  // pointer to its own memory of having seen the page, and a real
  // answer would need a memory-service query against just these doc
  // ids (not yet exposed via IPC — that's a B.2 follow-up for /cluster).
  var ground = cluster.docs.slice(0, 50).map(function(d){
    return '- ' + (d.title || d.host || d.url) + '  ' + d.url;
  }).join('\n');
  var prompt =
    'Below is a list of web pages the user has visited that belong to ' +
    'one topic cluster (' + (cluster.label || 'untitled') + '). ' +
    'Answer the user\'s question using only what you can confidently ' +
    'infer from these titles. If you can\'t answer with confidence, ' +
    'say so and recommend which pages to revisit.\n\n' +
    'Pages:\n' + ground + '\n\nQuestion: ' + question;
  sendWithPromise('sendPrompt', prompt, '', '').then(function() {
    finishAiMessage();
    setGenerating(false);
  }).catch(function() {
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Plan-a-task: /plan <task description>
// Asks the LLM to emit a multi-step plan in our action-token format,
// then parses it into a Script and saves it via the new savePlanScript
// IPC. The script is non-trusted by default — the user has to open
// it in the manager UI and click Run.
// --------------------------------------------------------------
function tryDispatchPlanCommand(text) {
  var m = text.match(/^\s*\/plan\b\s+(.+)$/i);
  if (!m) return false;
  var task = m[1].trim();
  addUserMessage(text);
  startAiMessage();
  var prompt =
    'You are a browser automation planner. Decompose the following ' +
    'task into a sequence of concrete browser steps using ONLY these ' +
    'action verbs:\n' +
    '  navigate <url>\n' +
    '  click <css-selector>\n' +
    '  type <css-selector> <text>\n' +
    '  wait-for <css-selector>\n' +
    '  scroll <pixels>\n' +
    '  extract <css-selector> as <var-name>\n' +
    '  notify <body>\n\n' +
    'Output one step per line, prefixed with "STEP: ". Then output ' +
    'a line "NAME: <short script name>" at the end. No prose, no ' +
    'markdown, no explanation.\n\n' +
    'Task: ' + task;
  sendWithPromise('sendPrompt', prompt, '', '').then(function() {
    // Parse currentAiText for STEP: and NAME: lines.
    var lines = currentAiText.split('\n');
    var steps = [];
    var name = task.slice(0, 60);
    lines.forEach(function(L){
      var sm = L.match(/^\s*STEP:\s*(.+)$/i);
      if (sm) steps.push(sm[1].trim());
      var nm = L.match(/^\s*NAME:\s*(.+)$/i);
      if (nm) name = nm[1].trim();
    });
    if (!steps.length) {
      currentAiText += '\n\n✗ Could not parse any STEP: lines from the LLM ' +
                       'plan. (Smaller models sometimes ignore the format; ' +
                       'try a more specific task.)';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    sendWithPromise('savePlanScript', {name: name, steps: steps, task: task})
        .then(function(r) {
          if (r.success) {
            currentAiText += '\n\n✓ Saved plan as script "' +
                             r.script_id + '"  (' + steps.length +
                             ' steps). Open the Automations manager ' +
                             'to review + run.';
          } else {
            currentAiText += '\n\n✗ ' + (r.error || 'save failed');
          }
          finishAiMessage();
          setGenerating(false);
        });
  }).catch(function() {
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Smart paste: /paste [extra]
// Reads the clipboard via navigator.clipboard.readText() (works in
// privileged WebUI context with no prompt), then routes through
// the existing Form Filler v2 path: probe active page → LLM maps
// pasted blob to fields → apply.
//
// Optional [extra] text is appended to the LLM prompt as a hint:
//   /paste prefer phone over fax
//
// Privacy note: the pasted content goes to the LOCAL LLM only, never
// over the network. The user pasted it; it doesn't leave the device.
// --------------------------------------------------------------
function tryDispatchPasteCommand(text) {
  var m = text.match(/^\s*\/paste\b\s*(.*)$/i);
  if (!m) return false;
  var hint = (m[1] || '').trim();
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Reading clipboard...');
  if (!navigator.clipboard || !navigator.clipboard.readText) {
    currentAiText = '✗ Clipboard API not available in this context.';
    finishAiMessage();
    setGenerating(false);
    return true;
  }
  navigator.clipboard.readText().then(function(clip) {
    clip = (clip || '').trim();
    if (!clip) {
      currentAiText = '✗ Clipboard is empty.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    appendToAiMessage('\nProbing form fields...');
    sendWithPromise('runFormFillAI').then(function(r) {
      if (!r.success) {
        currentAiText = '✗ ' + (r.error || 'probe failed');
        finishAiMessage();
        setGenerating(false);
        return;
      }
      // Reframe the prompt: instead of mapping from PROFILE → fields,
      // we map from PASTED BLOB → fields. We replace the "Profile:"
      // section with a "Pasted content:" section.
      var p = r.prompt;
      p = p.replace(/Profile:\n[\s\S]*?\n\nForm fields:/,
        'Pasted content (one blob, possibly multi-line):\n' + clip +
        '\n\n' +
        (hint ? 'Hint from user: ' + hint + '\n\n' : '') +
        'Form fields:');
      currentAiText = '';
      sendWithPromise('sendPrompt', p, '', '').then(function() {
        var s = currentAiText;
        var i = s.indexOf('{'), j = s.lastIndexOf('}');
        if (i < 0 || j <= i) {
          currentAiText += '\n\n✗ Could not parse JSON mapping.';
          finishAiMessage();
          setGenerating(false);
          return;
        }
        var parsed;
        try { parsed = JSON.parse(s.substring(i, j + 1)); }
        catch (e) {
          currentAiText += '\n\n✗ Invalid JSON: ' + e;
          finishAiMessage();
          setGenerating(false);
          return;
        }
        if (!parsed || Object.keys(parsed).length === 0) {
          currentAiText += '\n\n✗ Empty mapping — LLM didn\'t recognize ' +
                            'how to split this content.';
          finishAiMessage();
          setGenerating(false);
          return;
        }
        sendWithPromise('applyFormFillMap', {map: parsed}).then(function(r2){
          currentAiText = r2.success
              ? '✓ Smart-pasted into ' + r2.filled + ' of ' + r2.total +
                ' fields.'
              : '✗ ' + (r2.error || 'apply failed');
          finishAiMessage();
          setGenerating(false);
        });
      });
    });
  }, function(err) {
    currentAiText = '✗ Could not read clipboard: ' + err;
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Daily digest: /digest
// Aggregates the user's last-24h activity from the local memory
// service and asks the LLM to compose a tight briefing. Pure
// client-side composition over existing IPCs (listMemoryDocs,
// listActiveAgents). Privacy: every data point is already on the
// user's disk; the digest never sends anything to the network.
// --------------------------------------------------------------
function tryDispatchDigestCommand(text) {
  if (!/^\s*\/digest\b/i.test(text)) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Reading your recent activity...');
  var nowUnix = Math.floor(Date.now() / 1000);
  var dayAgo = nowUnix - 86400;
  // Pull more docs than we strictly need so the 24-hour filter has
  // material on a heavy-browsing day.
  Promise.all([
    sendWithPromise('listMemoryDocs', 200),
    sendWithPromise('listActiveAgents').catch(function(){ return null; })
  ]).then(function(arr) {
    var memRes = arr[0] || {};
    var agentRes = arr[1] || {agents: []};
    var docs = (memRes.docs || []).filter(function(d){
      return d.visited_at_unix && d.visited_at_unix >= dayAgo;
    });
    var agents = (agentRes.agents || []);

    if (!docs.length && !agents.length) {
      currentAiText = 'Nothing in the last 24 hours yet — browse a few ' +
                       'pages or wait for watchers to fire.';
      finishAiMessage();
      setGenerating(false);
      return;
    }

    // Build a compact context blob for the LLM. Cap to keep prompt
    // budget reasonable on the bundled model (~3k tokens of input).
    var lines = [];
    lines.push('Pages visited in the last 24h: ' + docs.length);
    docs.slice(0, 80).forEach(function(d) {
      var ts = new Date(d.visited_at_unix * 1000).toISOString().slice(11, 16);
      lines.push('  [' + ts + '] ' + (d.title || d.host || d.url) +
                 '  (' + d.host + ')');
    });
    if (agents.length) {
      lines.push('');
      lines.push('Background automations currently in flight: ' +
                 agents.length);
      agents.forEach(function(a) {
        var step = a.total_steps ? (a.current_step + '/' + a.total_steps)
                                  : String(a.current_step);
        lines.push('  · ' + (a.script_name || a.script_id) +
                   '  step ' + step +
                   (a.status_note ? '  — ' + a.status_note : ''));
      });
    }
    var prompt =
      'Compose a tight daily-digest briefing for the user based on ' +
      'their local browsing + automation activity over the last 24 ' +
      'hours. Group pages into 2-4 themes by topic, summarize each ' +
      'theme in one sentence, then list any in-flight automations. ' +
      'No preamble, no markdown headers, just the briefing in plain ' +
      'paragraphs. Aim for under 200 words.\n\n' +
      'Activity:\n' + lines.join('\n');
    currentAiText = '';
    sendWithPromise('sendPrompt', prompt, '', '').then(function() {
      finishAiMessage();
      setGenerating(false);
    }).catch(function() {
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Password vault: /vault [list|find|fill|add|delete|generate]
//
// All credentials are OSCrypt-encrypted on disk
// (~/.moltbrowser/vault.enc). Passwords are never echoed to chat;
// /vault list returns usernames + sites only, and /vault fill never
// shows the password in the message — it writes straight into the
// active tab's login form.
// --------------------------------------------------------------
function tryDispatchVaultCommand(text) {
  var m = text.match(/^\s*\/vault\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  var parts = rest.split(/\s+/);
  var sub = (parts[0] || 'help').toLowerCase();
  addUserMessage(text);
  startAiMessage();

  function fmtTime(unix){
    if (!unix) return 'never';
    var d = new Date(unix * 1000);
    return d.toLocaleDateString();
  }

  if (sub === 'help' || sub === '') {
    currentAiText =
      'Vault commands:\n' +
      '  /vault list                       — list saved credentials\n' +
      '  /vault find                       — entries for active tab\'s site\n' +
      '  /vault fill                       — autofill active tab\n' +
      '  /vault fill <id>                  — autofill with specific entry\n' +
      '  /vault add <host> <user> <pass>   — add (best done from settings)\n' +
      '  /vault delete <id>                — remove an entry\n' +
      '  /vault generate [length]          — generate strong password\n' +
      '\nAll entries are encrypted at rest via OSCrypt (Keychain on macOS).';
    finishAiMessage();
    setGenerating(false);
    return true;
  }

  if (sub === 'list') {
    sendWithPromise('vaultList').then(function(r) {
      var entries = (r && r.entries) || [];
      if (!entries.length) {
        currentAiText = 'Vault is empty. Use /vault add <host> <user> <pass>.';
      } else {
        var lines = ['Vault (' + entries.length + ' entries):'];
        entries.forEach(function(e){
          lines.push('  • ' + e.site_host + ' — ' + (e.username || '(no user)') +
                     '   [id ' + e.id.slice(0, 8) + '…]' +
                     '   last used: ' + fmtTime(e.last_used_unix));
        });
        currentAiText = lines.join('\n');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }

  if (sub === 'find') {
    sendWithPromise('vaultFindForActive').then(function(r) {
      var host = r && r.host || '(unknown)';
      var matches = (r && r.matches) || [];
      if (!matches.length) {
        currentAiText = 'No vault entries match ' + host + '.';
      } else {
        var lines = ['Matches for ' + host + ':'];
        matches.forEach(function(m){
          lines.push('  • ' + m.username + '   [id ' + m.id.slice(0, 8) +
                     '…]   last used: ' + fmtTime(m.last_used_unix));
        });
        lines.push('');
        lines.push('Fill with: /vault fill ' + matches[0].id.slice(0, 8));
        currentAiText = lines.join('\n');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }

  if (sub === 'fill') {
    var idArg = (parts[1] || '').trim();
    // Short-id lookup: user can pass any 8+ char prefix.
    var doFill = function(id) {
      sendWithPromise('vaultAutofill', id || '').then(function(r) {
        if (r.success) {
          currentAiText = '✓ Filled ' + r.filled + ' login form' +
              (r.filled === 1 ? '' : 's') + ' (' +
              r.total_password_fields + ' password field' +
              (r.total_password_fields === 1 ? '' : 's') + ' on page).';
        } else {
          currentAiText = '✗ ' + (r.error || 'autofill failed');
        }
        finishAiMessage();
        setGenerating(false);
      });
    };
    if (!idArg) { doFill(''); return true; }
    // Resolve short id → full id.
    sendWithPromise('vaultList').then(function(r) {
      var hit = (r.entries || []).find(function(e){
        return e.id.indexOf(idArg) === 0;
      });
      if (!hit) {
        currentAiText = '✗ No entry with id prefix "' + idArg + '"';
        finishAiMessage();
        setGenerating(false);
        return;
      }
      doFill(hit.id);
    });
    return true;
  }

  if (sub === 'add') {
    var host = parts[1], user = parts[2];
    var pass = parts.slice(3).join(' ');
    if (!host || !user || !pass) {
      currentAiText = 'Usage: /vault add <host> <username> <password>';
      finishAiMessage();
      setGenerating(false);
      return true;
    }
    sendWithPromise('vaultAdd',
        {site_host: host, username: user, password: pass}).then(function(r) {
      if (r.success) {
        currentAiText = '✓ Saved credential for ' + host + ' (id ' +
                         r.id.slice(0, 8) + '…).\n' +
                         '  Tip: clear the chat to remove the plaintext ' +
                         'password from screen.';
      } else {
        currentAiText = '✗ ' + (r.error || 'save failed');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }

  if (sub === 'delete' || sub === 'remove' || sub === 'rm') {
    var idArg2 = (parts[1] || '').trim();
    if (!idArg2) {
      currentAiText = 'Usage: /vault delete <id-prefix>';
      finishAiMessage();
      setGenerating(false);
      return true;
    }
    sendWithPromise('vaultList').then(function(r) {
      var hit = (r.entries || []).find(function(e){
        return e.id.indexOf(idArg2) === 0;
      });
      if (!hit) {
        currentAiText = '✗ No entry matches.';
        finishAiMessage();
        setGenerating(false);
        return;
      }
      sendWithPromise('vaultDelete', hit.id).then(function(r2) {
        currentAiText = r2.success
            ? '✓ Deleted ' + hit.site_host + ' / ' + hit.username
            : '✗ ' + (r2.error || 'delete failed');
        finishAiMessage();
        setGenerating(false);
      });
    });
    return true;
  }

  if (sub === 'generate' || sub === 'gen') {
    // Client-side via crypto.getRandomValues — keeps the generated
    // password out of any LLM/IPC path until the user explicitly
    // saves it.
    var len = parseInt(parts[1] || '20', 10);
    if (isNaN(len) || len < 8 || len > 128) len = 20;
    var alphabet =
      'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' +
      '0123456789!@#$%^&*-_=+';
    var buf = new Uint32Array(len);
    crypto.getRandomValues(buf);
    var pw = '';
    for (var i = 0; i < len; i++) pw += alphabet[buf[i] % alphabet.length];
    currentAiText = 'Generated password (' + len + ' chars):\n\n  ' + pw +
                    '\n\nTip: pair with /vault add <host> <user> ' + pw;
    finishAiMessage();
    setGenerating(false);
    return true;
  }

  currentAiText = 'Unknown /vault subcommand: ' + sub + '. Try /vault help.';
  finishAiMessage();
  setGenerating(false);
  return true;
}

// --------------------------------------------------------------
// Inline translate: /translate [lang] <text or selection>
// If no text supplied, uses the current text selection (via the
// runMoltAction eval shim). Default target language is English.
// --------------------------------------------------------------
function tryDispatchTranslateCommand(text) {
  var m = text.match(/^\s*\/translate\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  // First token might be a target language code (en, es, fr, de, ...).
  var lang = 'English';
  var body = rest;
  var firstSpace = rest.indexOf(' ');
  var firstTok = firstSpace > 0 ? rest.slice(0, firstSpace) : rest;
  var langMap = {
    en: 'English', es: 'Spanish', fr: 'French', de: 'German',
    it: 'Italian', pt: 'Portuguese', ja: 'Japanese', zh: 'Chinese',
    ko: 'Korean', ar: 'Arabic', hi: 'Hindi', ru: 'Russian',
    tr: 'Turkish', nl: 'Dutch', sv: 'Swedish', pl: 'Polish'
  };
  if (firstTok && langMap[firstTok.toLowerCase()]) {
    lang = langMap[firstTok.toLowerCase()];
    body = rest.slice(firstSpace + 1).trim();
  }
  addUserMessage(text);
  startAiMessage();
  function doTranslate(src) {
    if (!src) {
      currentAiText = 'Nothing to translate. Use /translate [lang] <text>, ' +
                       'or select text on the page first.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var prompt =
      'Translate the following text to ' + lang + '. Preserve ' +
      'meaning and tone. Output the translation only, no preamble.\n\n' +
      'Text:\n' + src.slice(0, 8000);
    sendWithPromise('sendPrompt', prompt, '', '').then(function() {
      finishAiMessage();
      setGenerating(false);
    });
  }
  if (body) { doTranslate(body); return true; }
  // No text supplied — fall back to using the captured active-page
  // text (whatever the side panel snapshotted on tab-change). It's
  // not the live selection but gets us 80% there without needing a
  // generic eval IPC. The user can scope down by quoting a phrase.
  var sp = window.__moltCurrentTabContext;
  var fallback = sp && sp.text ? sp.text.slice(0, 4000) : '';
  if (!fallback) {
    currentAiText = 'Usage: /translate [lang] <text>  (or open a page ' +
                     'with content first).';
    finishAiMessage();
    setGenerating(false);
    return true;
  }
  doTranslate(fallback);
  return true;
}

// --------------------------------------------------------------
// Form filler v2: /fill ai (LLM fallback)
// Two-phase: phase 1 asks the C++ side to probe form fields and
// return a ready prompt; phase 2 sends the LLM's JSON mapping back
// for application.
// --------------------------------------------------------------
function tryDispatchFillAICommand(text) {
  var m = text.match(/^\s*\/fill\s+ai\b\s*(.*)$/i);
  if (!m) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Probing form fields...');
  sendWithPromise('runFormFillAI').then(function(r) {
    if (!r.success) {
      currentAiText = '✗ ' + (r.error || 'probe failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    currentAiText = '';
    sendWithPromise('sendPrompt', r.prompt, '', '').then(function() {
      // Parse JSON from currentAiText.
      var s = currentAiText;
      var i = s.indexOf('{'), j = s.lastIndexOf('}');
      if (i < 0 || j <= i) {
        currentAiText += '\n\n✗ Could not parse JSON mapping.';
        finishAiMessage();
        setGenerating(false);
        return;
      }
      var parsed;
      try { parsed = JSON.parse(s.substring(i, j + 1)); }
      catch (e) {
        currentAiText += '\n\n✗ Invalid JSON: ' + e;
        finishAiMessage();
        setGenerating(false);
        return;
      }
      if (!parsed || typeof parsed !== 'object' ||
          Object.keys(parsed).length === 0) {
        currentAiText += '\n\n✗ Empty mapping — no fields confidently matched.';
        finishAiMessage();
        setGenerating(false);
        return;
      }
      sendWithPromise('applyFormFillMap', {map: parsed})
          .then(function(r2){
            currentAiText = r2.success
                ? '✓ Filled ' + r2.filled + ' of ' + r2.total +
                  ' fields via AI mapping.'
                : '✗ ' + (r2.error || 'apply failed');
            finishAiMessage();
            setGenerating(false);
          });
    });
  });
  return true;
}

function tryDispatchHistoryCommand(text) {
  var m = text.match(/^\s*\/history\b\s*(\d*)$/i);
  if (!m) return false;
  var limit = parseInt(m[1] || '200', 10);
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Loading your reading history...');
  sendWithPromise('listMemoryDocs', limit).then(function(r) {
    var docs = (r && r.docs) || [];
    if (!docs.length) {
      currentAiText = 'No memory documents yet — visit a few pages and ' +
                      'they\u2019ll show up here.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var clusters = clusterDocsByTitleKeywords(docs);
    // Stash on window so /cluster <n> can refer back to a specific
    // group without another IPC + re-clustering round-trip.
    window.__moltLastHistoryClusters = clusters;
    renderHistoryClusters(clusters, docs.length);
    setGenerating(false);
  }).catch(function(e) {
    currentAiText = '\u2717 ' + (e || 'history load failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// Simple greedy keyword clusterer:
//   1. Build a per-doc token bag from the title (stop-words stripped).
//   2. For each doc, look at every existing cluster; if Jaccard
//      similarity with the cluster's keyword set >= threshold, join;
//      otherwise start a new cluster.
//   3. Cluster name = the top-2 keywords by frequency in the cluster.
//
// Fast (~O(n*k) where k is current cluster count, plus tiny constants),
// deterministic, and "good enough" for the kind of grouping a user
// can read off a glance. Doesn't need an LLM call.
function clusterDocsByTitleKeywords(docs) {
  var STOP = MOLT_STOP_WORDS;
  function tokens(s) {
    var toks = (s || '').toLowerCase().match(/[a-z0-9]{3,}/g) || [];
    var seen = {};
    var out = [];
    toks.forEach(function(t){
      if (STOP[t]) return;
      if (seen[t]) return;
      seen[t] = true;
      out.push(t);
    });
    return out;
  }
  function jaccard(a, b) {
    if (!a.length || !b.length) return 0;
    var setB = {};
    b.forEach(function(t){ setB[t] = true; });
    var inter = 0;
    a.forEach(function(t){ if (setB[t]) inter++; });
    var uni = a.length + b.length - inter;
    return uni ? inter / uni : 0;
  }
  var THRESH = 0.20;  // tuned by hand on a sample of 200 docs
  var clusters = [];
  for (var i = 0; i < docs.length; i++) {
    var d = docs[i];
    var ts = tokens(d.title || d.host || '');
    if (!ts.length) continue;
    var bestIdx = -1, bestScore = 0;
    for (var c = 0; c < clusters.length; c++) {
      var s = jaccard(ts, clusters[c].keyTokens);
      if (s > bestScore) { bestScore = s; bestIdx = c; }
    }
    if (bestIdx >= 0 && bestScore >= THRESH) {
      var cl = clusters[bestIdx];
      cl.docs.push(d);
      // Merge tokens with frequency tracking.
      ts.forEach(function(t){ cl.tokenFreq[t] = (cl.tokenFreq[t] || 0) + 1; });
      // Recompute the canonical key token set (top-by-freq).
      cl.keyTokens = Object.keys(cl.tokenFreq).sort(function(a,b){
        return cl.tokenFreq[b] - cl.tokenFreq[a];
      }).slice(0, 8);
    } else {
      var freq = {};
      ts.forEach(function(t){ freq[t] = 1; });
      clusters.push({
        docs: [d],
        tokenFreq: freq,
        keyTokens: ts.slice(0, 8)
      });
    }
  }
  // Pick a display label per cluster: top-2 most-frequent tokens.
  clusters.forEach(function(cl){
    var top = Object.keys(cl.tokenFreq).sort(function(a,b){
      return cl.tokenFreq[b] - cl.tokenFreq[a];
    }).slice(0, 2);
    cl.label = top.map(function(t){
      return t.charAt(0).toUpperCase() + t.slice(1);
    }).join(' \u00b7 ') || 'Other';
  });
  // Largest clusters first.
  clusters.sort(function(a, b){ return b.docs.length - a.docs.length; });
  return clusters;
}

function renderHistoryClusters(clusters, totalDocs) {
  function esc(s){return (s+'').replace(/&/g,'&amp;').replace(/</g,'&lt;')
                                .replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
  function timeAgo(unix) {
    if (!unix) return '';
    var d = Date.now()/1000 - unix;
    if (d < 60)      return Math.round(d) + 's ago';
    if (d < 3600)   return Math.round(d/60) + 'm ago';
    if (d < 86400)  return Math.round(d/3600) + 'h ago';
    return Math.round(d/86400) + 'd ago';
  }
  var html = '<div class="history-summary">' + totalDocs +
             ' pages in your local memory, grouped into ' +
             clusters.length + ' topic' +
             (clusters.length === 1 ? '' : 's') + '.</div>';
  clusters.forEach(function(cl, idx){
    var rows = cl.docs.map(function(d){
      return '<li><a href="' + esc(d.url) + '" target="_blank">' +
             esc(d.title || d.host || d.url) + '</a>' +
             '<span class="hist-meta">' + esc(d.host) + ' \u00b7 ' +
             timeAgo(d.visited_at_unix) + '</span></li>';
    }).join('');
    html += '<details class="history-cluster"' +
            (idx < 3 ? ' open' : '') + '>' +
            '<summary><span class="hist-label">' + esc(cl.label) + '</span>' +
            '<span class="hist-count">' + cl.docs.length + '</span></summary>' +
            '<ul>' + rows + '</ul></details>';
  });
  // Render in the most recent AI message slot.
  currentAiText = '__HISTORY__';
  finishAiMessage();
  var msgs = document.querySelectorAll('#messages .message.ai');
  var last = msgs[msgs.length - 1];
  if (last) last.querySelector('.text').innerHTML = html;
}

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
  if (text.charAt(0) === '/' && tryDispatchPdfCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchBookmarksCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchAskTabsCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchSandboxCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTrackersCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchReputationCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchHopsCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTorCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchJsCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTriageCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchWatchCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchFillCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchPasteCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchDigestCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchVaultCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTranslateCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchFillAICommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchHistoryCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchReaderCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchReceiptCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchChaptersCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchClusterCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchPlanCommand(text)) {
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
    // PDF chat: if /pdf was just used, prepend the top-K relevant
    // chunks from the loaded PDF to ground the answer in that document.
    if (pdfContext && pdfContext.text) {
      var pdfChunks = chunkPageText(pdfContext.text, 600);
      var pdfTop = rankChunksByQuery(pdfChunks, text, 6);
      if (pdfTop.length) {
        pageCtx = (pageCtx ? (pageCtx + '\n\n') : '') +
                  'Loaded PDF (' + (pdfContext.host || pdfContext.url) +
                  ') — most relevant excerpts:\n' +
                  pdfTop.map(function(c){ return '- ' + c; }).join('\n');
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

// --------------------------------------------------------------
// Voice mode (Tier 5).
// Click the mic, the button goes red, speak. Click again (or wait
// 30s) and we stop, encode the captured audio as a 16-kHz mono WAV,
// base64 it, and ship to the transcribeAudio IPC which hands off
// to bundled whisper.cpp. The result lands in the chat input box
// — the user can review and edit before sending.
//
// We do the resampling ourselves rather than asking the browser
// for 16k input, because most macOS mics report 44.1k or 48k and
// the WebAudio constraint isn't reliably honored across hardware.
// --------------------------------------------------------------
var voiceState = {
  recording: false,
  ctx: null,
  source: null,
  processor: null,
  stream: null,
  chunks: [],       // Float32Array slices captured in real time
  inputRate: 0,
  startedAt: 0,
  maxMs: 30000      // hard cap so a forgotten mic doesn't run forever
};

function _wavBytesFromPCM(pcm16, sampleRate) {
  // pcm16 is Int16Array of mono samples.
  var byteLen = 44 + pcm16.length * 2;
  var ab = new ArrayBuffer(byteLen);
  var dv = new DataView(ab);
  function w(off, str){
    for (var i = 0; i < str.length; i++) dv.setUint8(off + i, str.charCodeAt(i));
  }
  w(0, 'RIFF');
  dv.setUint32(4, byteLen - 8, true);
  w(8, 'WAVE');
  w(12, 'fmt ');
  dv.setUint32(16, 16, true);          // fmt chunk size
  dv.setUint16(20, 1, true);           // PCM format
  dv.setUint16(22, 1, true);           // mono
  dv.setUint32(24, sampleRate, true);
  dv.setUint32(28, sampleRate * 2, true);
  dv.setUint16(32, 2, true);           // block align
  dv.setUint16(34, 16, true);          // bits per sample
  w(36, 'data');
  dv.setUint32(40, pcm16.length * 2, true);
  for (var i = 0; i < pcm16.length; i++) {
    dv.setInt16(44 + i * 2, pcm16[i], true);
  }
  return new Uint8Array(ab);
}

function _resampleTo16k(floatBuf, fromRate) {
  if (fromRate === 16000) return floatBuf;
  var ratio = fromRate / 16000;
  var outLen = Math.floor(floatBuf.length / ratio);
  var out = new Float32Array(outLen);
  for (var i = 0; i < outLen; i++) {
    // Cheap linear interp; whisper is forgiving.
    var srcIdx = i * ratio;
    var s0 = Math.floor(srcIdx);
    var s1 = Math.min(s0 + 1, floatBuf.length - 1);
    var t = srcIdx - s0;
    out[i] = floatBuf[s0] * (1 - t) + floatBuf[s1] * t;
  }
  return out;
}

function _floatToInt16(buf) {
  var out = new Int16Array(buf.length);
  for (var i = 0; i < buf.length; i++) {
    var s = Math.max(-1, Math.min(1, buf[i]));
    out[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
  }
  return out;
}

function _bytesToBase64(bytes) {
  // Chunk to avoid blowing the call-stack on a long recording.
  var chunkSize = 0x8000;
  var s = '';
  for (var i = 0; i < bytes.length; i += chunkSize) {
    s += String.fromCharCode.apply(null,
        bytes.subarray(i, Math.min(i + chunkSize, bytes.length)));
  }
  return btoa(s);
}

function toggleMic() {
  if (voiceState.recording) {
    _stopRecording();
  } else {
    _startRecording();
  }
}

function _startRecording() {
  var btn = document.getElementById('micBtn');
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    addErrorMessage('Mic API unavailable in this context.');
    return;
  }
  navigator.mediaDevices.getUserMedia({audio: {
    channelCount: 1,
    noiseSuppression: true,
    echoCancellation: true
  }}).then(function(stream) {
    voiceState.stream = stream;
    voiceState.chunks = [];
    voiceState.startedAt = Date.now();
    // Use the legacy ScriptProcessorNode — universally supported.
    // AudioWorklet would be cleaner but adds complexity for a v1.
    voiceState.ctx = new (window.AudioContext || window.webkitAudioContext)();
    voiceState.inputRate = voiceState.ctx.sampleRate;
    voiceState.source = voiceState.ctx.createMediaStreamSource(stream);
    voiceState.processor = voiceState.ctx.createScriptProcessor(4096, 1, 1);
    voiceState.processor.onaudioprocess = function(e) {
      var chan = e.inputBuffer.getChannelData(0);
      // Copy because the underlying buffer is reused next frame.
      voiceState.chunks.push(new Float32Array(chan));
      if (Date.now() - voiceState.startedAt > voiceState.maxMs) {
        _stopRecording();
      }
    };
    voiceState.source.connect(voiceState.processor);
    voiceState.processor.connect(voiceState.ctx.destination);
    voiceState.recording = true;
    if (btn) {
      btn.classList.add('recording');
      btn.textContent = '⏺';
      btn.title = 'Recording... click to stop (auto-stops at 30s)';
    }
  }, function(err) {
    addErrorMessage('Mic permission denied: ' + err);
  });
}

function _stopRecording() {
  var btn = document.getElementById('micBtn');
  if (!voiceState.recording) return;
  voiceState.recording = false;
  try { voiceState.processor.disconnect(); } catch(e) {}
  try { voiceState.source.disconnect(); } catch(e) {}
  try { voiceState.ctx.close(); } catch(e) {}
  if (voiceState.stream) {
    voiceState.stream.getTracks().forEach(function(t){ t.stop(); });
  }
  // Concatenate all captured chunks into one Float32Array.
  var total = voiceState.chunks.reduce(function(s, c){ return s + c.length; }, 0);
  var merged = new Float32Array(total);
  var off = 0;
  voiceState.chunks.forEach(function(c){
    merged.set(c, off);
    off += c.length;
  });
  voiceState.chunks = [];

  if (merged.length < 1600) {  // less than 0.1s at 16k
    if (btn) {
      btn.classList.remove('recording');
      btn.textContent = '🎙';
      btn.title = 'Hold or click to record (local Whisper)';
    }
    addErrorMessage('Recording too short.');
    return;
  }

  var resampled = _resampleTo16k(merged, voiceState.inputRate);
  var pcm16 = _floatToInt16(resampled);
  var wav = _wavBytesFromPCM(pcm16, 16000);
  var b64 = _bytesToBase64(wav);

  if (btn) {
    btn.classList.remove('recording');
    btn.classList.add('transcribing');
    btn.textContent = '…';
    btn.title = 'Transcribing locally...';
  }
  sendWithPromise('transcribeAudio', b64).then(function(r) {
    if (btn) {
      btn.classList.remove('transcribing');
      btn.textContent = '🎙';
      btn.title = 'Hold or click to record (local Whisper)';
    }
    if (!r.success) {
      addErrorMessage('Transcribe: ' + (r.error || 'unknown') +
                       (r.install_hint ? '\n' + r.install_hint : ''));
      return;
    }
    var input = document.getElementById('prompt');
    if (input) {
      // Append to whatever's already in the box; nice for "speak,
      // then add a clarifier and hit send".
      var prefix = input.value ? input.value + ' ' : '';
      input.value = prefix + r.text;
      input.focus();
    }
  });
}

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

  // Agent Inbox poller — refreshes the running-agents tray every 3s.
  // Cheap IPC (just a snapshot of an in-memory map) so polling at this
  // cadence is fine. Hides the tray when nothing's running so it
  // doesn't take up real estate.
  function renderAgents(agents) {
    var tray = document.getElementById('agentInbox');
    if (!tray) return;
    if (!agents || !agents.length) {
      tray.style.display = 'none';
      tray.innerHTML = '';
      return;
    }
    var rows = ['<div class="agent-inbox-header">' +
                'Running agents (' + agents.length + ')</div>'];
    agents.forEach(function(a) {
      var done = a.finished_at_unix && a.finished_at_unix > 0;
      var cls = !done ? '' : (a.succeeded ? ' done-ok' : ' done-err');
      var step = a.total_steps
          ? (a.current_step + '/' + a.total_steps)
          : (a.current_step + '');
      var note = a.status_note || '';
      if (done) note = a.succeeded ? 'Completed' : 'Failed';
      // Cheap text-escape: avoid breaking the DOM if a script name
      // contains stray < or &.
      function esc(s) { return (s + '').replace(/&/g,'&amp;')
                                       .replace(/</g,'&lt;')
                                       .replace(/>/g,'&gt;'); }
      rows.push(
        '<div class="agent-row">' +
          '<div class="agent-spinner' + cls + '"></div>' +
          '<div class="agent-name" title="' + esc(a.start_url) + '">' +
            esc(a.script_name) + '</div>' +
          '<div class="agent-progress">step ' + esc(step) + '</div>' +
          '<div class="agent-note">' + esc(note) + '</div>' +
        '</div>');
    });
    tray.innerHTML = rows.join('');
    tray.style.display = 'flex';
  }
  function pollAgents() {
    sendWithPromise('listActiveAgents').then(function(r) {
      renderAgents(r && r.agents);
    }).catch(function() {});
  }
  pollAgents();
  setInterval(pollAgents, 3000);
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
