// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_net_ui.h"

#include <optional>
#include <string>

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/ui/webui/molt_ai/molt_net_handler.h"
#include "chrome/browser/ui/webui/molt_ai/molt_webui_csp.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

constexpr char kNetPageJs[] = R"JS(// MoltNet popover script, served as /app.js under the strict molt WebUI
// CSP (molt_webui_csp.h) after the shared moltActions() prologue. The page
// has no Trusted Types policy: it never assigns HTML strings, everything
// is built with DOM APIs.
// chrome.send promise bridge (same shape as the AI chat UI).
var _id=0,_pend={};
function sendWithPromise(m){var a=[].slice.call(arguments,1);var id=m+'_'+(++_id);
  return new Promise(function(res,rej){_pend[id]={res:res,rej:rej};chrome.send(m,[id].concat(a));});}
window.cr=window.cr||{};
cr.webUIResponse=function(id,ok,r){var c=_pend[id];if(c){delete _pend[id];ok?c.res(r):c.rej(r);}};

function flag(cc){ if(!cc||cc.length!==2) return '🌐';
  return String.fromCodePoint.apply(null,[cc.charCodeAt(0),cc.charCodeAt(1)].map(function(x){return 0x1F1E6+(x|32)-97;})); }

// |routing| is about THIS window: a MoltNet window is routed via Tor, a
// regular window never is (MoltNet browsing happens in its own windows).
var STATE={running:false,starting:false,busy:false,selected:'',mode:'direct',
  routing:'not_routed',inMolt:false,proxyExt:false,error:'',countries:[],
  fonts:''};

