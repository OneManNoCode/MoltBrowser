// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_home_ui.h"

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

// Serves the MoltBrowser new-tab / home page as inline HTML. Local + static —
// no remote dependency — so the new tab loads instantly and privately. The
// search field and widget cards navigate via plain window.location (no IPC
// handler needed): a query goes to a private web search or straight to a URL,
// and the cards open the AI, MoltNet, and Import surfaces.
class MoltHomeDataSource : public content::URLDataSource {
 public:
  MoltHomeDataSource() = default;
  ~MoltHomeDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltHomeHost; }

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
<title>MoltSearch</title>
<style>
  :root{
    --ground:#06070c;
    --text:#f4f5fa;
    --muted:rgba(233,236,247,0.60);
    --faint:rgba(233,236,247,0.40);
    --brand:#ff5257;
    --brand-deep:#e0353b;
    --cloud:#a78bfa;
    --glass:rgba(255,255,255,0.055);
    --glass-hi:rgba(255,255,255,0.10);
    --edge:rgba(255,255,255,0.14);
    --edge-hi:rgba(255,255,255,0.26);
    --specular:rgba(255,255,255,0.28);
    --blur:blur(34px) saturate(180%);
    --font:-apple-system,"SF Pro Display","SF Pro Text",BlinkMacSystemFont,"Helvetica Neue",Arial,sans-serif;
    --mono:"SF Mono",ui-monospace,Menlo,monospace;
  }
  *{margin:0;padding:0;box-sizing:border-box}
  html,body{height:100%}
  body{font-family:var(--font);color:var(--text);background:var(--ground);
    -webkit-font-smoothing:antialiased;overflow-x:hidden;min-height:100vh}

  /* Ambient environment — vivid blurred orbs so the glass refracts. */
  .env{position:fixed;inset:0;z-index:0;overflow:hidden;pointer-events:none}
  .orb{position:absolute;border-radius:50%;filter:blur(90px);will-change:transform}
  .orb.a{width:60vw;height:60vw;left:-12vw;top:-16vw;opacity:.55;background:radial-gradient(circle at 35% 35%,#5b3a86,#241645 70%);animation:d1 30s ease-in-out infinite}
  .orb.b{width:52vw;height:52vw;right:-10vw;top:2vh;opacity:.5;background:radial-gradient(circle at 60% 40%,#7a2036,#3a0f22 72%);animation:d2 34s ease-in-out infinite}
  .orb.c{width:48vw;height:48vw;left:20vw;bottom:-18vw;opacity:.42;background:radial-gradient(circle at 40% 60%,#0f5f6e,#08303a 72%);animation:d3 38s ease-in-out infinite}
  .orb.d{width:38vw;height:38vw;right:18vw;bottom:6vh;opacity:.4;background:radial-gradient(circle at 50% 50%,#3a2a7a,#1a1140 74%);animation:d1 44s ease-in-out infinite reverse}
  @keyframes d1{0%,100%{transform:translate(0,0)}50%{transform:translate(4vw,3vh)}}
  @keyframes d2{0%,100%{transform:translate(0,0)}50%{transform:translate(-3vw,4vh)}}
  @keyframes d3{0%,100%{transform:translate(0,0)}50%{transform:translate(3vw,-3vh)}}

  .wrap{position:relative;z-index:1;min-height:100vh;display:flex;flex-direction:column;
    align-items:center;justify-content:center;padding:40px 24px;gap:30px}

  .brand{text-align:center;display:flex;flex-direction:column;align-items:center;gap:14px}
  .logo{font-size:clamp(40px,7vw,72px);font-weight:800;letter-spacing:-0.035em;line-height:1}
  .logo .m{color:var(--brand)}
  .logo .s{color:var(--text)}
  .logo .beta{font-size:.28em;font-weight:700;letter-spacing:.18em;color:var(--faint);vertical-align:super;margin-left:.4em}
  .kick{font-size:11.5px;letter-spacing:.34em;text-transform:uppercase;color:var(--brand);font-weight:600}

  /* Glass search */
  .search{width:min(620px,92vw);display:flex;align-items:center;gap:12px;height:62px;
    padding:0 8px 0 22px;border-radius:20px;cursor:text;
    background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
    border:1px solid var(--edge);
    box-shadow:0 30px 70px -22px rgba(0,0,0,.7),inset 0 1px 0 var(--specular);
    transition:border-color .2s,box-shadow .2s}
  .search:focus-within{border-color:var(--edge-hi);
    box-shadow:0 30px 70px -20px rgba(0,0,0,.7),inset 0 1px 0 var(--specular),0 0 0 4px rgba(255,82,87,.14)}
  .search input{flex:1;background:transparent;border:none;outline:none;color:var(--text);
    font-family:var(--font);font-size:16px}
  .search input::placeholder{color:var(--faint)}
  .search .go{flex:0 0 auto;width:44px;height:44px;border-radius:14px;border:none;cursor:pointer;
    display:grid;place-items:center;color:#fff;font-size:19px;
    background:linear-gradient(180deg,var(--brand),var(--brand-deep));
    box-shadow:0 10px 24px -8px rgba(255,82,87,.6);transition:transform .15s,opacity .15s}
  .search .go:hover{transform:translateY(-1px)}

  /* Widget cards */
  .cards{display:flex;gap:14px;flex-wrap:wrap;justify-content:center;width:min(620px,92vw)}
  .card{flex:1;min-width:170px;padding:16px 18px;border-radius:18px;cursor:pointer;text-align:left;
    background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
    border:1px solid var(--edge);
    box-shadow:0 22px 54px -20px rgba(0,0,0,.65),inset 0 1px 0 var(--specular);
    transition:transform .18s,border-color .18s,background .18s}
  .card:hover{transform:translateY(-4px);border-color:var(--edge-hi);background:var(--glass-hi)}
  .card .t{display:flex;align-items:center;gap:10px;font-size:14px;font-weight:650}
  .card .ic{width:30px;height:30px;border-radius:10px;display:grid;place-items:center;font-size:15px;flex:0 0 auto}
  .card.ai .ic{background:rgba(167,139,250,.22);box-shadow:inset 0 0 0 1px rgba(167,139,250,.3)}
  .card.net .ic{background:rgba(95,227,161,.18);box-shadow:inset 0 0 0 1px rgba(95,227,161,.28)}
  .card.imp .ic{background:rgba(255,82,87,.2);box-shadow:inset 0 0 0 1px rgba(255,82,87,.3)}
  .card .d{font-size:12px;color:var(--faint);margin-top:8px;line-height:1.5}

  @media (prefers-reduced-motion: reduce){.orb{animation:none}.card:hover,.search .go:hover{transform:none}}
</style>
</head>
<body>
<div class="env"><div class="orb a"></div><div class="orb b"></div><div class="orb c"></div><div class="orb d"></div></div>

<div class="wrap">
  <div class="brand">
    <div class="logo"><span class="m">Molt</span><span class="s">Search</span><span class="beta">BETA</span></div>
    <div class="kick">Private &middot; On-device &middot; Agentic</div>
  </div>

  <form class="search" id="searchForm" onsubmit="return runSearch(event)">
    <input id="q" type="text" autocomplete="off" spellcheck="false"
           placeholder="Search the web, or ask any model&hellip;" autofocus>
    <button class="go" type="submit" aria-label="Search">&rarr;</button>
  </form>

  <div class="cards">
    <div class="card ai" onclick="go('molt://ai/')">
      <div class="t"><span class="ic">&#10022;</span> Ask AI</div>
      <div class="d">Chat with local &amp; cloud models, grounded in the page.</div>
    </div>
    <div class="card net" onclick="go('molt://ai-settings/?section=providers')">
      <div class="t"><span class="ic">&#128737;</span> MoltNet</div>
      <div class="d">Private routing with a pick-your-country exit, powered by Tor.</div>
    </div>
    <div class="card imp" onclick="go('molt://ai-settings/?section=import')">
      <div class="t"><span class="ic">&#11015;</span> Import</div>
      <div class="d">Bring bookmarks &amp; passwords from any browser.</div>
    </div>
  </div>
</div>

<script>
function go(u){ window.location.href = u; }
// Looks-like-a-URL heuristic: has a scheme, or a dot with no spaces (a host).
function isUrl(s){
  if(/^[a-z][a-z0-9+.-]*:\/\//i.test(s)) return true;
  if(/^[^\s]+\.[^\s]{2,}([\/?#].*)?$/.test(s) && !s.includes(' ')) return true;
  return false;
}
function runSearch(e){
  e.preventDefault();
  var q = (document.getElementById('q').value || '').trim();
  if(!q) return false;
  if(isUrl(q)){
    window.location.href = /^[a-z][a-z0-9+.-]*:\/\//i.test(q) ? q : ('https://' + q);
  } else {
    // Private web search (no Google).
    window.location.href = 'https://duckduckgo.com/?q=' + encodeURIComponent(q);
  }
  return false;
}
</script>
</body>
</html>)HTML";

    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(html)));
  }

  bool ShouldReplaceExistingSource() override { return true; }
};

}  // namespace

MoltHomeUI::MoltHomeUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<MoltHomeDataSource>());
}

MoltHomeUI::~MoltHomeUI() = default;
