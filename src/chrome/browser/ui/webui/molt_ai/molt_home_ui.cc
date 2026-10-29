// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_home_ui.h"

#include <optional>
#include <string>

#include "base/memory/ref_counted_memory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_webui_csp.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

constexpr char kHomePageJs[] = R"JS(// MoltSearch new-tab page script. Served as /app.js under the strict
// molt WebUI CSP (molt_webui_csp.h): no inline handlers, and the page
// never writes HTML, so Trusted Types allow no policies at all.
(function() {
'use strict';
// Looks-like-a-URL heuristic: has a scheme, or a dot with no spaces (a host).
function isUrl(s) {
  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(s)) return true;
  if (/^[^\s]+\.[^\s]{2,}([\/?#].*)?$/.test(s) && !s.includes(' ')) return true;
  return false;
}
function runSearch(e) {
  e.preventDefault();
  var q = (document.getElementById('q').value || '').trim();
  if (!q) return;
  if (isUrl(q)) {
    // Only web URLs are opened directly from this privileged page; any
    // other scheme (javascript:, file:, data:...) is searched instead.
    var hasScheme = /^[a-z][a-z0-9+.-]*:\/\//i.test(q);
    if (!hasScheme || /^https?:\/\//i.test(q)) {
      window.location.href = hasScheme ? q : ('https://' + q);
      return;
    }
  }
  // Private web search (no Google).
  window.location.href = 'https://duckduckgo.com/?q=' + encodeURIComponent(q);
}
document.getElementById('searchForm').addEventListener('submit', runSearch);
})();
)JS";

// Serves the MoltBrowser new-tab / home page (HTML + /app.js). Local + static —
// no remote dependency — so the new tab loads instantly and privately. The
// search field and widget cards navigate via plain window.location (no IPC
// handler needed): a query goes to a private web search or straight to a URL,
// and the cards are links to the AI, MoltNet, and Import surfaces.
class MoltHomeDataSource : public content::URLDataSource {
 public:
  MoltHomeDataSource() = default;
  ~MoltHomeDataSource() override = default;

  std::string GetSource() override { return chrome::kChromeUIMoltHomeHost; }

  std::string GetMimeType(const GURL& url) override {
    return molt_webui::MimeTypeFor(url);
  }

  // Strict molt WebUI CSP; this page never writes HTML.
  std::string GetContentSecurityPolicy(
      const network::mojom::CSPDirectiveName directive) override {
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
          std::string(kHomePageJs)));
      return;
    }
    const std::string html = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>New Tab &ndash; MoltSearch</title>
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
    --focus:#9ecbff;
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
  .logo{font-size:clamp(2.5rem,7vw,4.5rem);font-weight:800;letter-spacing:-0.035em;line-height:1}
  .logo .m{color:var(--brand)}
  .logo .s{color:var(--text)}
  .logo .beta{font-size:.28em;font-weight:700;letter-spacing:.18em;color:var(--faint);vertical-align:super;margin-left:.1em}
  .kick{font-size:.71875rem;letter-spacing:.34em;text-transform:uppercase;color:var(--brand);font-weight:600}

  /* Glass search */
  .search{width:min(620px,92vw);display:flex;align-items:center;gap:12px;height:62px;
    padding:0 8px 0 22px;border-radius:20px;cursor:text;
    background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
    border:1px solid var(--edge);
    box-shadow:0 30px 70px -22px rgba(0,0,0,.7),inset 0 1px 0 var(--specular);
    transition:border-color .2s,box-shadow .2s}
  .search:has(#q:focus-visible){outline:3px solid var(--focus);outline-offset:2px}
  .search:focus-within{border-color:var(--edge-hi);
    box-shadow:0 30px 70px -20px rgba(0,0,0,.7),inset 0 1px 0 var(--specular),0 0 0 4px rgba(255,82,87,.14)}
  .search input{flex:1;background:transparent;border:none;outline:none;color:var(--text);
    font-family:var(--font);font-size:1rem}
  .search input::placeholder{color:var(--faint)}
  .search .go{flex:0 0 auto;width:44px;height:44px;border-radius:14px;border:none;cursor:pointer;
    display:grid;place-items:center;color:#fff;font-size:1.1875rem;
    background:linear-gradient(180deg,var(--brand),var(--brand-deep));
    box-shadow:0 10px 24px -8px rgba(255,82,87,.6);transition:transform .15s,opacity .15s}
  .search .go:hover{transform:translateY(-1px)}

  /* Widget cards */
  .cards{display:flex;gap:14px;flex-wrap:wrap;justify-content:center;width:min(620px,92vw)}
  .card{flex:1;min-width:170px;padding:16px 18px;border-radius:18px;cursor:pointer;text-align:left;
    display:block;color:inherit;text-decoration:none;
    background:var(--glass);-webkit-backdrop-filter:var(--blur);backdrop-filter:var(--blur);
    border:1px solid var(--edge);
    box-shadow:0 22px 54px -20px rgba(0,0,0,.65),inset 0 1px 0 var(--specular);
    transition:transform .18s,border-color .18s,background .18s}
  .card:hover{transform:translateY(-4px);border-color:var(--edge-hi);background:var(--glass-hi)}
  .card .t{display:flex;align-items:center;gap:10px;font-size:.875rem;font-weight:650}
  .card .ic{width:30px;height:30px;border-radius:10px;display:grid;place-items:center;font-size:.9375rem;flex:0 0 auto}
  .card.ai .ic{background:rgba(167,139,250,.22);box-shadow:inset 0 0 0 1px rgba(167,139,250,.3)}
  .card.net .ic{background:rgba(95,227,161,.18);box-shadow:inset 0 0 0 1px rgba(95,227,161,.28)}
  .card.imp .ic{background:rgba(255,82,87,.2);box-shadow:inset 0 0 0 1px rgba(255,82,87,.3)}
  .card .d{display:block;font-size:.75rem;color:var(--faint);margin-top:8px;line-height:1.5}

  :focus-visible{outline:3px solid var(--focus);outline-offset:2px}
  @media (prefers-reduced-motion: reduce){
    *,*::before,*::after{animation:none!important;transition:none!important;scroll-behavior:auto!important}
    .card:hover,.search .go:hover{transform:none}
  }
  /* High contrast: the glass, the orbs and the gradients are gone, so
     every control keeps a system-coloured edge. */
  @media (forced-colors: active){
    .env{display:none}
    .search,.card{border:1px solid CanvasText}
    .search .go{border:1px solid ButtonText}
    .card:hover{border-color:Highlight}
    :focus-visible,.search:has(#q:focus-visible){outline-color:Highlight}
  }
</style>
</head>
<body>
<div class="env" aria-hidden="true"><div class="orb a"></div><div class="orb b"></div><div class="orb c"></div><div class="orb d"></div></div>

<main class="wrap">
  <header class="brand">
    <h1 class="logo"><span class="m">Molt</span><span class="s">Search</span><span class="beta"> BETA</span></h1>
    <p class="kick">Private &middot; On-device &middot; Agentic</p>
  </header>

  <form class="search" id="searchForm" role="search">
    <input id="q" type="text" autocomplete="off" spellcheck="false"
           aria-label="Search the web or type a web address"
           placeholder="Search the web, or ask any model&hellip;" autofocus>
    <button class="go" type="submit" aria-label="Search">&rarr;</button>
  </form>

  <nav class="cards" aria-label="MoltBrowser">
    <a class="card ai" href="molt://ai/">
      <span class="t"><span class="ic" aria-hidden="true">&#10022;</span> Ask AI</span>
      <span class="d">Chat with local &amp; cloud models, grounded in the page.</span>
    </a>
    <a class="card net" href="molt://ai-settings/?section=providers">
      <span class="t"><span class="ic" aria-hidden="true">&#128737;</span> MoltNet</span>
      <span class="d">Private routing with a pick-your-country exit, powered by Tor.</span>
    </a>
    <a class="card imp" href="molt://ai-settings/?section=import">
      <span class="t"><span class="ic" aria-hidden="true">&#11015;</span> Import</span>
      <span class="d">Bring bookmarks &amp; passwords from any browser.</span>
    </a>
  </nav>
</main>

<script src="app.js"></script>
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
