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
/* Liquid Glass tokens — --accent is the single warm MoltBrowser red; violet is reserved for cloud/AI touches. */
:root{--accent:#e5484d;--accent-hi:#f26166;--violet:#a78bfa;--glass-bg:rgba(255,255,255,0.085);--glass-bg-hi:rgba(255,255,255,0.13);--glass-border:rgba(255,255,255,0.20);--glass-border-hi:rgba(255,255,255,0.32);--glass-shadow:0 24px 60px -16px rgba(0,0,0,0.66),inset 0 1px 0 rgba(255,255,255,0.32)}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0a0a0a;color:#e0e0e0;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:40px 20px;position:relative}
/* Liquid Glass ambient — heavily-blurred colour orbs behind all content so the translucent panels have something to refract. Sits behind normal-flow content via z-index:-1; pointer-events:none so it never intercepts clicks. */
.lg-ambient{position:fixed;inset:0;z-index:-1;pointer-events:none;overflow:hidden}
.lg-orb{position:absolute;border-radius:50%;filter:blur(80px);opacity:0.30;will-change:transform}
.lg-orb-red{width:46vw;height:46vw;background:radial-gradient(circle at center,#e5484d,transparent 70%);top:-8vw;left:-6vw;animation:lg-drift-a 34s ease-in-out infinite}
.lg-orb-violet{width:42vw;height:42vw;background:radial-gradient(circle at center,#a78bfa,transparent 70%);top:34vh;right:-8vw;opacity:0.32;animation:lg-drift-b 41s ease-in-out infinite}
.lg-orb-teal{width:40vw;height:40vw;background:radial-gradient(circle at center,#2bb6c4,transparent 70%);bottom:-10vw;left:18vw;opacity:0.26;animation:lg-drift-c 47s ease-in-out infinite}
.lg-orb-red2{width:30vw;height:30vw;background:radial-gradient(circle at center,#e5484d,transparent 70%);bottom:6vh;right:12vw;opacity:0.22;animation:lg-drift-b 38s ease-in-out infinite reverse}
@keyframes lg-drift-a{0%,100%{transform:translate(0,0)}50%{transform:translate(4vw,3vh)}}
@keyframes lg-drift-b{0%,100%{transform:translate(0,0)}50%{transform:translate(-3vw,4vh)}}
@keyframes lg-drift-c{0%,100%{transform:translate(0,0)}50%{transform:translate(3vw,-3vh)}}
.logo-area{display:flex;align-items:center;gap:12px;margin-bottom:4px}.logo-img{width:52px;height:52px;border-radius:14px;box-shadow:0 2px 12px rgba(0,0,0,0.5)}.logo-text{font-size:32px;font-weight:700;background:linear-gradient(135deg,#6366f1,#8b5cf6,#a855f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
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
.top-btn{padding:6px 14px;border-radius:14px;border:1px solid var(--glass-border);background:var(--glass-bg);color:#cfcfcf;font-size:12px;cursor:pointer;transition:all 0.2s;-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:inset 0 1px 0 rgba(255,255,255,0.14)}
.top-btn:hover{border-color:var(--glass-border-hi);background:var(--glass-bg-hi);color:#fff;transform:translateY(-1px)}
.context-info{font-size:11px;color:#444}
.chat-container{width:100%;max-width:720px;flex:1;display:flex;flex-direction:column}
.messages{flex:1;overflow-y:auto;padding:20px 0}
.message{margin-bottom:20px;padding:16px 20px;border-radius:12px;line-height:1.6;font-size:15px;max-width:90%}
.message.user{background:rgba(167,139,250,0.10);border:1px solid rgba(167,139,250,0.22);margin-left:auto;-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:0 20px 50px -18px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.30)}
.message.ai{background:rgba(255,255,255,0.085);border:1px solid rgba(255,255,255,0.12);-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:0 20px 50px -18px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.30)}
.message .label{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px;color:#6366f1}
.message.user .label{color:#8b5cf6}
.model-badge{display:inline-block;padding:2px 8px;border-radius:4px;background:#1a1a2e;color:#8b5cf6;font-size:11px;margin-bottom:8px}
.message .text{white-space:pre-wrap;word-wrap:break-word}
.message .text .cursor{display:inline-block;width:2px;height:16px;background:#6366f1;animation:blink 0.8s infinite;vertical-align:text-bottom;margin-left:1px}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
.input-area{display:flex;gap:12px;padding:20px 0;border-top:1px solid rgba(255,255,255,0.08)}
.input-area input{flex:1;padding:14px 20px;border-radius:16px;border:1px solid rgba(255,255,255,0.12);background:rgba(255,255,255,0.085);color:#e0e0e0;font-size:15px;outline:none;transition:border-color 0.2s,background 0.2s;-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:0 20px 50px -18px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.30)}
.input-area input:focus{border-color:var(--accent,#e5484d);background:rgba(255,255,255,0.07)}
.input-area button.send{padding:14px 28px;border-radius:16px;border:1px solid rgba(255,255,255,0.18);background:linear-gradient(135deg,var(--accent),var(--accent-hi));color:white;font-size:15px;font-weight:600;cursor:pointer;transition:opacity 0.2s,transform 0.15s;box-shadow:0 12px 30px -12px rgba(229,72,77,0.7),inset 0 1px 0 rgba(255,255,255,0.25)}
.input-area button.send:hover{opacity:0.92;transform:translateY(-1px)}
.input-area button.send:disabled{opacity:0.4;cursor:not-allowed}
.input-area button.cancel{padding:14px 16px;border-radius:12px;border:1px solid #f87171;background:transparent;color:#f87171;font-size:15px;cursor:pointer;display:none}
.input-area button.cancel.active{display:block}
/* Model Panel Overlay */
.model-overlay{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.55);-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px);z-index:100;display:none;align-items:center;justify-content:center}
.model-overlay.open{display:flex}
.model-dialog{background:var(--glass-bg);border:1px solid var(--glass-border);border-radius:20px;width:90%;max-width:560px;max-height:80vh;overflow-y:auto;padding:24px;-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:var(--glass-shadow)}
.model-dialog h2{font-size:18px;font-weight:600;margin-bottom:16px;color:#e0e0e0}
.model-card{padding:14px;border:1px solid var(--glass-border);border-radius:14px;margin-bottom:10px;background:rgba(255,255,255,0.04);transition:transform 0.15s,border-color 0.2s,background 0.2s;box-shadow:inset 0 1px 0 rgba(255,255,255,0.10)}
.model-card:hover{transform:translateY(-1px);border-color:var(--glass-border-hi);background:rgba(255,255,255,0.06)}
.model-card .name{font-size:14px;font-weight:600;color:#e0e0e0}
.model-card .meta{font-size:12px;color:#666;margin-top:2px}
.brandtile{display:inline-flex;align-items:center;justify-content:center;width:30px;height:30px;min-width:30px;border-radius:9px;flex:0 0 auto;overflow:hidden}
.model-card .card-head{display:flex;align-items:center;gap:10px;margin-bottom:8px}
.model-card .chead-txt{min-width:0}
.model-card .mco{font-size:10px;font-weight:700;letter-spacing:.05em;text-transform:uppercase;line-height:1.3;opacity:.92}
.model-card .card-actions{margin-top:10px;display:flex;gap:8px;align-items:center}
.model-card .btn{padding:6px 16px;border-radius:10px;font-size:12px;cursor:pointer;border:1px solid var(--glass-border);background:var(--glass-bg);color:#cfcfcf;transition:all 0.2s}
.model-card .btn:hover{border-color:var(--glass-border-hi);color:#fff;background:var(--glass-bg-hi)}
.model-card .btn:disabled{opacity:0.4;cursor:not-allowed}
.model-card .btn.primary{background:linear-gradient(135deg,var(--accent),var(--accent-hi));border-color:rgba(255,255,255,0.18);color:#fff;box-shadow:inset 0 1px 0 rgba(255,255,255,0.22)}
.model-card .btn.primary:hover{opacity:0.92}
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
/* Model chip — always-visible model selector at top of AI page */
.model-chip-wrap{position:relative;display:inline-block}
.model-chip{display:inline-flex;align-items:center;gap:8px;padding:6px 12px 6px 10px;border-radius:20px;background:var(--glass-bg);border:1px solid var(--glass-border);color:#e0e0e0;font-size:13px;font-weight:500;cursor:pointer;transition:all 0.2s;min-width:200px;-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:inset 0 1px 0 rgba(255,255,255,0.16)}
.model-chip:hover{border-color:var(--glass-border-hi);background:var(--glass-bg-hi);transform:translateY(-1px)}
.model-chip .icon{font-size:14px}
.model-chip .name{flex:1;text-align:left;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.model-chip .chevron{font-size:10px;color:#888;transition:transform 0.2s}
.model-chip.open .chevron{transform:rotate(180deg)}
.model-chip-progress{position:absolute;left:6px;top:50%;transform:translateY(-50%);width:20px;height:20px;display:none}
.model-chip.downloading .icon{display:none}
.model-chip.downloading .model-chip-progress{display:block}
.model-chip-progress svg{transform:rotate(-90deg)}
.model-chip-progress circle{fill:none;stroke:#222;stroke-width:2}
.model-chip-progress .fg{stroke:#8b5cf6;stroke-dasharray:50.27;stroke-dashoffset:50.27;transition:stroke-dashoffset 0.3s}
.model-chip-progress .pct{position:absolute;top:0;left:0;width:20px;height:20px;display:flex;align-items:center;justify-content:center;font-size:8px;font-weight:700;color:#a855f7}
.model-chip-dropdown{position:absolute;top:calc(100% + 6px);left:0;background:var(--glass-bg);border:1px solid var(--glass-border);border-radius:16px;padding:6px;min-width:280px;max-height:380px;overflow-y:auto;z-index:50;display:none;-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:var(--glass-shadow)}
.model-chip-dropdown.open{display:block}
.model-chip-item{display:flex;align-items:center;gap:10px;padding:10px 12px;border-radius:8px;cursor:pointer;transition:background 0.15s}
.model-chip-item:hover{background:rgba(255,255,255,0.08)}
.model-chip-item.active{background:rgba(74,222,128,0.14)}
.model-chip-item .mname{font-size:13px;color:#e0e0e0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.model-chip-item .msize{font-size:11px;color:#666}
.model-chip-item .mco{font-size:9.5px;font-weight:700;letter-spacing:.05em;text-transform:uppercase;line-height:1.25;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;opacity:.92}
.model-chip-item .mstatus{font-size:10px;padding:2px 8px;border-radius:10px;font-weight:600;text-transform:uppercase;letter-spacing:0.5px}
.model-chip-item .mstatus.active{background:#1a3a1a;color:#4ade80}
.model-chip-item .mstatus.downloaded{background:#1a1a3a;color:#8b5cf6}
.model-chip-item .mstatus.available{background:#1a1a1a;color:#666}
.model-chip-item .mstatus.downloading{background:#3a2e1a;color:#fbbf24}
.mcd-header{padding:9px 12px 4px;font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:0.6px;color:#777}
.mcd-footer{padding:10px 12px;font-size:12px;color:var(--violet);cursor:pointer;border-top:1px solid rgba(255,255,255,0.08);text-align:center}
.mcd-footer:hover{background:rgba(167,139,250,0.10)}
/* Cloud connect CTA — the top, most-prominent picker option. Violet accent (cloud/AI touch) on a translucent glass tile. */
.mcd-cloud-cta{display:flex;align-items:center;gap:10px;padding:11px 12px;margin-bottom:4px;border-radius:12px;cursor:pointer;background:rgba(167,139,250,0.10);border:1px solid rgba(167,139,250,0.22);transition:background 0.2s,border-color 0.2s,transform 0.15s;box-shadow:inset 0 1px 0 rgba(255,255,255,0.12)}
.mcd-cloud-cta:hover{background:rgba(167,139,250,0.16);border-color:rgba(167,139,250,0.35);transform:translateY(-1px)}
.mcd-cloud-cta .mcc-icon{font-size:16px}
.mcd-cloud-cta .mcc-text{flex:1;display:flex;flex-direction:column;font-size:13px;font-weight:600;color:#efeaff}
.mcd-cloud-cta .mcc-sub{font-size:11px;font-weight:400;color:#a99fd0;margin-top:1px}
.mcd-cloud-cta .mcc-arrow{font-size:14px;color:var(--violet)}
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
/* Code block copy button */
.code-wrap{position:relative;margin:10px 0}
.code-wrap pre{margin:0}
.code-copy{position:absolute;top:6px;right:6px;padding:3px 10px;border-radius:6px;border:1px solid #444;background:#222;color:#888;font-size:11px;cursor:pointer;opacity:0;transition:opacity 0.2s}
.code-wrap:hover .code-copy{opacity:1}
.code-copy:hover{color:#e0e0e0;border-color:var(--accent)}
.code-copy.copied{color:#4ade80;border-color:#4ade80}
/* Message actions */
.msg-actions{display:flex;gap:6px;margin-top:8px;opacity:0;transition:opacity 0.2s}
.message:hover .msg-actions{opacity:1}
.msg-action{padding:3px 10px;border-radius:6px;border:1px solid #333;background:none;color:#666;font-size:11px;cursor:pointer}
.msg-action:hover{color:#e0e0e0;border-color:var(--accent)}
/* Search bar */
.search-bar{padding:8px 0;width:100%;max-width:720px;display:none}
.search-bar.open{display:flex}
.search-bar input{flex:1;padding:8px 14px;border-radius:14px;border:1px solid var(--glass-border);background:var(--glass-bg);color:#e0e0e0;font-size:13px;outline:none;-webkit-backdrop-filter:blur(26px) saturate(1.7);backdrop-filter:blur(26px) saturate(1.7);box-shadow:inset 0 1px 0 rgba(255,255,255,0.14)}
.search-bar input:focus{border-color:var(--accent)}
.search-bar .search-count{font-size:11px;color:#888;padding:8px 10px}
.search-bar .search-close{background:none;border:none;color:#888;cursor:pointer;font-size:16px;padding:4px 8px}
.highlight{background:var(--accent);color:#fff;border-radius:2px;padding:0 2px}
/* Reduced motion — freeze the ambient orb drift and any hover lift. */
@media (prefers-reduced-motion: reduce){
  .lg-orb{animation:none!important}
  .status-dot.loading{animation:none}
  .top-btn:hover,.model-chip:hover,.model-card:hover,.mcd-cloud-cta:hover,.input-area button.send:hover{transform:none}
}
/* Light theme — the page has no data-theme toggle here, so honour the OS light preference: no ambient, light frosted surfaces (rgba(255,255,255,0.65)+blur), dark borders, dark text. Glass tokens are re-tuned so the same rules render correctly on light. */
@media (prefers-color-scheme: light){
  :root{--glass-bg:rgba(255,255,255,0.65);--glass-bg-hi:rgba(255,255,255,0.82);--glass-border:rgba(0,0,0,0.10);--glass-border-hi:rgba(0,0,0,0.18);--glass-shadow:0 18px 44px -18px rgba(0,0,0,0.28),inset 0 1px 0 rgba(255,255,255,0.85)}
  body{background:#f4f5f8;color:#1c1c22}
  .lg-ambient{display:none}
  .subtitle,.status-text{color:#5b5b66}
  .top-btn{color:#3a3a44;box-shadow:inset 0 1px 0 rgba(255,255,255,0.7)}
  .top-btn:hover{color:#111}
  .message.ai{box-shadow:0 18px 44px -18px rgba(0,0,0,0.28),inset 0 1px 0 rgba(255,255,255,0.85)}
  .message.user{background:rgba(167,139,250,0.16);border-color:rgba(124,92,214,0.30)}
  .message .text,.model-card .name,.model-chip .name,.model-chip-item .mname{color:#1c1c22}
  .input-area input{color:#1c1c22;box-shadow:0 18px 44px -18px rgba(0,0,0,0.28),inset 0 1px 0 rgba(255,255,255,0.85)}
  .input-area input:focus{background:rgba(255,255,255,0.85)}
  .model-chip{color:#1c1c22;box-shadow:inset 0 1px 0 rgba(255,255,255,0.75)}
  .model-dialog h2{color:#1c1c22}
  .model-card{background:rgba(255,255,255,0.5);box-shadow:inset 0 1px 0 rgba(255,255,255,0.7)}
  .model-card:hover{background:rgba(255,255,255,0.7)}
  .model-card .btn{color:#3a3a44}
  .model-card .btn:hover{color:#111}
  .model-chip-item:hover{background:rgba(0,0,0,0.05)}
  .model-chip-item .mstatus.available{background:rgba(0,0,0,0.06);color:#777}
  .search-bar input{color:#1c1c22;box-shadow:inset 0 1px 0 rgba(255,255,255,0.7)}
  .mcd-cloud-cta{background:rgba(167,139,250,0.14);border-color:rgba(124,92,214,0.28)}
  .mcd-cloud-cta .mcc-text{color:#3a2a66}
  .mcd-cloud-cta .mcc-sub{color:#6b5a99}
  .code-copy{background:rgba(255,255,255,0.7);border-color:rgba(0,0,0,0.12);color:#555}
  .model-overlay{background:rgba(0,0,0,0.28)}
}
</style>
</head>
<body>
<div class="lg-ambient" aria-hidden="true"><span class="lg-orb lg-orb-red"></span><span class="lg-orb lg-orb-violet"></span><span class="lg-orb lg-orb-teal"></span><span class="lg-orb lg-orb-red2"></span></div>
<div class="logo-area"><img class="logo-img" src="data:image/png;base64,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" alt="MoltBrowser"><div class="logo-text">MoltBrowser AI</div></div>
<div class="subtitle">Local AI — Private by Design — Powered by llama.cpp</div>
<div class="hw-info" id="hwInfo"></div>
<div class="top-bar">
  <div class="status-bar">
    <div class="status-dot offline" id="statusDot"></div>
    <div class="status-text" id="statusText">Initializing...</div>
  </div>
  <div class="top-actions">
    <!-- Always-visible model selector chip -->
    <div class="model-chip-wrap">
      <button class="model-chip" id="modelChip" onclick="toggleModelDropdown(event)">
        <span class="icon">&#129302;</span>
        <span class="model-chip-progress" id="modelChipProgress">
          <svg width="20" height="20" viewBox="0 0 20 20">
            <circle cx="10" cy="10" r="8"></circle>
            <circle class="fg" id="modelChipProgressFg" cx="10" cy="10" r="8"></circle>
          </svg>
          <span class="pct" id="modelChipPct">0%</span>
        </span>
        <span class="name" id="modelChipName">No Model</span>
        <span class="chevron">&#9662;</span>
      </button>
      <div class="model-chip-dropdown" id="modelChipDropdown"></div>
    </div>
    <button class="top-btn" onclick="newChat()">New Chat</button>
    <button class="top-btn" onclick="toggleSearch()">Search</button>
    <button class="top-btn" onclick="exportChat()">Export</button>
    <button class="top-btn" onclick="importChat()">Import</button>
    <button class="top-btn" onclick="toggleModelPanel()">Manage</button>
    <button class="top-btn" onclick="window.location='molt://ai-settings/'">Settings</button>
  </div>
  <div class="context-info" id="contextInfo"></div>
</div>
<div class="search-bar" id="searchBar">
  <input type="text" id="searchInput" placeholder="Search messages..." oninput="doSearch()">
  <span class="search-count" id="searchCount"></span>
  <button class="search-close" onclick="toggleSearch()">&times;</button>
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
var codeBlockId = 0;
function renderMarkdown(text) {
  var s = esc(text);
  s = s.replace(/```(\w*)\n([\s\S]*?)```/g, function(m, lang, code) {
    var id = 'cb-' + (++codeBlockId);
    var langLabel = lang ? '<span style="position:absolute;top:6px;left:10px;font-size:10px;color:#666;text-transform:uppercase">' + lang + '</span>' : '';
    return '<div class="code-wrap">' + langLabel +
      '<button class="code-copy" onclick="copyCode(\'' + id + '\',this)">Copy</button>' +
      '<pre id="' + id + '" style="background:#1a1a2e;padding:' + (lang ? '26px 12px 12px' : '12px') + ';border-radius:8px;overflow-x:auto;font-size:13px;border:1px solid #2a2a4a"><code>' + code.trim() + '</code></pre></div>';
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
  if (currentAiEl) {
    currentAiEl.innerHTML = renderMarkdown(currentAiText);
    var actions = document.createElement('div');
    actions.className = 'msg-actions';
    actions.innerHTML = '<button class="msg-action" onclick="copyResponse(this)">Copy response</button>';
    currentAiEl.parentNode.appendChild(actions);
  }
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

  // History = every message BEFORE the just-pushed user turn, as a JSON
  // array of {role, content} objects. The C++ side (HandleSendPrompt)
  // owns all chat templating — no model markers are assembled in JS.
  var prevHistory = conversationHistory.length > 1
      ? JSON.stringify(
            conversationHistory.slice(0, conversationHistory.length - 1))
      : '';

  // args[5] = the picked model id. A "provider:model" cloud id routes to the
  // frontier-API path; a plain id (or '') uses the local runtime.
  sendWithPromise('sendPrompt', t, prevHistory, '', '',
                  activeModelId || pickedModelId || '').then(function(r) {
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

// Brand tiles: maker logo + name shown above each model in the picker.
// Inline SVG (CSP-safe, no network); glyphs hard-code their own colors.
// "Gemma" is used instead of "Google" per the zero-"Google"-string rule.
var MOLT_BRANDS = {
  'Meta': {c:'#0866FF', t:'rgba(8,102,255,.15)', g:'<path d="M12 12.1C10.3 9.2 8.7 7.7 6.9 7.7 4.6 7.7 3 9.8 3 12.3s1.6 4.6 3.9 4.6c1.8 0 3.4-1.5 5.1-4.8M12 12.1c1.7-2.9 3.3-4.4 5.1-4.4 2.3 0 3.9 2.1 3.9 4.6s-1.6 4.6-3.9 4.6c-1.8 0-3.4-1.5-5.1-4.8" fill="none" stroke="#0866FF" stroke-width="2.1" stroke-linecap="round"/>'},
  'Qwen': {c:'#7C3AED', t:'rgba(124,58,237,.16)', g:'<path d="M12 2.7l8 4.6v9.4l-8 4.6-8-4.6V7.3z" fill="none" stroke="#7C3AED" stroke-width="1.8"/><path d="M8.3 9.2l3.7 2.1 3.7-2.1M12 11.3v4.5" fill="none" stroke="#7C3AED" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'},
  'Gemma': {c:'#4285F4', t:'rgba(66,133,244,.16)', g:'<path d="M12 3c.6 4.6 3.4 7.4 8 8-4.6.6-7.4 3.4-8 8-.6-4.6-3.4-7.4-8-8 4.6-.6 7.4-3.4 8-8z" fill="#4285F4"/>'},
  'Mistral AI': {c:'#FA500F', t:'rgba(250,80,15,.14)', g:'<g><rect x="3.5" y="5" width="17" height="3.1" rx="1" fill="#FFCC33"/><rect x="3.5" y="10.4" width="17" height="3.1" rx="1" fill="#FF7A1A"/><rect x="3.5" y="15.8" width="17" height="3.1" rx="1" fill="#F7101B"/></g>'},
  'Microsoft': {c:'#00A4EF', t:'rgba(0,164,239,.10)', g:'<g><rect x="3.6" y="3.6" width="7.4" height="7.4" fill="#F25022"/><rect x="13" y="3.6" width="7.4" height="7.4" fill="#7FBA00"/><rect x="3.6" y="13" width="7.4" height="7.4" fill="#00A4EF"/><rect x="13" y="13" width="7.4" height="7.4" fill="#FFB900"/></g>'},
  'OpenAI': {c:'#0FA47F', t:'rgba(15,164,127,.15)', g:'<g fill="none" stroke="#0FA47F" stroke-width="1.5"><ellipse cx="12" cy="12" rx="3.4" ry="8"/><ellipse cx="12" cy="12" rx="3.4" ry="8" transform="rotate(60 12 12)"/><ellipse cx="12" cy="12" rx="3.4" ry="8" transform="rotate(120 12 12)"/></g>'},
  'TinyLlama': {c:'#14B8A6', t:'rgba(20,184,166,.16)', g:'<path d="M8.6 4.4c-.6 0-1 .5-1 1.4l.3 3c-1.4.6-2.3 2-2.3 3.9v3.4c0 2.3 1.6 3.9 3.9 3.9h5c2.3 0 3.9-1.6 3.9-3.9v-3.4c0-1.9-.9-3.3-2.3-3.9l.3-3c0-.9-.4-1.4-1-1.4-.7 0-1.4.9-1.9 2.2-.9-.3-2-.3-3 0-.5-1.3-1.2-2.2-1.9-2.2z" fill="#14B8A6"/><circle cx="10" cy="13.2" r=".95" fill="#fff"/><circle cx="14" cy="13.2" r=".95" fill="#fff"/>'},
  'Anthropic': {c:'#D97757', t:'rgba(217,119,87,.15)', g:'<g stroke="#D97757" stroke-width="2" stroke-linecap="round"><path d="M12 4v16M4 12h16M6.3 6.3l11.4 11.4M17.7 6.3L6.3 17.7"/></g>'},
  'Gemini': {c:'#4285F4', t:'rgba(66,133,244,.16)', g:'<path d="M12 3c.6 4.6 3.4 7.4 8 8-4.6.6-7.4 3.4-8 8-.6-4.6-3.4-7.4-8-8 4.6-.6 7.4-3.4 8-8z" fill="#4285F4"/>'},
  'Mistral': {c:'#FA500F', t:'rgba(250,80,15,.14)', g:'<g><rect x="3.5" y="5" width="17" height="3.1" rx="1" fill="#FFCC33"/><rect x="3.5" y="10.4" width="17" height="3.1" rx="1" fill="#FF7A1A"/><rect x="3.5" y="15.8" width="17" height="3.1" rx="1" fill="#F7101B"/></g>'}
};
function moltBrandColor(c) { var b = MOLT_BRANDS[c]; return b ? b.c : '#94a3b8'; }
function moltBrandTile(company) {
  var b = MOLT_BRANDS[company];
  if (!b) {
    var ch = (company || '?').charAt(0).toUpperCase();
    return '<span class="brandtile" style="background:rgba(148,163,184,.16);color:#94a3b8;' +
           'font-weight:700;font-size:13px">' + esc(ch) + '</span>';
  }
  return '<span class="brandtile" style="background:' + b.t + '">' +
         '<svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true">' + b.g + '</svg></span>';
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
        '<div class="card-head">' + moltBrandTile(m.company) +
          '<div class="chead-txt">' +
            '<div class="mco" style="color:' + moltBrandColor(m.company) + '">' +
              esc(m.company || 'Local') + '</div>' +
            '<div class="name">' + esc(m.display_name) + '</div>' +
          '</div>' +
        '</div>' +
        '<div class="meta">' + (m.quantization ? esc(m.quantization) + ' \u00b7 ' : '') + sizeStr + '</div>' +
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
    if (r.success) {
      setStatus('ready', 'Model Ready');
      refreshModelList();
      // Refresh chip with new active model
      sendWithPromise('getModelStatus').then(function(rr) {
        allModels = rr.models || [];
        refreshModelChip();
      });
    }
    else setStatus('error', 'Load failed');
  });
}

function deleteModel(modelId) {
  sendWithPromise('deleteModel', modelId).then(function(r) {
    if (r.success) refreshModelList();
  });
}

// ---- Model Chip (always-visible top selector) ----
var allModels = [];
var activeModelId = null;
var pickedModelId = null;
var downloadingModelId = null;

// The currently-selected cloud model dict, or null. A cloud model is active
// purely by selection (is_loaded is always false for cloud).
function selectedCloudModel() {
  if (!activeModelId) return null;
  return allModels.find(function(m){
    return m.is_cloud && m.model_id === activeModelId;
  }) || null;
}

// Select a cloud "provider:model" model. Nothing to download or load — the
// backend short-circuits loadModel for cloud ids to instant-ready.
function selectCloudModel(modelId) {
  activeModelId = modelId;
  pickedModelId = modelId;
  setStatus('ready', 'Model Ready');
  sendWithPromise('loadModel', modelId).then(function(){}, function(){});
  refreshModelChip();
  toggleModelDropdown();
}

function refreshModelChip() {
  var chip = document.getElementById('modelChip');
  var nameEl = document.getElementById('modelChipName');
  if (!chip || !nameEl) return;
  var active = allModels.find(function(m){ return m.is_loaded; });
  if (active) {
    activeModelId = active.model_id;
    nameEl.textContent = active.display_name || active.model_id;
    return;
  }
  var cloud = selectedCloudModel();
  if (cloud) {
    nameEl.textContent = cloud.display_name || cloud.model_id;
    return;
  }
  var firstDownloaded = allModels.find(function(m){ return m.is_downloaded; });
  if (firstDownloaded) {
    nameEl.textContent = (firstDownloaded.display_name || firstDownloaded.model_id) + ' (click to load)';
  } else {
    nameEl.textContent = 'Choose Model';
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
  if (willOpen) {
    renderModelChipDropdown();
  }
}

function renderModelChipDropdown() {
  var dd = document.getElementById('modelChipDropdown');
  if (!dd) return;
  if (allModels.length === 0) {
    dd.innerHTML = '<div style="padding:14px;color:#666;font-size:12px;text-align:center">Loading models...</div>';
    sendWithPromise('getModelStatus').then(function(r) {
      allModels = r.models || [];
      renderModelChipDropdown();
      refreshModelChip();
    });
    return;
  }
  dd.innerHTML = '';
  // Partition into local (on-device GGUF) and cloud (via the user's key)
  // models. Cloud models carry is_cloud=true + provider from the backend.
  var localModels = [], cloudModels = [];
  allModels.forEach(function(m){ (m.is_cloud ? cloudModels : localModels).push(m); });

  // ---- Cloud connect: the FIRST, most-prominent option so bring-your-own-
  // key frontier models are the top action in the picker. ----
  var cloudCta = document.createElement('div');
  cloudCta.className = 'mcd-cloud-cta';
  cloudCta.innerHTML =
    '<span class="mcc-icon">☁️</span>' +
    '<span class="mcc-text">' +
      (cloudModels.length ? 'Manage cloud providers' : 'Connect a cloud model') +
      '<span class="mcc-sub">OpenAI, Claude, Gemini &amp; more — your key</span>' +
    '</span>' +
    '<span class="mcc-arrow">→</span>';
  cloudCta.onclick = function() {
    toggleModelDropdown();
    window.open('molt://ai-settings/?section=providers', '_blank');
  };
  dd.appendChild(cloudCta);

  if (cloudModels.length && localModels.length) {
    var lh = document.createElement('div');
    lh.className = 'mcd-header';
    lh.textContent = 'Local · Private 🔒';
    dd.appendChild(lh);
  }

  localModels.forEach(function(m) {
    var item = document.createElement('div');
    item.className = 'model-chip-item';
    var isActive = !!m.is_loaded;
    if (isActive) item.className += ' active';
    var statusClass, statusText;
    if (isActive) { statusClass = 'active'; statusText = 'Active'; }
    else if (downloadingModelId === m.model_id) { statusClass = 'downloading'; statusText = 'Downloading'; }
    else if (m.is_downloaded) { statusClass = 'downloaded'; statusText = 'Ready'; }
    else { statusClass = 'available'; statusText = 'Download'; }
    var sizeMB = m.file_size_mb || 0;
    var sizeStr2 = sizeMB > 1024 ? (sizeMB / 1024).toFixed(1) + ' GB' : sizeMB + ' MB';
    item.innerHTML =
      moltBrandTile(m.company) +
      '<div style="flex:1;min-width:0">' +
        '<div class="mco" style="color:' + moltBrandColor(m.company) + '">' +
          esc(m.company || 'Local') + '</div>' +
        '<div class="mname">' + esc(m.display_name || m.model_id) + '</div>' +
        '<div class="msize">' + (m.quantization ? esc(m.quantization) + ' · ' : '') + sizeStr2 + '</div>' +
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

  // ---- Cloud group: instant-select rows (no download/load) ----
  if (cloudModels.length) {
    var ch = document.createElement('div');
    ch.className = 'mcd-header';
    ch.textContent = 'Cloud · via your key ☁️';
    dd.appendChild(ch);
    cloudModels.forEach(function(m) {
      var item = document.createElement('div');
      item.className = 'model-chip-item';
      var isActive = activeModelId === m.model_id;
      if (isActive) item.className += ' active';
      item.innerHTML =
        moltBrandTile(m.provider) +
        '<div style="flex:1;min-width:0">' +
          '<div class="mco" style="color:' + moltBrandColor(m.provider) + '">' +
            esc(m.provider || 'Cloud') + '</div>' +
          '<div class="mname">' + esc(m.display_name || m.model_id) + '</div>' +
          '<div class="msize">cloud · via your key</div>' +
        '</div>' +
        '<span class="mstatus ' + (isActive ? 'active' : 'downloaded') + '">' +
          (isActive ? 'Active' : 'Use') + '</span>';
      item.onclick = function() {
        if (isActive) { toggleModelDropdown(); return; }
        selectCloudModel(m.model_id);
      };
      dd.appendChild(item);
    });
  }

  // (Cloud connect/manage lives at the TOP of the dropdown now — the
  // .mcd-cloud-cta entry above — so it's the first option the user sees.)
}

function updateModelChipProgress(percent) {
  var fg = document.getElementById('modelChipProgressFg');
  var pct = document.getElementById('modelChipPct');
  if (fg) {
    var circumference = 2 * Math.PI * 8; // r=8
    var offset = circumference - (percent / 100) * circumference;
    fg.style.strokeDashoffset = offset;
    fg.style.strokeDasharray = circumference;
  }
  if (pct) pct.textContent = Math.round(percent) + '%';
}

// Close dropdown when clicking outside
document.addEventListener('click', function(e) {
  var wrap = document.querySelector('.model-chip-wrap');
  if (wrap && !wrap.contains(e.target)) {
    var dd = document.getElementById('modelChipDropdown');
    var chip = document.getElementById('modelChip');
    if (dd) dd.classList.remove('open');
    if (chip) chip.classList.remove('open');
  }
});

// Initialize chip on page load
sendWithPromise('getModelStatus').then(function(r) {
  allModels = r.models || [];
  refreshModelChip();
});

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
  // Update chip if this is the model we're downloading
  if (downloadingModelId === modelId && total > 0) {
    var pct = (current / total) * 100;
    updateModelChipProgress(pct);
    var nameEl = document.getElementById('modelChipName');
    var modelInfo = allModels.find(function(m){ return m.model_id === modelId; });
    if (nameEl && modelInfo) {
      nameEl.textContent = 'Downloading ' + (modelInfo.display_name || modelId) + '\u2026';
    }
  }
});

cr.addWebUiListener('download-complete', function(modelId, success) {
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.remove('active');
  // Reset chip download state
  if (downloadingModelId === modelId) {
    downloadingModelId = null;
    var chip = document.getElementById('modelChip');
    if (chip) chip.classList.remove('downloading');
    if (success) {
      // Refresh model list and load it as active
      sendWithPromise('getModelStatus').then(function(r) {
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

// ---- Copy Code / Response ----

function copyCode(id, btn) {
  var pre = document.getElementById(id);
  if (!pre) return;
  navigator.clipboard.writeText(pre.textContent).then(function() {
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
    var text = el.textContent.toLowerCase();
    if (text.indexOf(query) >= 0) {
      count++;
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
          var m = document.getElementById('messages');
          m.innerHTML = '';
          for (var i = 0; i < data.messages.length; i++) {
            var msg = data.messages[i];
            if (msg.role === 'user') {
              addUserMsg(msg.content);
            } else {
              var d = document.createElement('div');
              d.className = 'message ai';
              d.innerHTML = '<div class="label">MoltBrowser AI</div><div class="model-badge">Local LLM</div><div class="text">' + renderMarkdown(msg.content) + '</div>' +
                '<div class="msg-actions"><button class="msg-action" onclick="copyResponse(this)">Copy response</button></div>';
              m.appendChild(d);
            }
          }
          m.scrollTop = m.scrollHeight;
          updateContextInfo();
          addSysMsg('Imported ' + data.messages.length + ' messages from ' + file.name);
        } else {
          addErr('Invalid chat export file');
        }
      } catch (err) {
        addErr('Failed to parse file: ' + err.message);
      }
    };
    reader.readAsText(file);
  };
  input.click();
}

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
