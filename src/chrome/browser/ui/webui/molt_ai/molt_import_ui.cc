// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_import_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/ui/webui/molt_ai/molt_import_handler.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

// Serves the Import (browser migration) popover as inline HTML. The page talks
// to C++ over chrome.send (see MoltImportHandler) to drive the same
// molt_ai::BrowserImporter the settings import section uses. Sized for a
// compact bubble (~344x420); it also renders as a full tab for verification.
// Real glass is guaranteed by in-page ambient orbs behind a backdrop-filter
// frost layer — so the frosting reads even if the OS won't composite backdrop
// across the separate bubble widget. Structurally a sibling of MoltNetUI.
class MoltImportDataSource : public content::URLDataSource {
 public:
  MoltImportDataSource() = default;
  ~MoltImportDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltImportHost; }

  std::string GetMimeType(const GURL& url) override { return "text/html"; }

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
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Import</title>
<style>
  :root{
    --text:#f4f5fa;
    --muted:rgba(233,236,247,0.64);
    --faint:rgba(233,236,247,0.42);
    --accent:#ff5257;
    --accent-deep:#e0353b;
    --ok:#5fe3a1;
    --frost:rgba(18,20,30,0.52);
    --edge:rgba(255,255,255,0.14);
    --edge-hi:rgba(255,255,255,0.30);
    --specular:rgba(255,255,255,0.26);
    --blur:blur(30px) saturate(175%);
    --font:-apple-system,"SF Pro Text",BlinkMacSystemFont,"Helvetica Neue",Arial,sans-serif;
  }
  *{margin:0;padding:0;box-sizing:border-box}
  html,body{height:100%;background:transparent}
  body{font-family:var(--font);color:var(--text);-webkit-font-smoothing:antialiased;
    display:flex;align-items:center;justify-content:center;overflow:hidden}

  /* Rounded, clipped surface with a floating drop shadow in the transparent
     margin around it. */
  .panel{position:relative;width:320px;height:396px;max-width:calc(100vw - 20px);
    border-radius:18px;overflow:hidden;border:1px solid var(--edge);
    box-shadow:0 22px 50px -14px rgba(0,0,0,.68),0 2px 8px rgba(0,0,0,.4);
    display:flex}

  /* Ambient orbs — the guaranteed glass substrate. */
  .env{position:absolute;inset:0;z-index:0;overflow:hidden}
  .orb{position:absolute;border-radius:50%;filter:blur(64px)}
  .orb.a{width:76%;height:76%;left:-18%;top:-22%;background:radial-gradient(circle at 35% 35%,#4a2f8f,#241645 70%)}
  .orb.b{width:70%;height:70%;right:-16%;bottom:-18%;background:radial-gradient(circle at 60% 40%,#c0303f,#7a2036 72%)}
  .orb.c{width:60%;height:60%;left:22%;bottom:-14%;background:radial-gradient(circle at 44% 56%,#159aad,#0f5f6e 72%)}

  /* Frost layer — backdrop-filter blurs the orbs behind it (same compositor,
     always works) plus the toolbar behind the bubble when the OS composites
     it (bonus). */
  .frost{position:absolute;inset:0;z-index:1;display:flex;flex-direction:column;
    background:var(--frost);
    -webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
    box-shadow:inset 0 1px 0 var(--specular)}

  .hdr{padding:16px 18px 12px;display:flex;align-items:center;gap:10px;flex:0 0 auto}
  .dot{width:9px;height:9px;border-radius:50%;background:var(--faint);transition:.25s}
  .dot.on{background:var(--ok);box-shadow:0 0 10px 1px rgba(95,227,161,.6)}
  .dot.busy{background:var(--accent);box-shadow:0 0 10px 1px rgba(255,82,87,.5)}
  .title{font-size:14px;font-weight:700;letter-spacing:-.01em}
  .st{font-size:11px;color:var(--muted);margin-top:1px}

  .sec{font-size:10px;letter-spacing:.16em;text-transform:uppercase;color:var(--faint);
    padding:10px 18px 6px;flex:0 0 auto}

  .list{flex:1 1 auto;overflow-y:auto;padding:0 8px 4px}
  .list::-webkit-scrollbar{width:8px}
  .list::-webkit-scrollbar-thumb{background:rgba(255,255,255,.14);border-radius:4px;border:2px solid transparent;background-clip:content-box}
  .row{display:flex;align-items:center;gap:11px;padding:9px 12px;border-radius:11px;cursor:pointer;
    border:1px solid transparent;transition:background .14s,border-color .14s}
  .row:hover{background:rgba(255,255,255,.06);border-color:var(--edge)}
  .row .flag{width:22px;height:22px;flex:0 0 auto;display:flex;align-items:center;justify-content:center}
  .row .flag svg{width:20px;height:20px;display:block}
  .row .name{font-size:13px;flex:1}
  .row .chk{color:var(--faint);font-size:15px;opacity:0;flex:0 0 auto;transition:opacity .14s}
  .row:hover .chk{opacity:.8}
  .row.active{background:rgba(95,227,161,.10);border-color:rgba(95,227,161,.30)}
  .row.active .chk{opacity:1;color:var(--ok)}
  .row.dim{opacity:.4;cursor:default}
  .row.dim:hover{background:transparent;border-color:transparent}
  .row.dim:hover .chk{opacity:0}
  .empty{font-size:12px;color:var(--faint);text-align:center;padding:22px 16px}

  .divider{height:1px;background:var(--edge);margin:0 14px;flex:0 0 auto}

  .toggle-row{padding:12px 18px 8px;display:flex;align-items:center;gap:12px;flex:0 0 auto}
  .toggle-row .lbl{font-size:12.5px;color:var(--muted)}
  .toggle-row .sub{font-size:11px;color:var(--faint);margin-top:1px}
  .sw{margin-left:auto;width:44px;height:25px;border-radius:13px;background:rgba(255,255,255,.12);
    border:1px solid var(--edge);cursor:pointer;position:relative;transition:.2s;flex:0 0 auto}
  .sw.on{background:linear-gradient(180deg,#ff676c,var(--accent-deep));border-color:transparent}
  .sw .knob{position:absolute;top:2px;left:2px;width:19px;height:19px;border-radius:50%;
    background:#fff;transition:.2s;box-shadow:0 1px 3px rgba(0,0,0,.35)}
  .sw.on .knob{transform:translateX(19px)}

  .status{flex:0 0 auto;min-height:16px;padding:2px 18px 4px;font-size:11px;
    color:var(--muted);text-align:center;line-height:1.35}

  .foot{flex:0 0 auto;display:flex;gap:8px;padding:8px 14px 14px}
  .act{flex:1;padding:9px 4px;border-radius:11px;cursor:pointer;text-align:center;
    font-size:11px;font-weight:600;background:rgba(255,255,255,.05);
    border:1px solid var(--edge);color:var(--muted);transition:.15s;white-space:nowrap}
  .act:hover{background:rgba(255,255,255,.11);border-color:var(--edge-hi);color:var(--text)}
  .act.primary{flex:2;background:linear-gradient(180deg,#ff676c,var(--accent-deep));
    border-color:transparent;color:#fff}
  .act.primary:hover{filter:brightness(1.06);color:#fff}

  /* Segmented [ Bookmarks | Import ] switch under the header. */
  .tabs{display:flex;gap:4px;margin:0 14px 4px;padding:3px;border-radius:12px;
    background:rgba(255,255,255,.05);border:1px solid var(--edge);flex:0 0 auto}
  .tab{flex:1;text-align:center;font-size:12px;font-weight:600;padding:7px 4px;
    border-radius:9px;cursor:pointer;color:var(--muted);transition:.15s;user-select:none}
  .tab:hover{color:var(--text)}
  .tab.on{background:rgba(255,255,255,.12);color:var(--text);
    box-shadow:inset 0 1px 0 var(--specular)}

  /* Each tab's pane fills the remaining height; only one is shown at a time. */
  .view{flex:1 1 auto;display:flex;flex-direction:column;min-height:0}
  .view[hidden]{display:none}

  .search{margin:2px 14px 8px;flex:0 0 auto}
  .search input{width:100%;padding:8px 11px;border-radius:10px;font-size:12.5px;
    color:var(--text);background:rgba(255,255,255,.06);border:1px solid var(--edge);
    outline:none;font-family:var(--font)}
  .search input::placeholder{color:var(--faint)}
  .search input:focus{border-color:var(--edge-hi);background:rgba(255,255,255,.09)}

  /* Two-line bookmark rows: title over a dim host. */
  .row .meta{flex:1;min-width:0;display:flex;flex-direction:column}
  .row .meta .name{flex:0 0 auto;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .row .meta .sub{font-size:11px;color:var(--faint);white-space:nowrap;
    overflow:hidden;text-overflow:ellipsis;margin-top:1px}

  .empty .lnk{color:var(--accent);cursor:pointer;font-weight:600}
  .empty .lnk:hover{text-decoration:underline}

  @media (prefers-reduced-motion: reduce){*{transition:none!important}}
</style>
</head>
<body>
<div class="panel">
  <div class="env"><div class="orb a"></div><div class="orb b"></div><div class="orb c"></div></div>
  <div class="frost">
    <div class="hdr">
      <span class="dot" id="dot"></span>
      <div><div class="title" id="title">Bookmarks</div><div class="st" id="st">&nbsp;</div></div>
    </div>
    <div class="tabs">
      <div class="tab on" id="tab-bm" onclick="setTab('bookmarks')">Bookmarks</div>
      <div class="tab" id="tab-im" onclick="setTab('import')">Import</div>
    </div>

    <!-- DEFAULT view: the user's own saved bookmarks. -->
    <div class="view" id="view-bm">
      <div class="search"><input id="bmsearch" type="text" placeholder="Search bookmarks&hellip;" autocomplete="off" spellcheck="false"></div>
      <div class="list" id="bmlist"></div>
    </div>

    <!-- SECOND view: migrate bookmarks/passwords from another browser. -->
    <div class="view" id="view-im" hidden>
      <div class="sec">Import from</div>
      <div class="list" id="implist"></div>
      <div class="divider"></div>
      <div class="toggle-row">
        <div><div class="lbl">Include saved passwords</div><div class="sub">Off by default &middot; may prompt for Keychain</div></div>
        <div class="sw" id="sw" onclick="togglePw()"><div class="knob"></div></div>
      </div>
      <div class="status" id="status"></div>
      <div class="foot">
        <div class="act primary" id="go" onclick="importPrimary()">Import bookmarks</div>
        <div class="act" onclick="go('molt://ai-settings/?section=import')">Manage</div>
      </div>
    </div>
  </div>
</div>
<script>
// chrome.send promise bridge (same shape as the MoltNet popover).
var _id=0,_pend={};
function sendWithPromise(m){var a=[].slice.call(arguments,1);var id=m+'_'+(++_id);
  return new Promise(function(res,rej){_pend[id]={res:res,rej:rej};chrome.send(m,[id].concat(a));});}
window.cr=window.cr||{};
cr.webUIResponse=function(id,ok,r){var c=_pend[id];if(c){delete _pend[id];ok?c.res(r):c.rej(r);}};
// FireWebUIListener("event", value) calls cr.webUIListenerCallback in JS. This
// lightweight page bridges those onto DOM CustomEvents (same shape as the
// settings page), so cr.addWebUIListener('import-progress', ...) works.
cr.webUIListenerCallback=function(event){
  var detail=arguments.length>1?arguments[1]:undefined;
  document.dispatchEvent(new CustomEvent(event,{detail:detail}));};
cr.addWebUIListener=cr.addWebUIListener||function(event,fn){
  document.addEventListener(event,function(e){fn(e.detail);});};

function el(id){return document.getElementById(id);}
function go(u){window.location.href=u;}

// Per-browser marks: small ORIGINAL simplified icons built from each source's
// signature colors + geometric shapes (nominative use to identify the SOURCE
// browser we import FROM — not pixel-exact trademarked logos). Keyed by the
// lowercase wire id the backend sends. Constant strings only (never backend
// data), so assigning them via innerHTML is safe. Falls back to a neutral globe.
var LOGO={
  chrome:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<circle cx="12" cy="12" r="11" fill="#e8453c"/>'+
    '<path d="M12 1a11 11 0 0 1 9.5 5.5H12a5.5 5.5 0 0 0-4.9 3L3.4 5.2A11 11 0 0 1 12 1z" fill="#fbbc05"/>'+
    '<path d="M2.5 6.7 8 16a5.5 5.5 0 0 0 4 3l-3 4A11 11 0 0 1 2.5 6.7z" fill="#34a853"/>'+
    '<circle cx="12" cy="12" r="4.5" fill="#4285f4"/>'+
    '<circle cx="12" cy="12" r="3.4" fill="#fff"/><circle cx="12" cy="12" r="2.6" fill="#4285f4"/></svg>',
  chromium:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<circle cx="12" cy="12" r="11" fill="#557aa8"/>'+
    '<path d="M12 1a11 11 0 0 1 9.5 5.5H12a5.5 5.5 0 0 0-4.9 3L3.4 5.2A11 11 0 0 1 12 1z" fill="#8fb4d6"/>'+
    '<path d="M2.5 6.7 8 16a5.5 5.5 0 0 0 4 3l-3 4A11 11 0 0 1 2.5 6.7z" fill="#a9c7e0"/>'+
    '<circle cx="12" cy="12" r="4.5" fill="#2c5c8f"/>'+
    '<circle cx="12" cy="12" r="3.4" fill="#eef4fa"/><circle cx="12" cy="12" r="2.6" fill="#2c5c8f"/></svg>',
  edge:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<defs><linearGradient id="mbEdge" x1="0" y1="0" x2="1" y2="1">'+
    '<stop offset="0" stop-color="#37c6d0"/><stop offset="1" stop-color="#1b74c8"/></linearGradient></defs>'+
    '<path d="M12 1a11 11 0 0 1 10.6 8c-1.2-2.4-3.7-3.6-6.4-3.6-4 0-7.2 2.7-7.2 6 0 1.9 1 3.3 2.4 4.3C7 15 3 12.4 3 8.6 3 4.2 7 1 12 1z" fill="url(#mbEdge)"/>'+
    '<path d="M22.6 9c.3 1 .4 2 .4 3 0 6.1-4.9 11-11 11-3.4 0-6-1.4-7.7-3.6 1.6 1 3.6 1.4 5.6 1.1 4.6-.7 7.6-3.7 8.1-6.9.4-2.4-.7-4-2.2-5.1 2.3-.3 4.9.4 6.8.4z" fill="#3aa6e0"/></svg>',
  brave:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<path d="M12 1.5 20 4l-.6 9.2c-.2 3-2.1 5.6-4.9 6.9L12 21.5l-2.5-1.4c-2.8-1.3-4.7-3.9-4.9-6.9L4 4z" fill="#f15a24"/>'+
    '<path d="M12 4 18 5.8l-.5 7.3c-.1 2.2-1.5 4.1-3.6 5.1L12 19l-1.9-.8c-2.1-1-3.5-2.9-3.6-5.1L6 5.8z" fill="#e8471c"/>'+
    '<path d="M12 7 9.2 9.5l1 1.4-1.7 1.9 1.3 2.2L12 17l2.2-2 1.3-2.2-1.7-1.9 1-1.4z" fill="#fff"/></svg>',
  opera:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<circle cx="12" cy="12" r="11" fill="#e8203a"/>'+
    '<ellipse cx="12" cy="12" rx="4.6" ry="7.2" fill="#fff"/></svg>',
  vivaldi:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<rect x="1.5" y="1.5" width="21" height="21" rx="6" fill="#ef3939"/>'+
    '<path d="M6 8h5l-2.5 8z" fill="#fff"/><path d="M18 8h-4l1.6 5z" fill="#fff"/>'+
    '<circle cx="12" cy="10" r="1.6" fill="#fff"/></svg>',
  firefox:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<defs><radialGradient id="mbFf" cx="0.4" cy="0.7" r="0.9">'+
    '<stop offset="0" stop-color="#ffdd44"/><stop offset="0.5" stop-color="#ff8c1a"/>'+
    '<stop offset="1" stop-color="#e0430f"/></radialGradient></defs>'+
    '<path d="M12 1.5c1.5 2 1.2 4 .3 5.4 1.2-.6 2-.2 2.5.6.9-1 .6-2.3.6-2.3 2.8 2 4.6 5.3 4.6 9 0 5.4-4.3 9.8-9.6 9.8S1.5 19.6 1.5 14.2c0-3 1.3-5.4 3.2-6.9-.4 1.4.1 2.7.9 3.4-.6-2.6.6-5.2 2.6-6.7-.5 1.7.2 3 1.3 3.7C11.7 6.3 12.6 3.9 12 1.5z" fill="url(#mbFf)"/>'+
    '<circle cx="11.5" cy="15" r="4.5" fill="#ffd23f" opacity="0.5"/></svg>',
  safari:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<circle cx="12" cy="12" r="11" fill="#1b9df0"/>'+
    '<circle cx="12" cy="12" r="9" fill="#e8f3fb"/>'+
    '<path d="M12 12 16 8l-2 6z" fill="#f4402e"/>'+
    '<path d="M12 12 8 16l2-6z" fill="#c8d6e0"/></svg>',
  globe:'<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">'+
    '<circle cx="12" cy="12" r="10.5" fill="none" stroke="#888" stroke-width="1.6"/>'+
    '<ellipse cx="12" cy="12" rx="5" ry="10.5" fill="none" stroke="#888" stroke-width="1.6"/>'+
    '<line x1="1.5" y1="12" x2="22.5" y2="12" stroke="#888" stroke-width="1.6"/></svg>'};

var STATE={tab:'bookmarks',browsers:[],selected:'',includePasswords:false,
           busy:false,bookmarks:[],filter:''};

function isInstalled(b){return b.installed!==false;}
function setStatus(t){el('status').textContent=t||'';}

// Pure client-side show/hide between the Bookmarks and Import panes, plus the
// shared header (dot + title + subline) which reflects the active tab.
function setTab(t){
  STATE.tab=t;
  el('tab-bm').className='tab'+(t==='bookmarks'?' on':'');
  el('tab-im').className='tab'+(t==='import'?' on':'');
  el('view-bm').hidden=(t!=='bookmarks');
  el('view-im').hidden=(t!=='import');
  updateHeader();
}

function updateHeader(){
  if(STATE.tab==='bookmarks'){
    el('title').textContent='Bookmarks';
    el('dot').className='dot on';
    var n=STATE.bookmarks.length;
    el('st').textContent=n?(n+' saved'):'No bookmarks yet';
  }else{
    el('title').textContent='Import';
    el('dot').className='dot'+(STATE.busy?' busy':(STATE.browsers.length?' on':''));
    var m=STATE.browsers.filter(isInstalled).length;
    el('st').textContent=m?(m+' browser'+(m===1?'':'s')+' detected')
                          :'No other browsers detected';
  }
}

function render(){
  el('sw').className='sw'+(STATE.includePasswords?' on':'');
  el('go').textContent=STATE.includePasswords?'Import bookmarks & passwords':'Import bookmarks';
  var list=el('implist');list.innerHTML='';
  if(!STATE.browsers.length){
    var e=document.createElement('div');e.className='empty';
    e.textContent='No other browsers detected on this device.';
    list.appendChild(e);
  }else{
    STATE.browsers.forEach(function(b){list.appendChild(rowEl(b));});
  }
  updateHeader();
}

// ---- Bookmarks pane (the user's own saved bookmarks) ----

function bmRowEl(b){
  var r=document.createElement('div');r.className='row';
  r.onclick=function(){openBookmark(b.url);};
  var f=document.createElement('span');f.className='flag';
  f.innerHTML=LOGO.globe;  // constant string only
  var m=document.createElement('div');m.className='meta';
  var n=document.createElement('div');n.className='name';
  n.textContent=b.title||b.host||b.url||'';
  var s=document.createElement('div');s.className='sub';
  s.textContent=b.host||'';
  m.appendChild(n);m.appendChild(s);
  r.appendChild(f);r.appendChild(m);return r;
}

function renderBookmarks(){
  var list=el('bmlist');list.innerHTML='';
  if(!STATE.bookmarks.length){
    var e=document.createElement('div');e.className='empty';
    // Constant markup only; the click switches to the Import tab.
    e.innerHTML='No bookmarks yet — <span class="lnk" id="toimport">import some &rarr;</span>';
    list.appendChild(e);
    el('toimport').onclick=function(){setTab('import');};
    return;}
  var f=STATE.filter.toLowerCase();
  var items=f?STATE.bookmarks.filter(function(b){
    return (b.title||'').toLowerCase().indexOf(f)>=0 ||
           (b.host||'').toLowerCase().indexOf(f)>=0;}):STATE.bookmarks;
  if(!items.length){
    var e2=document.createElement('div');e2.className='empty';
    e2.textContent='No matches.';list.appendChild(e2);return;}
  items.forEach(function(b){list.appendChild(bmRowEl(b));});
}

function openBookmark(url){
  if(!url) return;
  // Fire-and-forget: the bubble stays open so the user can open several.
  sendWithPromise('openBookmark',url).then(function(){}).catch(function(){});
}

function loadBookmarks(){
  sendWithPromise('getBookmarks').then(function(r){
    STATE.bookmarks=(r&&r.bookmarks)?r.bookmarks:[];
    renderBookmarks();updateHeader();
  }).catch(function(){STATE.bookmarks=[];renderBookmarks();updateHeader();});
}

function rowEl(b){
  var installed=isInstalled(b);
  var r=document.createElement('div');
  r.className='row'+(installed?'':' dim')+((STATE.selected===b.id&&installed)?' active':'');
  if(installed){r.onclick=function(){selectAndImport(b.id);};}
  var f=document.createElement('span');f.className='flag';
  f.innerHTML=LOGO[b.id]||LOGO.globe;  // constant strings only
  var n=document.createElement('span');n.className='name';n.textContent=b.display_name;
  var c=document.createElement('span');c.className='chk';c.textContent='→';
  r.appendChild(f);r.appendChild(n);r.appendChild(c);return r;
}

// Compose the per-import summary from the resolved result (same wording as the
// settings import UI, trimmed for the compact popover).
function summary(r,withPw){
  if(!r||!r.success){
    if(r&&r.needs_full_disk_access)
      return 'Needs Full Disk Access (System Settings › Privacy & Security)';
    return (r&&r.error)?r.error:'Import failed';}
  var msg='✓ '+(r.bookmarks_imported||0)+' bookmarks';
  if(withPw){
    if(r.needs_full_disk_access) msg+=', 0 passwords — needs Full Disk Access';
    else if(r.passwords_supported===false) msg+=', passwords not supported here';
    else if(r.keychain_denied) msg+=', Keychain permission denied';
    else{msg+=', '+(r.passwords_imported||0)+' passwords';
      if(r.password_failures) msg+=' ('+r.password_failures+' skipped)';}}
  return msg;
}

function doImport(id){
  if(STATE.busy) return;  // one import at a time — avoid double-adding
  var withPw=STATE.includePasswords;
  STATE.busy=true;render();
  setStatus('Importing…');
  sendWithPromise('importFromBrowser',id,withPw).then(function(r){
    STATE.busy=false;render();setStatus(summary(r,withPw));
  }).catch(function(e){
    STATE.busy=false;render();setStatus((e&&e.error)?e.error:'Import failed');});
}

function selectAndImport(id){STATE.selected=id;render();doImport(id);}

function importPrimary(){
  var id=STATE.selected;
  if(!id){for(var i=0;i<STATE.browsers.length;i++){
    if(isInstalled(STATE.browsers[i])){id=STATE.browsers[i].id;break;}}}
  if(!id){setStatus('No browser to import from.');return;}
  STATE.selected=id;render();doImport(id);
}

function togglePw(){STATE.includePasswords=!STATE.includePasswords;render();}

// {phase,done,total} progress streamed between the bookmark and password write
// phases. textContent only, so backend-supplied text can't inject markup.
function onImportProgress(p){
  if(!p) return;
  var label=p.phase==='passwords'?'passwords':'bookmarks';
  var total=p.total||0;
  setStatus('Importing '+label+'… ('+(p.done||0)+(total?'/'+total:'')+')');
}

function loadBrowsers(){
  sendWithPromise('getImportableBrowsers').then(function(r){
    var all=(r&&r.browsers)?r.browsers:[];
    // Installed browsers first, preserving relative order otherwise.
    all=all.slice().sort(function(a,b){
      return (isInstalled(a)?0:1)-(isInstalled(b)?0:1);});
    STATE.browsers=all;
    STATE.selected='';
    for(var i=0;i<all.length;i++){
      if(isInstalled(all[i])){STATE.selected=all[i].id;break;}}
    render();  // render() -> updateHeader() refreshes the Import subline
  }).catch(function(){STATE.browsers=[];render();});
}

cr.addWebUIListener('import-progress',onImportProgress);
document.addEventListener('DOMContentLoaded',function(){
  setTab('bookmarks');           // default view = the user's own bookmarks
  var s=el('bmsearch');
  if(s) s.addEventListener('input',function(){
    STATE.filter=s.value||'';renderBookmarks();});
  loadBookmarks();               // primary: fill the Bookmarks list
  loadBrowsers();                // secondary: pre-populate the Import tab
});
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltImportUI::MoltImportUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(web_ui->GetWebContents()->GetBrowserContext(),
                              std::make_unique<MoltImportDataSource>());
  web_ui->AddMessageHandler(std::make_unique<MoltImportHandler>());
}

MoltImportUI::~MoltImportUI() = default;