function statusText(){
  if(STATE.busy||STATE.starting) return 'Connecting to Tor…';
  if(STATE.routing==='tor') return 'Private via Tor';
  if(STATE.routing==='blocked') return 'Tor stopped — window blocked';
  if(STATE.routing==='overridden') return 'Proxy overridden — not protected';
  return STATE.running?'This window: not routed':'Off · not routed';
}
function whereText(){
  if(STATE.inMolt){
    if(STATE.routing==='tor') return 'This MoltNet window sends all its traffic through Tor. Its cookies and site data are discarded when the last MoltNet window closes.';
    if(STATE.routing==='blocked') return 'Tor is not running, so this window can\'t load pages. It never falls back to your direct connection.';
    return 'A policy, extension or command-line flag controls the proxy here, so MoltNet can\'t guarantee Tor routing. Close this window.';
  }
  var t='This window uses your normal connection. MoltNet browsing happens in separate Tor windows with their own cookies.';
  if(STATE.proxyExt) t+=' (This window\'s proxy is set by a policy or extension.)';
  return t;
}
// A build without MoltNet's bundled fonts still works, but its MoltNet
// windows don't look like every other MoltNet user's: say so, without
// blocking anything.
function fontsText(){
  if(STATE.fonts==='missing') return 'This build of MoltBrowser has no bundled fonts, so MoltNet windows use a fixed list of your system\'s fonts. They look less like other MoltNet users\' windows than with the fonts every release includes.';
  if(STATE.fonts==='damaged') return 'MoltBrowser\'s bundled fonts are damaged, so MoltNet pages show no text. Reinstall MoltBrowser to repair them.';
  return '';
}
function render(){
  var d=document.getElementById('dot'), st=document.getElementById('st'),
      sub=document.getElementById('sub');
  var bad=(STATE.routing==='blocked'||STATE.routing==='overridden');
  d.className='dot'+((STATE.busy||STATE.starting||bad)?' busy':(STATE.routing==='tor'?' on':''));
  st.textContent=statusText();
  sub.textContent=STATE.selected?flag(STATE.selected)+' '+STATE.selected.toUpperCase():'🌐 AUTO';
  document.getElementById('where').textContent=whereText();
  document.getElementById('err').textContent=STATE.error||'';
  document.getElementById('fonts').textContent=fontsText();
  var on=STATE.running||STATE.starting||STATE.busy;
  var modes=document.getElementById('modes').children;
  for(var i=0;i<modes.length;i++){
    var m=modes[i].getAttribute('data-mode');
    modes[i].className='mode'+(((m==='direct')!==on)?' active':'');
  }
  var list=document.getElementById('list'); list.textContent='';
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
function apply(s){ if(!s) return;
  STATE.running=!!s.running; STATE.starting=!!s.starting; STATE.selected=s.selected||'';
  if(s.mode) STATE.mode=s.mode; STATE.routing=s.routing||'not_routed';
  STATE.inMolt=!!s.in_moltnet_window; STATE.proxyExt=!!s.proxy_externally_controlled;
  STATE.fonts=s.fonts||'';
  STATE.error=s.error||''; STATE.busy=false; render(); renderRoute(); }
function loadCountries(){ sendWithPromise('moltnet.getExitCountries').then(function(r){
  STATE.countries=r.available||[]; if(r.selected!=null) STATE.selected=r.selected; render(); }); }
function refresh(){ sendWithPromise('moltnet.getStatus').then(apply); }
function busy(){ STATE.busy=true; STATE.error=''; render(); }
// The mode, the exit country (and moltnet.toggle) are settings: the browser
// takes the click for each, so every click sends exactly one of them. "On"
// opens a MoltNet window, as the New Tor window button does.
function setMode(m){ if(m!=='direct') busy();
  sendWithPromise('moltnet.setMode',m==='direct'?'direct':'multi_hop').then(apply); }
function openWindow(){ busy(); sendWithPromise('moltnet.openWindow').then(apply); }
function setCountry(cc){ STATE.selected=cc; render();
  sendWithPromise('moltnet.setExitCountry',cc).then(apply); }
function newIdentity(){ busy(); sendWithPromise('moltnet.newIdentity').then(apply); }
function go(u){ window.location.href=u; }

// ---- Route transparency panel --------------------------------------------
// Relay data comes from Tor's own control port. The user's real IP is never
// looked up or shown.
function countryLabel(cc){ if(!cc) return ''; cc=cc.toUpperCase();
  for(var i=0;i<STATE.countries.length;i++){ if(STATE.countries[i].code.toUpperCase()===cc) return STATE.countries[i].name; }
  return cc; }
function hopNode(fl,role,ipText,cls,btnLabel){
  var d=document.createElement('div'); d.className='hop'+(cls?' '+cls:'');
  var f=document.createElement('span'); f.className='hflag'; f.textContent=fl;
  var m=document.createElement('div'); m.className='hmain';
  var r=document.createElement('div'); r.className='hrole'; r.textContent=role;
  var ip=document.createElement('div'); ip.className='hip'; ip.textContent=ipText;
  m.appendChild(r); m.appendChild(ip);
  d.appendChild(f); d.appendChild(m);
  var btn=null;
  if(btnLabel){ btn=document.createElement('div'); btn.className='hbtn'; btn.textContent=btnLabel; d.appendChild(btn); }
  return {node:d, ipEl:ip, btn:btn};
}
function renderRoute(){
  var wrap=document.getElementById('routeWrap'); if(!wrap) return;
  if(!STATE.running){ wrap.textContent=''; return; }
  sendWithPromise('moltnet.getCircuit').then(function(r){
    if(!r||!r.connected){ wrap.textContent=''; return; }
    wrap.textContent='';
    var sec=document.createElement('div'); sec.className='sec'; sec.textContent='Current Tor route'; wrap.appendChild(sec);
    var route=document.createElement('div'); route.className='route';
    var hops=r.hops||[];
    hops.forEach(function(h,i){
      if(i>0){ var line=document.createElement('div'); line.className='hline'; route.appendChild(line); }
      var role=h.role==='guard'?'Guard':(h.role==='exit'?'Exit':'Middle');
      var isExit=h.role==='exit';
      var ipText=(h.ip||'—')+(isExit?' · websites see this IP':'');
      var cname=h.country_name||countryLabel(h.country);
      var canVerify=isExit&&STATE.inMolt&&STATE.routing==='tor';
      var hn=hopNode(flag((h.country||'').toLowerCase()),
        role+' · '+cname, ipText, isExit?'exit':'', canVerify?'Verify':'');
      if(hn.btn) hn.btn.onclick=function(){ verifyExit(hn); };
      route.appendChild(hn.node);
    });
    wrap.appendChild(route);
    var note=document.createElement('div'); note.className='note';
    note.textContent='No single relay sees both who you are and the site you visit. Sites can see they are being visited over Tor.';
    wrap.appendChild(note);
    var ev=document.createElement('div'); ev.className='note evlink';
    ev.textContent='⧉ Copy route evidence';
    ev.onclick=function(){ copyEvidence(r, ev); };
    wrap.appendChild(ev);
  });
}
// Export the current path as verifiable text: each relay's fingerprint can be
// looked up in the PUBLIC Tor consensus (metrics.torproject.org) to check
// it's a real relay in the country/role shown — independent of MoltBrowser.
// Contains only public relay data; never the user's own IP.
function copyEvidence(r, el){
  var t='MoltNet route evidence — '+new Date().toISOString()+'\n\n';
  (r.hops||[]).forEach(function(h){
    var role=h.role.charAt(0).toUpperCase()+h.role.slice(1);
    t+=role+'  ·  '+(h.country_name||h.country||'?')+'  ·  '+(h.ip||'?')+
       '\n    relay: '+(h.nickname||'?')+'   fingerprint: '+(h.fingerprint||'?')+'\n';
  });
  t+='\nVerify each relay is a real, published Tor relay (independent of MoltBrowser):\n'+
     '  https://metrics.torproject.org/rs.html#search/<fingerprint>\n'+
     '  https://onionoo.torproject.org/details?lookup=<fingerprint>\n'+
     'From a MoltNet window, confirm the exit IP websites see + that traffic is on Tor:\n'+
     '  https://check.torproject.org/api/ip\n';
  var done=function(){ var o=el.textContent; el.textContent='✓ Copied — paste anywhere'; setTimeout(function(){el.textContent=o;},1800); };
  if(navigator.clipboard&&navigator.clipboard.writeText){ navigator.clipboard.writeText(t).then(done, done); }
  else { done(); }
}
function verifyExit(hn){
  if(hn.btn) hn.btn.textContent='…';
  sendWithPromise('moltnet.verifyExit').then(function(r){
    if(r&&r.ok){
      hn.ipEl.textContent=(r.ip||'?')+(r.is_tor?' · Tor confirmed ✓':' · NOT Tor');
      hn.ipEl.className=r.is_tor?'hip reveal':'hip';
      if(hn.btn) hn.btn.remove();
    } else if(hn.btn){ hn.btn.textContent='Verify'; }
  });
}

// Static controls (no inline handlers under the page CSP).
moltActions({
  modeOff: function() { setMode('direct'); },
  modeOn: function() { setMode('multi_hop'); },
  openWindow: function() { openWindow(); },
  newIdentity: function() { newIdentity(); },
  openSettings: function() { go('molt://ai-settings/'); },
});

// app.js loads at the end of <body>, so the DOM is ready.
loadCountries(); refresh();
)JS";

