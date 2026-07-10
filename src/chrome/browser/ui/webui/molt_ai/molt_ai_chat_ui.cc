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
/* Default theme = "Gray" — a true dark-GRAY palette, visibly distinct
   from Black. The other two themes override the vars via data-theme
   on <html>:
     black -> data-theme="black" (pure-black surfaces)
     white -> data-theme="light" (pure-white surfaces)
   Gray is the absence of the attribute so old sessions keep working;
   picking Gray in the menu removes the attribute, which fully restores
   every base var below (Black overrides the same set, so the two
   round-trip cleanly). */
:root{
  /* Rich dark-violet frosted ground (a subtle violet/near-black
     gradient painted on <html>), matching the mockup's --ground feel
     but with depth so panels never read as flat black. */
  --bg:#0a0912;
  --bg-grad-a:#12102a;
  --bg-grad-b:#0a0912;
  --bg-grad-c:#0d0a18;
  --surface:#1b1a2a;
  --surface2:#25243a;
  --border:rgba(255,255,255,0.14);
  --text:#f4f5fa;
  --muted:rgba(233,236,247,0.62);
  --faint:rgba(233,236,247,0.40);
  --accent:#ff5257;        /* MoltBrowser brand red */
  --accent-hover:#ff676c;
  --ok:#5fe3a1;
  --warn:#e0b454;
  --err:#f07070;
  /* ---- Liquid Glass tokens (dark themes) ----
     Floating panels read as frosted glass over the ambient orbs.
     Light theme overrides these below with opaque-leaning values.
     Tuned to the mockup's --glass / --glass-strong / --edge / --specular. */
  --glass-bg:rgba(20,22,32,0.42);
  --glass-bg-strong:rgba(24,26,38,0.62);
  --glass-border:rgba(255,255,255,0.14);
  --glass-border-hover:rgba(255,255,255,0.30);
  --edge-soft:rgba(255,255,255,0.07);
  --specular:rgba(255,255,255,0.28);
  --glass-blur:blur(34px) saturate(1.75);
  --glass-blur-lg:blur(52px) saturate(1.8);
  --glass-shadow:0 24px 60px -18px rgba(0,0,0,0.70),inset 0 1px 0 var(--specular);
  --glass-shadow-sm:0 14px 34px -12px rgba(0,0,0,0.6),inset 0 1px 0 rgba(255,255,255,0.24);
  --violet:#a78bfa;
  --violet-2:#8ea2ff;
  --brand-soft:rgba(255,82,87,0.16);
  --mono:"SF Mono",ui-monospace,"JetBrains Mono",Menlo,monospace;
  --ambient-opacity:0.85;
}
/* Black theme: pure black with slightly lifted surfaces. Overrides
   every var Gray defines differently, so switching Black->Gray (attr
   removal) restores the full gray palette and vice versa. */
:root[data-theme="black"]{
  --bg:#000000;
  --bg-grad-a:#0a0714;
  --bg-grad-b:#000000;
  --bg-grad-c:#050409;
  --surface:#111018;
  --surface2:#1b1a26;
  --border:rgba(255,255,255,0.13);
  --text:#f4f5fa;
  --muted:rgba(233,236,247,0.58);
  --faint:rgba(233,236,247,0.36);
  /* Pure-black ground: keep glass frosted-dark and let the orbs
     glow so panels never read as flat black rectangles. */
  --glass-bg:rgba(18,20,30,0.46);
  --glass-bg-strong:rgba(22,24,36,0.64);
  --glass-border:rgba(255,255,255,0.13);
  --glass-border-hover:rgba(255,255,255,0.28);
  --specular:rgba(255,255,255,0.24);
  --ambient-opacity:0.72;
}
/* White theme: pure white surfaces; accent stays Molt red (darkened
   for contrast on white). Everything keyed to the vars flips. */
:root[data-theme="light"]{
  --bg:#ffffff;
  --bg-grad-a:#ffffff;
  --bg-grad-b:#ffffff;
  --bg-grad-c:#ffffff;
  --surface:#f6f6f6;
  --surface2:#ececec;
  --border:rgba(0,0,0,0.10);
  --text:#111111;
  --muted:#5a5a63;
  --faint:#8a8a93;
  --accent:#d63638;
  --accent-hover:#b32d2e;
  --ok:#1a8a45;
  --warn:#9a6700;
  --err:#c62828;
  /* Light theme: clean, no ambient orbs. Glass = frosted white so
     panels still float and refract, but surfaces stay bright and text
     keeps AA contrast. Ambient opacity 0 disables the orb layer. */
  --glass-bg:rgba(255,255,255,0.65);
  --glass-bg-strong:rgba(255,255,255,0.80);
  --glass-border:rgba(0,0,0,0.08);
  --glass-border-hover:rgba(0,0,0,0.16);
  --edge-soft:rgba(0,0,0,0.06);
  --specular:rgba(255,255,255,0.9);
  --glass-blur:blur(24px) saturate(1.5);
  --glass-blur-lg:blur(30px) saturate(1.6);
  --glass-shadow:0 18px 44px -20px rgba(0,0,0,0.28),inset 0 1px 0 rgba(255,255,255,0.9);
  --glass-shadow-sm:0 10px 26px -16px rgba(0,0,0,0.22),inset 0 1px 0 rgba(255,255,255,0.85);
  --violet:#7c5cff;
  --violet-2:#6d7fff;
  --brand-soft:rgba(214,54,56,0.12);
  --ambient-opacity:0;
}
*{margin:0;padding:0;box-sizing:border-box}
/* Ground color lives on <html> so the fixed ambient layer (z-index:-1)
   can paint above it but behind every body child, whether or not that
   child is positioned. Body itself is transparent. */
/* Rich dark-violet frosted ground: a subtle violet/near-black
   gradient (not flat black) so the frosted panels have colored depth
   to refract. Light theme collapses all three stops to white. */
html{background:
  radial-gradient(120% 90% at 78% 0%,var(--bg-grad-a),transparent 60%),
  radial-gradient(120% 90% at 12% 100%,var(--bg-grad-c),transparent 62%),
  var(--bg-grad-b);
  background-color:var(--bg)}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:transparent;color:var(--text);height:100vh;display:flex;flex-direction:column;position:relative;overflow:hidden}
button{font-family:inherit}
/* ---- Liquid Glass ambient layer ----
   Heavily-blurred low-opacity orbs give the frosted panels something to
   refract. Fixed + pointer-events:none + z-index:-1 so it never touches
   layout or input. Disabled on the light theme via --ambient-opacity:0. */
.lg-ambient{position:fixed;inset:-10%;z-index:-1;pointer-events:none;overflow:hidden;opacity:var(--ambient-opacity);transition:opacity 0.3s}
.lg-ambient::before,.lg-ambient::after,.lg-ambient .lg-orb{content:'';position:absolute;border-radius:50%;filter:blur(80px);will-change:transform}
/* Saturated, violet-dominant environment matching the mockup's orbs:
   a deep violet mass, a molt-red mass, and a teal mass. These are the
   ONLY colored depth behind the frosted glass, so they lead violet
   with red + teal accents and sit at high opacity (see --ambient-opacity).
   The teal .lg-orb DOM node gets a second violet glow via box-shadow to
   fill the mockup's fourth orb without adding elements. */
