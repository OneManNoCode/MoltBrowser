// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_net_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/ui/webui/molt_ai/molt_net_handler.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

// Serves the MoltNet (Tor) control popover as inline HTML. The page talks to
// C++ over chrome.send (see MoltNetHandler) to drive the same TorManager the
// toolbar globe menu uses. Sized for a compact bubble (~344x476); it also
// renders as a full tab for verification. Real glass is guaranteed by in-page
// ambient orbs behind a backdrop-filter frost layer — so the frosting reads
// even if the OS won't composite backdrop across the separate bubble widget.
class MoltNetDataSource : public content::URLDataSource {
 public:
  MoltNetDataSource() = default;
  ~MoltNetDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltNetHost; }

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
<title>MoltNet</title>
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
  .panel{position:relative;width:320px;height:452px;max-width:calc(100vw - 20px);
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
  .brand{margin-left:auto;font-size:10px;font-weight:700;letter-spacing:.14em;color:var(--faint)}

  .toggle-row{padding:2px 18px 14px;display:flex;align-items:center;gap:12px;flex:0 0 auto}
  .toggle-row .lbl{font-size:12.5px;color:var(--muted)}
  .toggle-row .sub{font-size:11px;color:var(--faint);margin-top:1px}
  .sw{margin-left:auto;width:44px;height:25px;border-radius:13px;background:rgba(255,255,255,.12);
    border:1px solid var(--edge);cursor:pointer;position:relative;transition:.2s;flex:0 0 auto}
  .sw.on{background:linear-gradient(180deg,#ff676c,var(--accent-deep));border-color:transparent}
  .sw .knob{position:absolute;top:2px;left:2px;width:19px;height:19px;border-radius:50%;
    background:#fff;transition:.2s;box-shadow:0 1px 3px rgba(0,0,0,.35)}
  .sw.on .knob{transform:translateX(19px)}

  /* Routing-mode segmented control */
  .modes{display:flex;gap:6px;padding:2px 14px 8px;flex:0 0 auto}
  .mode{flex:1;padding:8px 4px 7px;border-radius:11px;cursor:pointer;text-align:center;
    background:rgba(255,255,255,.05);border:1px solid var(--edge);transition:.15s}
  .mode:hover{background:rgba(255,255,255,.10);border-color:var(--edge-hi)}
  .mode.active{background:linear-gradient(180deg,rgba(255,103,108,.30),rgba(224,53,59,.20));
    border-color:rgba(255,82,87,.55);box-shadow:0 0 0 1px rgba(255,82,87,.18) inset}
  .mode .mname{font-size:11.5px;font-weight:650;color:var(--text)}
  .mode .mdesc{font-size:9px;letter-spacing:.02em;color:var(--faint);margin-top:2px;text-transform:uppercase}

  .divider{height:1px;background:var(--edge);margin:0 14px;flex:0 0 auto}
  .sec{font-size:10px;letter-spacing:.16em;text-transform:uppercase;color:var(--faint);
    padding:13px 18px 6px;flex:0 0 auto}

  .list{flex:1 1 auto;overflow-y:auto;padding:0 8px 4px}
  .list::-webkit-scrollbar{width:8px}
  .list::-webkit-scrollbar-thumb{background:rgba(255,255,255,.14);border-radius:4px;border:2px solid transparent;background-clip:content-box}
  .row{display:flex;align-items:center;gap:11px;padding:9px 12px;border-radius:11px;cursor:pointer;
    border:1px solid transparent;transition:background .14s,border-color .14s}
  .row:hover{background:rgba(255,255,255,.06);border-color:var(--edge)}
  .row .flag{font-size:17px;width:22px;text-align:center;flex:0 0 auto}
  .row .name{font-size:13px;flex:1}
  .row .chk{color:var(--ok);font-size:14px;opacity:0;flex:0 0 auto;transition:opacity .14s}
  .row.active{background:rgba(95,227,161,.10);border-color:rgba(95,227,161,.30)}
  .row.active .chk{opacity:1}

  .foot{flex:0 0 auto;display:flex;gap:8px;padding:10px 14px 14px}
  .act{flex:1;padding:9px 4px;border-radius:11px;cursor:pointer;text-align:center;
    font-size:11px;font-weight:600;background:rgba(255,255,255,.05);
    border:1px solid var(--edge);color:var(--muted);transition:.15s;white-space:nowrap}
  .act:hover{background:rgba(255,255,255,.11);border-color:var(--edge-hi);color:var(--text)}

  @media (prefers-reduced-motion: reduce){*{transition:none!important}}
</style>
</head>
<body>
<div class="panel">
  <div class="env"><div class="orb a"></div><div class="orb b"></div><div class="orb c"></div></div>
  <div class="frost">
    <div class="hdr">
      <span class="dot" id="dot"></span>
      <div><div class="title">MoltNet</div><div class="st" id="st">Checking&hellip;</div></div>
      <span class="brand" id="sub"></span>
    </div>
    <div class="sec">Routing mode</div>
    <div class="modes" id="modes">
      <div class="mode" data-mode="direct" onclick="setMode('direct')">
        <div class="mname">Direct</div><div class="mdesc">No&nbsp;routing</div></div>
      <div class="mode" data-mode="proxy" onclick="setMode('proxy')">
        <div class="mname">Single&nbsp;proxy</div><div class="mdesc">1&nbsp;hop</div></div>
      <div class="mode" data-mode="multi_hop" onclick="setMode('multi_hop')">
        <div class="mname">Multi&#8209;hop</div><div class="mdesc">Tor</div></div>
    </div>
    <div class="divider"></div>
    <div class="sec">Exit country</div>
    <div class="list" id="list"></div>
    <div class="foot">
      <div class="act" onclick="newIdentity()">&#128260; New&nbsp;identity</div>
      <div class="act" onclick="go('molt://ai-settings/?section=import')">&#128229; Import</div>
      <div class="act" onclick="go('molt://ai-settings/')">&#9881;&#65039; Settings</div>
    </div>
  </div>
</div>
<script>
// chrome.send promise bridge (same shape as the AI chat UI).
var _id=0,_pend={};
function sendWithPromise(m){var a=[].slice.call(arguments,1);var id=m+'_'+(++_id);
  return new Promise(function(res,rej){_pend[id]={res:res,rej:rej};chrome.send(m,[id].concat(a));});}
window.cr=window.cr||{};
cr.webUIResponse=function(id,ok,r){var c=_pend[id];if(c){delete _pend[id];ok?c.res(r):c.rej(r);}};

function flag(cc){ if(!cc||cc.length!==2) return '🌐';
  return String.fromCodePoint.apply(null,[cc.charCodeAt(0),cc.charCodeAt(1)].map(function(x){return 0x1F1E6+(x|32)-97;})); }

var STATE={running:false,busy:false,selected:'',mode:'direct',countries:[]};

function render(){
  var d=document.getElementById('dot'), st=document.getElementById('st'),
      sub=document.getElementById('sub');
  d.className='dot'+(STATE.busy?' busy':(STATE.running?' on':''));
  st.textContent=STATE.busy?'Connecting…':(STATE.running?'Connected':'Disconnected');
  // Always show the chosen exit country's flag (even when disconnected) so
  // picking a country gives immediate feedback.
  sub.textContent=STATE.selected?flag(STATE.selected)+' '+STATE.selected.toUpperCase():'🌐 AUTO';
  // Highlight the active routing mode.
  var modes=document.getElementById('modes').children;
  for(var i=0;i<modes.length;i++){
    modes[i].className='mode'+(modes[i].getAttribute('data-mode')===STATE.mode?' active':'');
  }
  var list=document.getElementById('list'); list.innerHTML='';
  list.appendChild(rowEl('','🌐','Auto (recommended)'));
  STATE.countries.forEach(function(c){ list.appendChild(rowEl(c.code,flag(c.code),c.name)); });
}
function rowEl(code,fl,name){
  var r=document.createElement('div');
  r.className='row'+((STATE.selected||'')===code?' active':'');
  r.onclick=function(){ setCountry(code); };
  var f=document.createElement('span'); f.className='flag'; f.textContent=fl;
  var n=document.createElement('span'); n.className='name'; n.textContent=name;
  var c=document.createElement('span'); c.className='chk'; c.textContent='✓';
  r.appendChild(f); r.appendChild(n); r.appendChild(c); return r;
}
function apply(s){ STATE.running=!!s.running; STATE.selected=s.selected||'';
  if(s.mode) STATE.mode=s.mode; STATE.busy=false; render(); }
function loadCountries(){ sendWithPromise('moltnet.getExitCountries').then(function(r){
  STATE.countries=r.available||[]; if(r.selected!=null) STATE.selected=r.selected; render(); }); }
function refresh(){ sendWithPromise('moltnet.getStatus').then(apply); }
function setMode(m){ STATE.mode=m; STATE.busy=(m!=='direct'&&!STATE.running); render();
  sendWithPromise('moltnet.setMode',m).then(apply); }
function setCountry(cc){ STATE.selected=cc; render();
  sendWithPromise('moltnet.setExitCountry',cc).then(apply); }
function newIdentity(){ sendWithPromise('moltnet.newIdentity').then(apply); }
function go(u){ window.location.href=u; }

document.addEventListener('DOMContentLoaded',function(){ loadCountries(); refresh(); });
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltNetUI::MoltNetUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltNetDataSource>());
  web_ui->AddMessageHandler(std::make_unique<MoltNetHandler>());
}

MoltNetUI::~MoltNetUI() = default;