// Serves the MoltNet (Tor) control popover (HTML + /app.js). The page talks to
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

  std::string GetMimeType(const GURL& url) override {
    return molt_webui::MimeTypeFor(url);
  }

  std::string GetContentSecurityPolicy(
      const network::mojom::CSPDirectiveName directive) override {
    // Strict molt WebUI CSP. The page never writes HTML strings, so no
    // Trusted Types policy may be created.
    if (std::optional<std::string> csp =
            molt_webui::PageContentSecurityPolicy(
                directive, molt_webui::TrustedTypesMode::kNoPolicies)) {
      return *csp;
    }
    return content::URLDataSource::GetContentSecurityPolicy(directive);
  }

  void StartDataRequest(
      const GURL& url,
      const content::WebContents::Getter& wc_getter,
      content::URLDataSource::GotDataCallback callback) override {
    if (molt_webui::IsAppJsRequest(url)) {
      std::move(callback).Run(base::MakeRefCounted<base::RefCountedString>(
          std::string(molt_webui::kMoltActionsJs) + kNetPageJs));
      return;
    }
    const std::string html = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MoltNet — Private routing</title>
<style>
  :root{
    --text:#f4f5fa;
    --muted:rgba(233,236,247,0.64);
    --faint:rgba(233,236,247,0.42);
    --accent:#ff5257;
    --accent-deep:#e0353b;
    --ok:#5fe3a1;
    --frost:rgba(16,18,27,0.66);
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
  .title{font-size:.875rem;font-weight:700;letter-spacing:-.01em}
  .st{font-size:.6875rem;color:var(--muted);margin-top:1px}
  .brand{margin-left:auto;font-size:.625rem;font-weight:700;letter-spacing:.14em;color:var(--faint)}

  .toggle-row{padding:2px 18px 14px;display:flex;align-items:center;gap:12px;flex:0 0 auto}
  .toggle-row .lbl{font-size:.78125rem;color:var(--muted)}
  .toggle-row .sub{font-size:.6875rem;color:var(--faint);margin-top:1px}
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
  .mode .mname{font-size:.71875rem;font-weight:650;color:var(--text)}
  .mode .mdesc{font-size:.5625rem;letter-spacing:.02em;color:var(--faint);margin-top:2px;text-transform:uppercase}

  .divider{height:1px;background:var(--edge);margin:0 14px;flex:0 0 auto}
  .sec{font-size:.625rem;letter-spacing:.16em;text-transform:uppercase;color:var(--faint);
    padding:13px 18px 6px;flex:0 0 auto}

  .list{padding:0}
  .row{display:flex;align-items:center;gap:11px;padding:9px 12px;border-radius:11px;cursor:pointer;
    border:1px solid transparent;transition:background .14s,border-color .14s}
  .row:hover{background:rgba(255,255,255,.06);border-color:var(--edge)}
  .row .flag{font-size:1.0625rem;width:22px;text-align:center;flex:0 0 auto}
  .row .name{font-size:.8125rem;flex:1}
  .row .chk{color:var(--ok);font-size:.875rem;opacity:0;flex:0 0 auto;transition:opacity .14s}
  .row.active{background:rgba(95,227,161,.10);border-color:rgba(95,227,161,.30)}
  .row.active .chk{opacity:1}

  .foot{flex:0 0 auto;display:flex;gap:8px;padding:10px 14px 14px}
  .act{flex:1;padding:9px 4px;border-radius:11px;cursor:pointer;text-align:center;
    font-size:.6875rem;font-weight:600;background:rgba(255,255,255,.05);
    border:1px solid var(--edge);color:var(--muted);transition:.15s;white-space:nowrap}
  .act:hover{background:rgba(255,255,255,.11);border-color:var(--edge-hi);color:var(--text)}

  /* Your-route transparency panel */
  .scroll{flex:1 1 auto;overflow-y:auto;padding:0 8px 4px}
  .scroll::-webkit-scrollbar{width:8px}
  .scroll::-webkit-scrollbar-thumb{background:rgba(255,255,255,.14);border-radius:4px;border:2px solid transparent;background-clip:content-box}
  .route{padding:2px 4px 6px}
  .hop{display:flex;align-items:center;gap:11px;padding:8px 10px;border-radius:11px;border:1px solid transparent}
  .hop.dev{background:rgba(255,255,255,.04);border-color:var(--edge)}
  .hop.exit{background:rgba(255,82,87,.09);border-color:rgba(255,82,87,.24)}
  .hop .hflag{font-size:1rem;width:20px;text-align:center;flex:0 0 auto}
  .hop .hmain{flex:1;min-width:0}
  .hop .hrole{font-size:.78125rem;color:var(--text);font-weight:600}
  .hop .hip{font-size:.65625rem;color:var(--faint);font-family:var(--mono,ui-monospace,monospace);margin-top:1px;overflow-wrap:anywhere;word-break:normal}
  .hop .hip.reveal{color:var(--ok)}
  .hop .hbtn{font-size:.65625rem;padding:3px 10px;border-radius:8px;border:1px solid var(--edge-hi);background:transparent;color:var(--text);cursor:pointer;flex:0 0 auto;white-space:nowrap}
  .hop .hbtn:hover{background:rgba(255,255,255,.10)}
  .hline{width:2px;height:9px;background:rgba(255,255,255,.22);margin-left:19px}
  .note{font-size:.59375rem;color:var(--faint);padding:2px 8px 6px;line-height:1.4}
  .note.warn{color:#ffb4b4;padding-top:0}
  .note.warn:empty{display:none}
  .note.fonts{padding:0 18px 8px;font-size:.625rem;color:var(--muted)}
  .note.fonts:empty{display:none}
  #where{padding:0 18px 8px;font-size:.65625rem}
  .evlink{cursor:pointer;color:var(--muted)}
  .evlink:hover{color:var(--text)}
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
    <div class="sec">Tor routing</div>
    <div class="modes" id="modes">
      <div class="mode" data-mode="direct" data-on-click="modeOff">
        <div class="mname">Off</div><div class="mdesc">Tor stopped</div></div>
      <div class="mode" data-mode="multi_hop" data-on-click="modeOn">
        <div class="mname">On</div><div class="mdesc">Tor&nbsp;&middot;&nbsp;own&nbsp;window</div></div>
    </div>
    <div class="note" id="where"></div>
    <div class="note warn" id="err"></div>
    <div class="note fonts" id="fonts" role="note"></div>
    <div class="divider"></div>
    <div class="scroll">
      <div id="routeWrap"></div>
      <div class="sec">Exit country</div>
      <div class="note">Pinning a country shrinks the set of exits you blend in with, which reduces anonymity. The choice resets when MoltBrowser restarts.</div>
      <div class="list" id="list"></div>
    </div>
    <div class="foot">
      <div class="act" id="openWindowBtn" data-on-click="openWindow">&#129477; New&nbsp;Tor&nbsp;window</div>
      <div class="act" id="newIdentityBtn" data-on-click="newIdentity">&#128260; New&nbsp;identity</div>
      <div class="act" id="settingsBtn" data-on-click="openSettings">&#9881;&#65039; Settings</div>
    </div>
  </div>
</div>
<script src="app.js"></script>
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