.lg-ambient::before{width:60%;height:60%;top:-14%;left:-10%;background:radial-gradient(circle at 32% 32%,#4a2f8f 0%,#2a1a63 42%,transparent 72%);animation:lg-drift-a 34s ease-in-out infinite alternate}
.lg-ambient::after{width:54%;height:54%;bottom:-14%;right:-10%;background:radial-gradient(circle at 60% 40%,#c0303f 0%,#7a2036 44%,transparent 72%);animation:lg-drift-b 40s ease-in-out infinite alternate}
.lg-ambient .lg-orb{width:50%;height:50%;top:34%;left:30%;background:radial-gradient(circle at 44% 56%,#159aad 0%,#0f5f6e 46%,transparent 72%);box-shadow:0 0 220px 60px rgba(91,58,134,0.55);animation:lg-drift-c 46s ease-in-out infinite alternate}
@keyframes lg-drift-a{from{transform:translate3d(0,0,0)}to{transform:translate3d(9%,7%,0)}}
@keyframes lg-drift-b{from{transform:translate3d(0,0,0)}to{transform:translate3d(-8%,-6%,0)}}
@keyframes lg-drift-c{from{transform:translate3d(0,0,0) scale(1)}to{transform:translate3d(5%,-8%,0) scale(1.08)}}
/* Respect reduced-motion: freeze the ambient drift entirely. */
@media (prefers-reduced-motion: reduce){
  .lg-ambient::before,.lg-ambient::after,.lg-ambient .lg-orb{animation:none}
}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
@keyframes spin{to{transform:rotate(360deg)}}
/* ---- Header ---- */
.header{flex:0 0 44px;height:44px;padding:0 10px;border-bottom:1px solid var(--edge-soft);background:var(--glass-bg);-webkit-backdrop-filter:var(--glass-blur);backdrop-filter:var(--glass-blur);box-shadow:inset 0 1px 0 var(--specular);display:flex;align-items:center;gap:8px;position:relative;z-index:5}
/* Conic red->violet->blue avatar tile before the title, matching the
   mockup's .p-head .av. Rendered as a pseudo-element so no HTML changes. */
.header .title{font-size:13.5px;font-weight:650;color:var(--text);white-space:nowrap;display:flex;align-items:center;gap:9px}
.header .title::before{content:'';width:22px;height:22px;border-radius:8px;flex:0 0 auto;background:conic-gradient(from 200deg,#ff5257,#a78bfa,#8ea2ff,#ff5257);box-shadow:0 0 12px rgba(167,139,250,0.5),inset 0 1px 0 rgba(255,255,255,0.35)}
.header .status{font-size:10.5px;color:var(--faint);display:flex;align-items:center;gap:5px;transition:color 0.15s;min-width:0;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.header .status::before{content:'';width:6px;height:6px;border-radius:50%;flex:0 0 auto;background:var(--faint);transition:background 0.15s}
.header .status.ready{color:var(--muted)}
.header .status.ready::before{background:var(--ok);box-shadow:0 0 6px var(--ok)}
.header .status.loading{color:var(--warn)}
.header .status.loading::before{background:var(--warn);animation:pulse 1s infinite}
.header .status.error{color:var(--err)}
.header .status.error::before{background:var(--err)}
.header .status.offline{color:var(--faint)}
.header .status.offline::before{background:var(--faint)}
.header-actions{margin-left:auto;display:flex;gap:2px;align-items:center}
.icon-btn{background:none;border:none;border-radius:8px;color:var(--muted);width:28px;height:28px;display:flex;align-items:center;justify-content:center;font-size:14px;cursor:pointer;transition:background 0.15s,color 0.15s}
.icon-btn:hover{background:var(--surface2);color:var(--text)}
.icon-btn.labeled{width:auto;padding:0 10px;font-size:11px;border:1px solid var(--border)}
/* ---- Header overflow menu ---- */
.overflow-wrap{position:relative}
.overflow-menu{position:absolute;top:calc(100% + 6px);right:0;min-width:212px;background:var(--glass-bg-strong);-webkit-backdrop-filter:var(--glass-blur);backdrop-filter:var(--glass-blur);border:1px solid var(--glass-border);border-radius:16px;padding:5px;box-shadow:var(--glass-shadow);display:none;z-index:60}
.overflow-menu.open{display:block}
.om-item{display:flex;align-items:center;gap:8px;padding:8px 10px;border-radius:8px;font-size:12px;color:var(--text);cursor:pointer;transition:background 0.15s;white-space:nowrap}
.om-item:hover{background:var(--surface2)}
.om-sep{height:1px;background:var(--border);margin:5px 6px}
.om-label{padding:6px 10px 2px;font-size:10px;font-weight:600;letter-spacing:0.5px;text-transform:uppercase;color:var(--faint)}
.om-check{margin-left:auto;color:var(--accent);font-weight:700;opacity:0;transition:opacity 0.15s}
.om-check.visible{opacity:1}
.om-meta{margin-top:5px;border-top:1px solid var(--border);padding:7px 10px 4px;font-size:10px;color:var(--faint);display:flex;gap:10px;flex-wrap:wrap}
/* ---- Recents drawer ---- */
.drawer-scrim{position:absolute;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.45);z-index:29;opacity:0;pointer-events:none;transition:opacity 0.15s}
.drawer-scrim.open{opacity:1;pointer-events:auto}
.drawer{position:absolute;top:0;bottom:0;left:0;width:240px;background:var(--glass-bg-strong);-webkit-backdrop-filter:var(--glass-blur);backdrop-filter:var(--glass-blur);border-right:1px solid var(--glass-border);z-index:30;display:flex;flex-direction:column;padding:10px;transform:translateX(-100%);transition:transform 0.18s ease;box-shadow:6px 0 40px -8px rgba(0,0,0,0.5),inset -1px 0 0 rgba(255,255,255,0.08)}
.drawer.open{transform:translateX(0)}
.drawer-new{display:flex;align-items:center;gap:6px;padding:9px 10px;border-radius:10px;border:1px solid var(--border);background:var(--surface2);color:var(--text);font-size:12.5px;font-weight:500;cursor:pointer;text-align:left;transition:border-color 0.15s}
.drawer-new:hover{border-color:var(--faint)}
.drawer-search input{width:100%;margin-top:8px;padding:7px 10px;border-radius:10px;border:1px solid var(--border);background:var(--bg);color:var(--text);font-size:12px;outline:none;transition:border-color 0.15s}
.drawer-search input::placeholder{color:var(--faint)}
.drawer-search input:focus{border-color:var(--faint)}
.drawer-label{padding:14px 6px 6px;font-size:10px;font-weight:600;letter-spacing:0.6px;text-transform:uppercase;color:var(--faint)}
.conv-list{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:2px}
.conv-item{display:flex;align-items:center;gap:6px;padding:8px;border-radius:9px;font-size:12px;color:var(--muted);cursor:pointer;transition:background 0.15s,color 0.15s}
.conv-item:hover,.conv-item.active{background:var(--surface2);color:var(--text)}
.conv-title{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.conv-del{flex:0 0 auto;background:none;border:none;color:var(--faint);font-size:12px;cursor:pointer;opacity:0;padding:2px 5px;border-radius:6px;transition:opacity 0.15s,color 0.15s}
.conv-item:hover .conv-del{opacity:1}
.conv-del:hover{color:var(--err)}
.conv-empty{padding:14px 8px;font-size:11px;color:var(--faint);text-align:center}
/* ---- Search bar (Cmd+F) ---- */
.search-bar{padding:6px 16px;border-bottom:1px solid var(--glass-border);background:var(--glass-bg);-webkit-backdrop-filter:var(--glass-blur);backdrop-filter:var(--glass-blur);box-shadow:inset 0 1px 0 rgba(255,255,255,0.12);display:none;gap:6px;align-items:center;position:relative;z-index:4}
.search-bar.open{display:flex}
.search-bar input{flex:1;padding:6px 10px;border-radius:8px;border:1px solid var(--border);background:var(--surface);color:var(--text);font-size:12px;outline:none}
.search-bar input:focus{border-color:var(--faint)}
.search-bar .search-count{font-size:10px;color:var(--faint);padding:6px 4px;white-space:nowrap}
.search-bar .search-close{background:none;border:none;color:var(--muted);cursor:pointer;font-size:14px;padding:4px}
.search-bar .search-close:hover{color:var(--text)}
.highlight{background:#e8c25a;color:#111;border-radius:2px;padding:0 2px}
/* ---- Tab context strip + anonymous banner ---- */
.tab-context{display:flex;align-items:center;gap:8px;padding:6px 16px;font-size:11px;color:var(--faint);background:var(--glass-bg);-webkit-backdrop-filter:var(--glass-blur);backdrop-filter:var(--glass-blur);border-bottom:1px solid var(--glass-border);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;position:relative;z-index:3}
.tab-context-icon{flex-shrink:0}
.tab-context-label{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:default}
.anon-banner{display:flex;align-items:center;gap:8px;background:var(--surface2);color:var(--muted);padding:6px 16px;font-size:11px;font-weight:500;border-bottom:1px solid var(--border)}
.anon-icon{flex-shrink:0}
/* ---- Agent inbox tray ---- */
.agent-inbox{background:var(--surface);border-bottom:1px solid var(--border);padding:6px 12px;font-size:11px;display:flex;flex-direction:column;gap:4px}
.agent-inbox-header{font-weight:600;color:var(--muted);font-size:10px;text-transform:uppercase;letter-spacing:0.5px}
.agent-row{display:flex;align-items:center;gap:8px;padding:4px 6px;background:var(--surface2);border-radius:6px}
.agent-spinner{width:8px;height:8px;border-radius:50%;background:var(--accent);animation:agent-pulse 1.5s ease-in-out infinite;flex-shrink:0}
.agent-spinner.done-ok{background:var(--ok);animation:none}
.agent-spinner.done-err{background:var(--err);animation:none}
@keyframes agent-pulse{0%,100%{opacity:0.4}50%{opacity:1}}
.agent-name{font-weight:500;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:0 0 auto;max-width:40%}
.agent-progress{color:var(--muted);flex:0 0 auto;font-variant-numeric:tabular-nums}
.agent-note{color:var(--muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1 1 auto;font-style:italic}
/* ---- Messages ---- */
.messages{flex:1;overflow-y:auto;padding:18px 16px 8px}
.message{max-width:72ch;margin:0 auto 14px;font-size:12.5px;line-height:1.55}
.message .sender{display:none}
.message .text{white-space:pre-wrap;word-wrap:break-word}
/* Assistant bubble: translucent white glass, left-aligned, with a
   small bottom-left radius, per the mockup's .msg.a. No backdrop-filter
   here — messages scroll and the perf rule keeps GPU blur off list
   rows; the luminous border + inset highlight still read as glass. */
.message.ai{display:flex;justify-content:flex-start}
.message.ai .text{background:rgba(255,255,255,0.07);border:1px solid rgba(255,255,255,0.12);border-radius:15px;border-bottom-left-radius:5px;padding:10px 13px;max-width:86%;box-shadow:inset 0 1px 0 rgba(255,255,255,0.10)}
.message.ai .text b{color:#c9b8ff}
/* Light theme: frosted-white assistant bubble so it stays legible on a
   bright ground (the 7%-white glass would vanish on white). */
:root[data-theme="light"] .message.ai .text{background:#f4f4f7;border-color:var(--border);box-shadow:inset 0 1px 0 rgba(255,255,255,0.9)}
:root[data-theme="light"] .message.ai .text b{color:#6d4bd0}
.message.user{display:flex;justify-content:flex-end;align-items:center;gap:4px}
/* User bubble: molt-red gradient, right-aligned, white text, small
   bottom-right radius + soft red glow, per the mockup's .msg.u. No
   backdrop-filter (scrolling row, per the perf rule). */
.message.user .text{background:linear-gradient(180deg,rgba(255,82,87,0.9),rgba(224,53,59,0.85));border:1px solid rgba(255,82,87,0.5);border-radius:15px;border-bottom-right-radius:5px;padding:9px 13px;max-width:85%;color:#fff;box-shadow:0 8px 20px -8px rgba(255,82,87,0.5),inset 0 1px 0 rgba(255,255,255,0.2)}
/* ---- Inline edit of user messages (Claude-style) ---- */
.msg-edit{background:none;border:none;color:var(--faint);font-size:12px;cursor:pointer;opacity:0;padding:2px 6px;border-radius:6px;transition:opacity 0.15s,color 0.15s,background 0.15s;flex:0 0 auto}
.message.user:hover .msg-edit{opacity:1}
.msg-edit:hover{color:var(--text);background:var(--surface2)}
.msg-edit-area{display:flex;flex-direction:column;gap:6px;width:100%;max-width:85%}
.msg-edit-area textarea{width:100%;background:var(--surface2);border:1px solid var(--border);border-radius:12px;padding:9px 13px;color:var(--text);font-size:13px;line-height:1.5;font-family:inherit;resize:vertical;min-height:60px;outline:none}
.msg-edit-area textarea:focus{border-color:var(--faint)}
.msg-edit-btns{display:flex;gap:6px;justify-content:flex-end}
.msg-edit-btns button{padding:4px 12px;border-radius:8px;font-size:11px;cursor:pointer;border:1px solid var(--border);background:transparent;color:var(--muted)}
.msg-edit-btns .save{background:var(--accent);border-color:var(--accent);color:#fff}
.msg-edit-btns .save:hover{background:var(--accent-hover)}
.msg-edit-btns .cancel-edit:hover{color:var(--text);border-color:var(--faint)}
.attached-note{font-size:10px;color:var(--muted);margin-top:4px;font-style:italic}
.welcome-hint{color:var(--muted);font-size:11px;margin-top:6px}
.message.system{text-align:center}
.message.system .text{display:inline-block;font-size:11px;color:var(--ok);opacity:0.85}
.message.error .text{border-left:3px solid var(--err);background:var(--surface);border-radius:8px;padding:8px 12px;color:var(--err);font-size:12px}
.message .text .cursor{display:inline-block;width:2px;height:14px;background:var(--accent);animation:blink 0.8s infinite;vertical-align:text-bottom;margin-left:1px}
a.chat-link{color:var(--accent);text-decoration:underline;cursor:pointer;word-break:break-all}
/* ---- Code copy + per-message hover actions ---- */
.code-wrap{position:relative;margin:8px 0}
.code-wrap pre{margin:0}
.code-copy{position:absolute;top:4px;right:4px;padding:2px 8px;border-radius:5px;border:1px solid var(--border);background:var(--surface);color:var(--muted);font-size:10px;cursor:pointer;opacity:0;transition:opacity 0.15s}
.code-wrap:hover .code-copy{opacity:1}
.code-copy:hover{color:var(--text);border-color:var(--faint)}
.code-copy.copied{color:var(--ok);border-color:var(--ok)}
.msg-actions{display:flex;gap:4px;margin-top:6px;opacity:0;transition:opacity 0.15s}
.message:hover .msg-actions{opacity:1}
.msg-action{padding:2px 8px;border-radius:5px;border:1px solid var(--border);background:none;color:var(--faint);font-size:10px;cursor:pointer}
.msg-action:hover{color:var(--text);border-color:var(--faint)}
/* ---- Inline agent action chips ---- */
.molt-action-summary{margin:8px auto 0;max-width:72ch;padding:6px 10px;background:var(--surface2);color:var(--muted);font-size:12px;border-radius:8px;border:1px solid var(--border)}
.molt-action-summary span{margin-right:6px}
.molt-action-result{margin:4px auto;max-width:72ch;padding:4px 10px;border-radius:8px;font-size:11px;background:var(--surface);border:1px solid var(--border)}
.molt-action-result.ok{color:var(--ok)}
.molt-action-result.fail{color:var(--err)}
.molt-action-result .detail{opacity:0.7;font-size:10px;margin-left:6px}
.molt-action-confirm{margin:6px auto;max-width:72ch;padding:8px 10px;border-radius:10px;background:var(--surface);color:var(--text);border:1px solid var(--border);border-left:3px solid var(--accent);font-size:11px;display:flex;align-items:center;gap:10px}
.molt-action-confirm .ac-icon{font-size:16px;flex-shrink:0}
.molt-action-confirm .ac-body{flex:1;min-width:0}
.molt-action-confirm .ac-title{font-weight:600;font-size:10px;text-transform:uppercase;letter-spacing:0.5px;color:var(--muted);margin-bottom:2px}
.molt-action-confirm .ac-label{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-family:ui-monospace,Menlo,monospace}
.molt-action-confirm .ac-buttons{display:flex;gap:6px;flex-shrink:0}
.molt-action-confirm button{padding:4px 10px;border-radius:7px;border:1px solid var(--border);background:transparent;color:var(--text);font-size:11px;cursor:pointer;font-family:inherit}
.molt-action-confirm .ac-allow{background:var(--accent);border-color:var(--accent);color:#fff}
.molt-action-confirm .ac-allow:hover{background:var(--accent-hover)}
.molt-action-confirm .ac-deny:hover{border-color:var(--err);color:var(--err)}
/* ---- Real-time navigate chip ---- */
.molt-navigating-chip{display:flex;align-items:center;gap:6px;padding:6px 10px;border-radius:8px;background:var(--surface);border:1px solid var(--border);color:var(--muted);font-size:11px;margin:4px auto;max-width:72ch;overflow:hidden}
.molt-navigating-chip .nav-icon{flex-shrink:0;font-size:14px}
.molt-navigating-chip .nav-label{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.molt-navigating-chip .nav-spinner{width:10px;height:10px;border:2px solid var(--border);border-top-color:var(--muted);border-radius:50%;animation:spin 0.8s linear infinite;flex-shrink:0}
/* ---- Model-needs-selection banner ---- */
.model-select-banner{display:flex;align-items:center;gap:6px;padding:7px 10px;border-radius:10px;background:var(--surface);border:1px solid var(--warn);color:var(--warn);font-size:11px;margin:0 auto 10px;max-width:72ch;animation:chip-pulse 1.4s ease-in-out infinite}
.model-select-banner .msb-icon{flex-shrink:0;font-size:13px}
@keyframes chip-pulse{0%,100%{box-shadow:0 0 0 0 rgba(224,180,84,0.35)}60%{box-shadow:0 0 0 5px rgba(224,180,84,0)}}
/* ---- Quick action chips (above composer) ---- */
.actions{padding:0 16px 8px;display:flex;gap:6px;flex-wrap:wrap;width:100%;max-width:calc(72ch + 32px);margin:0 auto}
.actions button{padding:5px 11px;border-radius:999px;border:1px solid var(--border);background:transparent;color:var(--muted);font-size:11.5px;cursor:pointer;transition:border-color 0.15s,color 0.15s,background 0.15s}
.actions button:hover{border-color:var(--faint);color:var(--text);background:var(--surface)}
.actions button:disabled{opacity:0.4;cursor:not-allowed}
/* ---- Attachment chip (above the composer card) ---- */
.attach-chip-wrap{width:100%;max-width:calc(72ch + 32px);margin:0 auto 6px;display:flex}
.attach-chip{display:flex;align-items:center;gap:8px;padding:6px 10px;border-radius:10px;background:var(--surface);border:1px solid var(--border);font-size:11px;color:var(--muted);max-width:100%;min-width:0}
.attach-chip .a-name{color:var(--text);font-weight:500;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.attach-chip .a-size{color:var(--faint);flex:0 0 auto}
.attach-chip .a-status{flex:0 0 auto;white-space:nowrap}
.attach-chip .a-status.ok{color:var(--ok)}
.attach-chip .a-status.err{color:var(--err)}
.attach-chip .a-spin{width:10px;height:10px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin 0.8s linear infinite;flex:0 0 auto}
.attach-chip .a-close{background:none;border:none;color:var(--faint);cursor:pointer;font-size:12px;padding:0 2px;flex:0 0 auto}
.attach-chip .a-close:hover{color:var(--err)}
/* ---- Composer card ---- */
.composer{flex:0 0 auto;padding:4px 16px 14px}
.composer-card{width:100%;max-width:calc(72ch + 32px);margin:0 auto;background:var(--glass-bg);-webkit-backdrop-filter:var(--glass-blur);backdrop-filter:var(--glass-blur);border:1px solid var(--glass-border);border-radius:18px;padding:11px 12px 9px;display:flex;flex-direction:column;gap:8px;box-shadow:var(--glass-shadow-sm),inset 0 1px 0 var(--specular);transition:border-color 0.15s,box-shadow 0.15s,background 0.15s}
.composer-card:focus-within{border-color:var(--glass-border-hover);background:var(--glass-bg-strong);box-shadow:var(--glass-shadow-sm),inset 0 1px 0 var(--specular),0 0 0 3px var(--brand-soft)}
.composer-card textarea{width:100%;background:transparent;border:none;outline:none;resize:none;color:var(--text);font-size:13px;line-height:1.5;font-family:inherit;max-height:120px;overflow-y:auto;padding:2px 2px 0}
.composer-card textarea::placeholder{color:var(--faint)}
.composer-card textarea:disabled{opacity:0.5}
.composer-row{display:flex;align-items:center;gap:6px;min-width:0}
.composer-spacer{flex:1}
.browse-btn{padding:5px 11px;border-radius:999px;border:1px solid var(--edge-soft);background:rgba(255,255,255,0.06);color:var(--muted);font-size:11.5px;font-weight:500;cursor:pointer;transition:border-color 0.15s,color 0.15s,background 0.15s;flex:0 0 auto}
.browse-btn:hover{border-color:var(--glass-border);color:var(--text);background:rgba(255,255,255,0.12)}
/* ---- Transient agent hint (empty-input nudge above the composer) ---- */
.agent-hint{width:100%;max-width:calc(72ch + 32px);margin:0 auto 6px;font-size:11px;color:var(--muted);padding:0 4px}
/* ---- Live dictation status (mic): subtle listening pulse ---- */
.dictation-hint{display:none;width:100%;max-width:calc(72ch + 32px);margin:0 auto 6px;font-size:11px;color:var(--muted);padding:0 4px;align-items:center;gap:6px}
.dictation-hint.on{display:flex}
.dictation-dot{width:7px;height:7px;border-radius:50%;background:var(--err);animation:pulse 1.2s ease-in-out infinite;flex:0 0 auto}
.dictation-hint.transcribing .dictation-dot{background:var(--warn)}
/* ---- Agent-action mode chip (Ask first / Auto) + upward menu ---- */
.mode-chip-wrap{position:relative;display:flex;align-items:center;flex-shrink:0}
/* Composer pills (mode / agent / model), per the mockup's .pill:
   translucent white glass with a soft edge. */
.mode-chip{display:flex;align-items:center;gap:5px;padding:5px 10px;border-radius:999px;background:rgba(255,255,255,0.06);border:1px solid var(--edge-soft);color:var(--muted);font-size:11.5px;font-weight:500;cursor:pointer;transition:border-color 0.15s,color 0.15s,background 0.15s;white-space:nowrap}
.mode-chip:hover{border-color:var(--glass-border);color:var(--text);background:rgba(255,255,255,0.12)}
.mode-chip .chevron{font-size:8px;color:var(--faint);flex:0 0 auto;transition:transform 0.15s}
.mode-chip.open .chevron{transform:rotate(180deg)}
.mode-chip-dropdown{position:absolute;bottom:calc(100% + 8px);left:0;min-width:232px;max-width:calc(100vw - 32px);background:var(--glass-bg-strong);-webkit-backdrop-filter:var(--glass-blur);backdrop-filter:var(--glass-blur);border:1px solid var(--glass-border);border-radius:16px;padding:5px;z-index:50;display:none;box-shadow:var(--glass-shadow)}
.mode-chip-dropdown.open{display:block}
.mode-item{display:flex;align-items:flex-start;gap:8px;padding:8px 10px;border-radius:8px;cursor:pointer;transition:background 0.15s}
.mode-item:hover{background:var(--surface2)}
.mode-item-icon{font-size:13px;flex:0 0 auto;padding-top:1px}
.mode-item-main{flex:1;min-width:0}
.mode-item-name{font-size:12px;color:var(--text)}
.mode-item-desc{font-size:10px;color:var(--faint);margin-top:1px;line-height:1.4}
.mode-item .om-check{margin-left:4px;padding-top:1px}
.composer-row .mic{width:28px;height:28px;border-radius:50%;border:none;background:transparent;color:var(--muted);font-size:14px;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:background 0.15s,color 0.15s;flex:0 0 auto}
.composer-row .mic:hover{background:var(--surface2);color:var(--text)}
.composer-row .mic.recording{background:var(--err);color:#fff;animation:mic-pulse 1.2s ease-in-out infinite}
.composer-row .mic.transcribing{background:var(--surface2);color:var(--warn)}
@keyframes mic-pulse{0%,100%{box-shadow:0 0 0 0 rgba(240,112,112,0.5)}50%{box-shadow:0 0 0 6px rgba(240,112,112,0)}}
.composer-row .cancel{padding:5px 11px;border-radius:999px;border:1px solid var(--err);background:transparent;color:var(--err);font-size:11.5px;cursor:pointer;display:none;flex:0 0 auto}
.composer-row .cancel.active{display:block}
/* Round red send button with a soft red glow, per the mockup's .send. */
.composer-row .send{width:32px;height:32px;border-radius:50%;border:none;background:linear-gradient(180deg,var(--accent),#e0353b);color:#fff;font-size:15px;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:background 0.15s,opacity 0.15s,box-shadow 0.15s,transform 0.15s;flex:0 0 auto;box-shadow:0 6px 16px -5px rgba(255,82,87,0.6),inset 0 1px 0 rgba(255,255,255,0.3)}
.composer-row .send:hover{background:linear-gradient(180deg,var(--accent-hover),#e0353b);box-shadow:0 8px 20px -5px rgba(255,82,87,0.7),inset 0 1px 0 rgba(255,255,255,0.35);transform:translateY(-1px)}
.composer-row .send:disabled{opacity:0.35;cursor:not-allowed;box-shadow:none;transform:none}
/* ---- Model picker pill + pop-up dropdown ---- */
.model-chip-wrap{position:relative;display:flex;align-items:center;min-width:0;flex-shrink:1}
/* Model pill: violet-tinted glass, per the mockup's .pill.model (the
   one place violet leads in the composer row). */
.model-chip{display:flex;align-items:center;gap:6px;padding:5px 10px;border-radius:999px;background:rgba(167,139,250,0.12);border:1px solid rgba(167,139,250,0.35);color:var(--text);font-size:11.5px;font-weight:500;cursor:pointer;transition:border-color 0.15s,color 0.15s,background 0.15s;max-width:100%;min-width:0}
.model-chip:hover{border-color:rgba(167,139,250,0.55);color:var(--text);background:rgba(167,139,250,0.2)}
.model-chip .name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
.model-chip .chevron{font-size:8px;color:var(--faint);flex:0 0 auto;transition:transform 0.15s}
.model-chip.open .chevron{transform:rotate(180deg)}
.model-chip.needs-selection{border-color:var(--warn);animation:chip-pulse 1.6s ease-in-out infinite}
.model-chip.model-ready-flash{border-color:var(--ok);box-shadow:0 0 0 3px rgba(88,189,125,0.25);transition:all 0.4s}
.model-chip.loading::before{content:'';width:10px;height:10px;border:2px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin 0.8s linear infinite;flex:0 0 auto}
.model-chip-progress{display:none;width:18px;height:18px;position:relative;flex:0 0 auto}
.model-chip.downloading .model-chip-progress{display:block}
.model-chip-progress svg{transform:rotate(-90deg);display:block}
.model-chip-progress circle{fill:none;stroke:var(--border);stroke-width:2}
.model-chip-progress .fg{stroke:var(--accent);stroke-dasharray:43.98;stroke-dashoffset:43.98;transition:stroke-dashoffset 0.3s}
.model-chip-progress .pct{position:absolute;top:0;left:0;width:18px;height:18px;display:flex;align-items:center;justify-content:center;font-size:7px;font-weight:700;color:var(--text)}
/* The pill sits on the RIGHT side of the composer row, so the popup is
   right-anchored and grows leftward; clamp against the left edge. */
/* Model picker dropdown = frosted glass slab, per the mockup's .picker
   (heavy blur, luminous top edge, deep drop shadow, generous radius). */
.model-chip-dropdown{position:absolute;bottom:calc(100% + 8px);right:0;left:auto;min-width:260px;max-width:calc(100vw - 32px);background:var(--glass-bg-strong);-webkit-backdrop-filter:var(--glass-blur-lg);backdrop-filter:var(--glass-blur-lg);border:1px solid var(--glass-border);border-radius:18px;padding:8px;max-height:340px;overflow-y:auto;z-index:50;display:none;box-shadow:0 30px 70px -16px rgba(0,0,0,0.75),inset 0 1px 0 var(--specular)}
.model-chip-dropdown.open{display:block}
/* Group labels: uppercase, tracked, faint — the mockup's .ph-lbl. */
.mcd-header{padding:8px 12px 5px;font-size:9.5px;font-weight:600;letter-spacing:0.13em;text-transform:uppercase;color:var(--faint)}
/* Model rows = the mockup's .mrow. */
.model-chip-item{display:flex;align-items:center;gap:11px;padding:9px 12px;border-radius:11px;cursor:pointer;transition:background 0.15s}
.model-chip-item:hover{background:rgba(255,255,255,0.06)}
.model-chip-item.disabled{opacity:0.45;cursor:default}
.model-chip-item.disabled:hover{background:transparent}
.model-chip-item .mmain{flex:1;min-width:0}
.model-chip-item .mname{font-size:12.5px;font-weight:550;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
/* Mono quant/size subtitle, per the mockup's .mrow .nm small. */
.model-chip-item .msize{font-size:10px;color:var(--faint);margin-top:1px;font-family:var(--mono)}
.mcheck{font-size:13px;font-weight:700;flex:0 0 auto}
/* Selected/loaded row shows the mockup's green check (.ck). */
.mcheck.on{color:var(--ok)}
.mcheck.disk{color:var(--faint)}
.mget{font-size:11px;color:var(--faint);flex:0 0 auto;white-space:nowrap}
.mpct{font-size:10.5px;color:var(--warn);flex:0 0 auto;font-variant-numeric:tabular-nums}
.mrow-bar{height:3px;border-radius:2px;background:rgba(255,255,255,0.10);overflow:hidden;margin-top:5px}
.mrow-fill{height:100%;background:var(--accent);width:0;transition:width 0.3s}
.mcd-footer{margin-top:4px;border-top:1px solid var(--edge-soft);padding:9px 12px;font-size:11.5px;color:var(--muted);cursor:pointer;transition:color 0.15s,background 0.15s;border-radius:0 0 11px 11px}
.mcd-footer:hover{color:var(--text);background:rgba(255,255,255,0.06)}
/* Light theme: the 6%-white hover states vanish on a bright ground, so
   swap them for a subtle gray, and neutralize the mono subtitle color. */
:root[data-theme="light"] .model-chip-item:hover{background:var(--surface2)}
:root[data-theme="light"] .mcd-footer:hover{background:var(--surface2)}
:root[data-theme="light"] .mrow-bar{background:var(--surface2)}
:root[data-theme="light"] .model-chip:hover,
:root[data-theme="light"] .mode-chip:hover,
:root[data-theme="light"] .browse-btn:hover{background:var(--surface2)}
/* Cloud CTA: the one place violet leads (a cloud/AI touch). Frosted
   violet glass card that lifts gently on hover. */
/* Cloud CTA: the mockup's violet gradient card (.cta) — violet-tinted
   bg, violet border, icon + title + subtitle + arrow; lifts on hover. */
.mcd-cloud-cta{display:flex;align-items:center;gap:11px;padding:11px 12px;margin-bottom:6px;border-radius:13px;cursor:pointer;background:linear-gradient(100deg,rgba(167,139,250,0.22),rgba(142,162,255,0.08));border:1px solid rgba(167,139,250,0.42);box-shadow:inset 0 1px 0 rgba(255,255,255,0.16);transition:background 0.15s,transform 0.15s,border-color 0.15s,box-shadow 0.15s}
.mcd-cloud-cta:hover{background:linear-gradient(100deg,rgba(167,139,250,0.34),rgba(142,162,255,0.14));border-color:rgba(167,139,250,0.6);transform:translateY(-1px);box-shadow:0 10px 24px -12px rgba(167,139,250,0.5),inset 0 1px 0 rgba(255,255,255,0.2)}
.mcd-cloud-cta .mcc-icon{font-size:16px;line-height:1}
.mcd-cloud-cta .mcc-text{flex:1;display:flex;flex-direction:column;font-size:12.5px;font-weight:650;color:var(--text)}
.mcd-cloud-cta .mcc-sub{font-size:10px;font-weight:400;color:var(--muted);margin-top:1px}
.mcd-cloud-cta .mcc-arrow{color:var(--muted);font-size:14px}
/* ---- Model management overlay ---- */
.model-panel{position:absolute;top:0;left:0;right:0;bottom:0;background:var(--bg);z-index:10;display:none;flex-direction:column;overflow-y:auto}
.model-panel.open{display:flex}
.model-panel-header{padding:10px 16px;border-bottom:1px solid var(--border);display:flex;align-items:center;justify-content:space-between}
.model-panel-header h3{font-size:13px;font-weight:600;color:var(--text)}
.model-card{padding:12px 16px;border-bottom:1px solid var(--surface2)}
.model-card .name{font-size:13px;font-weight:600;color:var(--text)}
.model-card .meta{font-size:11px;color:var(--faint);margin-top:2px}
.model-card .card-actions{margin-top:8px;display:flex;gap:6px;align-items:center}
.model-card .btn{padding:4px 12px;border-radius:7px;font-size:11px;cursor:pointer;border:1px solid var(--border);background:var(--surface);color:var(--muted);transition:border-color 0.15s,color 0.15s}
.model-card .btn:hover{border-color:var(--faint);color:var(--text)}
.model-card .btn:disabled{opacity:0.4;cursor:not-allowed}
.model-card .btn.primary{background:var(--accent);border-color:var(--accent);color:#fff}
.model-card .btn.primary:hover{background:var(--accent-hover)}
.model-card .btn.danger{border-color:var(--err);color:var(--err);background:transparent}
.model-card .btn.danger:hover{background:rgba(240,112,112,0.12)}
.model-card .badge{display:inline-block;padding:2px 8px;border-radius:5px;font-size:10px;font-weight:600}
.model-card .badge.active{background:rgba(88,189,125,0.14);color:var(--ok)}
.model-card .badge.downloaded{background:var(--surface2);color:var(--muted)}
.model-card .badge.unavailable{background:var(--surface);color:var(--faint)}
.model-card .progress-wrap{margin-top:6px;display:none}
.model-card .progress-wrap.active{display:block}
.model-card .progress-bar{height:4px;border-radius:2px;background:var(--surface2);overflow:hidden}
.model-card .progress-fill{height:100%;background:var(--accent);transition:width 0.3s;width:0}
.model-card .progress-text{font-size:10px;color:var(--faint);margin-top:2px}
/* ---- First-run welcome overlay ---- */
/* First-run takeover: must fully obscure whatever is behind it, so it
   keeps an opaque --bg base with a faint glass sheen on top and a heavy
   backdrop blur for depth. */
.welcome-overlay{position:absolute;top:0;left:0;right:0;bottom:0;background:linear-gradient(var(--glass-bg),var(--glass-bg)),var(--bg);-webkit-backdrop-filter:blur(40px) saturate(1.6);backdrop-filter:blur(40px) saturate(1.6);z-index:20;display:none;flex-direction:column;align-items:center;justify-content:center;padding:24px;text-align:center}
.welcome-overlay.open{display:flex}
.welcome-logo-area{display:flex;flex-direction:column;align-items:center;gap:8px;margin-bottom:8px}
.welcome-logo-img{width:56px;height:56px;border-radius:14px;box-shadow:0 2px 12px rgba(0,0,0,0.5)}
.welcome-logo-text{font-size:19px;font-weight:700;color:var(--text)}
.welcome-text{color:var(--muted);font-size:13px;max-width:320px;line-height:1.6;margin-bottom:16px}
.welcome-features{text-align:left;max-width:280px;margin-bottom:20px}
.welcome-features div{padding:6px 0;font-size:12px;color:var(--muted);display:flex;align-items:center;gap:8px}
.welcome-features .feat-icon{color:var(--accent);font-size:14px}
.welcome-btn{padding:12px 32px;border-radius:12px;border:none;background:var(--accent);color:#fff;font-size:14px;font-weight:600;cursor:pointer;transition:background 0.15s;margin-bottom:8px}
.welcome-btn:hover{background:var(--accent-hover)}
.welcome-btn:disabled{opacity:0.6;cursor:default}
.welcome-skip{color:var(--faint);font-size:11px;cursor:pointer;border:none;background:none;padding:4px}
.welcome-skip:hover{color:var(--muted)}
.welcome-progress{width:100%;max-width:280px;margin-top:12px;display:none}
.welcome-progress.active{display:block}
.welcome-pbar{height:6px;border-radius:3px;background:var(--surface2);overflow:hidden}
.welcome-pfill{height:100%;background:var(--accent);transition:width 0.3s;width:0}
.welcome-ptext{font-size:11px;color:var(--muted);margin-top:4px}
/* ---- Profile editor (/fill) ---- */
.profile-editor{display:flex;flex-direction:column;gap:6px;padding:8px;background:var(--surface);border:1px solid var(--border);border-radius:8px}
.profile-help{font-size:11px;color:var(--muted);margin-bottom:4px;line-height:1.5}
.profile-help code{background:var(--surface2);padding:1px 4px;border-radius:3px}
.profile-row{display:flex;align-items:center;gap:8px}
.profile-row label{flex:0 0 110px;font-size:11px;color:var(--muted)}
.profile-row input{flex:1;padding:4px 8px;font-size:12px;background:var(--bg);color:var(--text);border:1px solid var(--border);border-radius:6px}
.profile-row input:focus{outline:none;border-color:var(--faint)}
.profile-actions{display:flex;gap:6px;margin-top:6px}
.profile-actions button{padding:4px 12px;font-size:12px;border-radius:6px;background:var(--accent);color:#fff;border:none;cursor:pointer}
.profile-actions button:hover{background:var(--accent-hover)}
.profile-actions .profile-cancel{background:var(--surface2);color:var(--muted)}
.profile-actions .profile-cancel:hover{color:var(--text)}
/* ---- History clusters (/history) ---- */
.history-summary{font-size:11px;color:var(--muted);margin-bottom:8px;padding-bottom:6px;border-bottom:1px solid var(--border)}
.history-cluster{margin-bottom:6px;background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:6px 10px}
.history-cluster summary{cursor:pointer;display:flex;justify-content:space-between;align-items:center;font-size:12px;list-style:none}
.history-cluster summary::-webkit-details-marker{display:none}
.hist-label{font-weight:600;color:var(--text)}
.hist-count{font-size:10px;color:var(--muted);background:var(--surface2);padding:2px 8px;border-radius:10px}
.history-cluster ul{margin:6px 0 0 0;padding-left:0;list-style:none}
.history-cluster li{padding:3px 0;font-size:11px;display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.history-cluster li a{color:var(--muted);text-decoration:none;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1;min-width:0}
.history-cluster li a:hover{color:var(--text);text-decoration:underline}
.hist-meta{color:var(--faint);font-size:10px;flex-shrink:0;font-variant-numeric:tabular-nums}
</style>
<script>
  // Apply the persisted theme before first paint to avoid a flash.
  // Default is 'gray' (no attribute — the vars users know); explicit
  // 'black' or 'light' choices set the override attribute.
  try {
    var t = localStorage.getItem('moltTheme');
    if (t === 'light' || t === 'black')
      document.documentElement.setAttribute('data-theme', t);
  } catch (e) {}
</script>
</head>
<body>
<!-- Liquid Glass ambient orbs: purely decorative, behind all content.
     Styled by .lg-ambient; disabled on the light theme. -->
<div class="lg-ambient" aria-hidden="true"><div class="lg-orb"></div></div>
<div class="header">
  <button class="icon-btn" onclick="toggleDrawer()" title="Chats">&#9776;</button>
  <div class="title">MoltBrowser AI</div>
  <div class="status offline" id="statusIndicator">Initializing...</div>
  <div class="header-actions">
    <button class="icon-btn" onclick="newChat()" title="New chat">&#9998;</button>
    <div class="overflow-wrap">
      <button class="icon-btn" onclick="toggleOverflowMenu(event)" title="More options">&#8943;</button>
      <div class="overflow-menu" id="overflowMenu">
        <div class="om-item" onclick="closeOverflowMenu();newChat()">New chat</div>
        <div class="om-item" onclick="closeOverflowMenu();toggleSearch()">Search in chat</div>
        <div class="om-item" onclick="closeOverflowMenu();exportChat()">Export chat</div>
        <div class="om-item" onclick="closeOverflowMenu();importChat()">Import chat</div>
        <div class="om-item" onclick="closeOverflowMenu();toggleModelPanel()">Manage models</div>
        <div class="om-sep"></div>
        <div class="om-label">Theme</div>
        <div class="om-item" onclick="setTheme('gray')">Gray<span class="om-check visible" id="thCheckGray">&#10003;</span></div>
        <div class="om-item" onclick="setTheme('black')">Black<span class="om-check" id="thCheckBlack">&#10003;</span></div>
        <div class="om-item" onclick="setTheme('light')">White<span class="om-check" id="thCheckLight">&#10003;</span></div>
        <div class="om-sep"></div>
        <div class="om-item" onclick="closeOverflowMenu();openMoltSettings()">Settings</div>
        <div class="om-meta"><span id="hwGpu"></span><span id="hwRam"></span><span id="hwCores"></span></div>
      </div>
    </div>
    <button class="icon-btn" onclick="closeSidePanel()" title="Close panel" aria-label="Close panel">&#10005;</button>
  </div>
</div>
<!-- Recents drawer: overlay conversation list. Backed by the
     listConversations / loadConversation / deleteConversation
     messages on MoltAIChatHandler; autosaved per turn from JS. -->
<div class="drawer-scrim" id="drawerScrim" onclick="closeDrawer()"></div>
<div class="drawer" id="drawer">
  <button class="drawer-new" onclick="closeDrawer();newChat()">+ New chat</button>
  <div class="drawer-search"><input type="text" id="drawerSearch" placeholder="Search chats" oninput="filterConvList()"></div>
  <div class="drawer-label">Recents</div>
  <div class="conv-list" id="convList"></div>
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
    // Quick-action chips only make sense when a real page is loaded,
    // so they follow the same visibility as the tab-context strip.
    var qa = document.getElementById('quickActions');
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
      if (qa) qa.style.display = 'none';
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
    if (qa) qa.style.display = 'flex';
  };
  if (window.__moltLastTabContext)
    window.__moltSetTabContext(window.__moltLastTabContext);
</script>
<!-- Agent Inbox: live tray of currently-running background automations.
     Polled every 3s from JS; hidden when no runs are active. -->
<div class="agent-inbox" id="agentInbox" style="display:none;"></div>
<div class="messages" id="messages">
  <div class="message ai">
    <div class="sender">AI Assistant</div>
    <div class="text"><span id="welcomeMsgText">Welcome! Choose a model below to start.</span><div class="welcome-hint">Type /help for commands.</div></div>
  </div>
</div>
<div class="actions" id="quickActions" style="display:none">
  <button onclick="quickAction('summarize')">Summarize</button>
  <button onclick="quickAction('extract')">Extract data</button>
  <button onclick="quickAction('explain')">Explain</button>
  <button onclick="quickAction('translate')">Translate</button>
</div>
<div class="composer">
  <div class="attach-chip-wrap" id="attachChipWrap" style="display:none"></div>
  <div class="agent-hint" id="agentHint" style="display:none">Tell the agent what to do &#8212; e.g. &quot;find MoonSwatch prices&quot;</div>
  <div class="dictation-hint" id="dictationHint"><span class="dictation-dot"></span><span id="dictationHintText">Listening&#8230;</span></div>
  <div id="inputArea" class="composer-card">
    <textarea id="chatInput" rows="1" placeholder="Ask MoltBrowser AI&#8230;" autofocus></textarea>
    <div class="composer-row">
      <div class="mode-chip-wrap">
        <button class="mode-chip" id="modeChip" onclick="toggleModeDropdown(event)" title="How agent actions run on the page">
          <span class="mode-icon" id="modeChipIcon">&#128737;</span>
          <span class="name" id="modeChipName">Ask first</span>
          <span class="chevron">&#9662;</span>
        </button>
        <div class="mode-chip-dropdown" id="modeChipDropdown">
          <div class="mode-item" onclick="setActionMode('ask')">
            <span class="mode-item-icon">&#128737;</span>
            <div class="mode-item-main">
              <div class="mode-item-name">Ask first</div>
              <div class="mode-item-desc">Confirm each page action before it runs</div>
            </div>
            <span class="om-check visible" id="ovCheckAsk">&#10003;</span>
          </div>
          <div class="mode-item" onclick="setActionMode('auto')">
            <span class="mode-item-icon">&#9889;</span>
            <div class="mode-item-main">
              <div class="mode-item-name">Auto</div>
              <div class="mode-item-desc">Run page actions immediately without asking</div>
            </div>
            <span class="om-check" id="ovCheckAuto">&#10003;</span>
          </div>
        </div>
      </div>
      <button class="browse-btn" id="agentBtn" title="AI browses and acts on this page">Agent</button>
      <button class="mic" id="attachBtn" onclick="pickAttachment()" title="Attach a file (pdf, docx, txt, md, png, jpg, webp)">&#128206;</button>
      <div class="composer-spacer"></div>
      <div class="model-chip-wrap">
        <button class="model-chip" id="modelChip" onclick="toggleModelDropdown(event)">
          <span class="model-chip-progress" id="modelChipProgress">
            <svg width="18" height="18" viewBox="0 0 18 18">
              <circle cx="9" cy="9" r="7"></circle>
              <circle class="fg" id="modelChipProgressFg" cx="9" cy="9" r="7"></circle>
            </svg>
            <span class="pct" id="modelChipPct">0%</span>
          </span>
          <span class="name" id="modelChipName">Choose model</span>
          <span class="chevron">&#9662;</span>
        </button>
        <div class="model-chip-dropdown" id="modelChipDropdown"></div>
      </div>
      <button class="mic" id="micBtn" onclick="toggleMic()" title="Hold or click to record (local Whisper)">🎙</button>
      <button class="cancel" id="cancelBtn" onclick="cancelGeneration()">Stop</button>
      <button class="send" id="sendBtn" onclick="sendMessage()" title="Send">&#8593;</button>
    </div>
  </div>
</div>

<!-- Model Management Panel -->
<div class="model-panel" id="modelPanel">
  <div class="model-panel-header">
    <h3>Model Management</h3>
    <button class="icon-btn labeled" onclick="toggleModelPanel()">Close</button>
  </div>
  <div id="modelList"></div>
</div>

<!-- First-Run Welcome Overlay -->
<div class="welcome-overlay" id="welcomeOverlay">
  <div class="welcome-logo-area"><img class="welcome-logo-img" src="data:image/png;base64,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" alt="MoltBrowser"><div class="welcome-logo-text">Welcome to MoltBrowser AI</div></div>
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
