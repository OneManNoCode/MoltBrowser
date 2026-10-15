// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/consent/consent_killer_tab_helper.h"

#include <string>

#include "base/functional/callback_helpers.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace consent {

namespace {

// Same isolated world the other MoltBrowser page helpers use.
constexpr int32_t kWorldId = 1;

// Hosts (and their subdomains) that serve a consent dialog as their own
// document: Sourcepoint / TrustArc message iframes and the full-page
// consent interstitials of Google, YouTube and Yahoo.
constexpr std::string_view kCmpHosts[] = {
    "privacy-mgmt.com", "sp-prod.net",         "consent-pref.trustarc.com",
    "consent.trustarc.com", "consent.google.com", "consent.youtube.com",
    "consent.yahoo.com", "guce.yahoo.com",
};

// The killer. Runs in an isolated world; `__moltConsentMode` (prepended by
// InjectInto) is "cmp-document" inside a known CMP document, where the
// text fallback may search the whole page.
constexpr char kKillerJS[] = R"JS(
(function() {
  'use strict';
  if (window.__moltConsentKillerInstalled) return;
  window.__moltConsentKillerInstalled = true;
  var cmpDocument = (typeof __moltConsentMode !== 'undefined' &&
                     __moltConsentMode === 'cmp-document');

  // Tier 1: vendor "reject" buttons. Only buttons that reject; never a
  // vendor's settings/customize button.
  var REJECT_SELECTORS = [
    '#onetrust-reject-all-handler',
    '.ot-pc-refuse-all-handler',
    '#CybotCookiebotDialogBodyButtonDecline',
    '#CybotCookiebotDialogBodyLevelButtonLevelOptinDeclineAll',
    '#didomi-notice-disagree-button',
    '.didomi-continue-without-agreeing',
    'button.cky-btn-reject',
    'button.osano-cm-denyAll',
    'button.sp_choice_type_REJECT_ALL',
    '[data-testid="uc-deny-all-button"]',
    '.cmplz-btn.cmplz-deny',
    '.fc-cta-do-not-consent',
    '#truste-consent-required',
    '.klaro .cm-btn-decline',
    'button[data-cookiefirst-action="reject"]',
    '#cmpwelcomebtnno',
    '.iubenda-cs-reject-btn',
    '#hs-eu-decline-button',
    '#cookiescript_reject',
  ];

  // Where the text fallback may look (outside CMP documents): the
  // containers of known consent-management frameworks, and only if they
  // talk about cookies/consent. Never a site's own dialogs (role=dialog,
  // aria-modal) or site-chosen ids such as #cookie-banner: a "Decline" or
  // "Reject" there could be anything (an invitation, a review, an order).
  var CONTAINER_SELECTORS = [
    '#onetrust-banner-sdk', '#onetrust-pc-sdk', '#CybotCookiebotDialog',
    '.qc-cmp2-container', '#qc-cmp2-ui', '#didomi-host', '#didomi-notice',
    '.cky-consent-container', '.osano-cm-dialog', '#truste-consent-track',
    '.truste_box_overlay', '#sp_message_container', '.cmplz-cookiebanner',
    '.fc-consent-root', '#cmpbox', '#usercentrics-root',
    '#usercentrics-cmp-ui', '.cc-window', '#cookie-law-info-bar',
    '.iubenda-cs-container', '#hs-eu-cookie-confirmation', '.klaro',
    '.cookiefirst-root', '#BorlabsCookieBox', '#cookiescript_injected',
  ].join(',');
  var CONSENT_TEXT = new RegExp(
      'cookie|consent|gdpr|privacy|personal data|tracking|partners|' +
      'datenschutz|einwilligung|confidentialit|consentement|privacidad|' +
      'riservatezza|toestemming', 'i');

  // Button labels that reject (exact match or prefix), lowercase.
  var REJECT_PHRASES = [
    'reject all', 'reject all cookies', 'reject non-essential',
    'reject optional', 'reject additional', 'decline all',
    'decline optional', 'decline non-essential', 'refuse all', 'deny all',
    'deny', 'decline', 'reject', 'only necessary', 'necessary only',
    'only essential', 'essential only', 'strictly necessary only',
    'use necessary cookies only', 'use essential cookies only',
    'accept only necessary', 'accept necessary', 'accept only essential',
    'accept essential cookies only', 'allow only necessary',
    'allow necessary only', 'continue without accepting',
    'continue without agreeing', 'do not accept', 'don’t accept',
    "don't accept", 'do not consent', 'i do not agree', 'disagree',
    'alle ablehnen', 'ablehnen', 'nur notwendige', 'nur erforderliche',
    'tout refuser', 'refuser', 'continuer sans accepter',
    'rechazar todo', 'rechazar', 'rifiuta tutto', 'rifiuta',
    'alles weigeren', 'weigeren', 'odrzuć wszystkie', 'avvisa alla',
  ];
  // Never click these, whatever else the label says.
  var CUSTOMIZE_TEXT = new RegExp(
      'custom|setting|preference|manage|option|more info|learn more|' +
      'details|partner|vendor|purpose|einstellung|anpassen|verwalten|' +
      'param[eè]tr|personnalis|configur|ajustes|impostazion|' +
      'instellingen|show', 'i');
  var ACCEPT_TEXT = new RegExp(
      '(^|\\W)(accept|agree|allow|ok|got it|akzeptieren|zustimmen|' +
      'accepter|aceptar|accetta)(\\W|$)', 'i');

  var MAX_RUNS = 50;
  var MAX_MS = 15000;
  var MAX_ELEMENTS = 6000;
  var BUTTONS = 'button, [role="button"], input[type="button"], ' +
                'input[type="submit"]';

  function label(el) {
    var t = el.innerText || el.textContent || el.value ||
            el.getAttribute('aria-label') || '';
    return String(t).replace(/\s+/g, ' ').trim().toLowerCase();
  }
  function isReject(t) {
    for (var i = 0; i < REJECT_PHRASES.length; i++) {
      var p = REJECT_PHRASES[i];
      if (t === p || t.indexOf(p + ' ') === 0 || t.indexOf(p + ',') === 0) {
        return true;
      }
    }
    return false;
  }
  // A curated vendor button is clicked unless its label says customize or
  // accept (vendors occasionally repurpose ids).
  function safeVendorButton(el) {
    var t = label(el);
    if (CUSTOMIZE_TEXT.test(t)) return false;
    return isReject(t) || !ACCEPT_TEXT.test(t);
  }
  function visible(el) {
    if (!el.isConnected || !el.getClientRects().length) return false;
    var s = window.getComputedStyle(el);
    return s.visibility !== 'hidden' && s.display !== 'none' &&
           !el.disabled;
  }
  // The document plus every open shadow root, bounded.
  function roots() {
    var out = [document];
    var seen = 0;
    for (var r = 0; r < out.length && seen < MAX_ELEMENTS; r++) {
      var walker = document.createTreeWalker(out[r], NodeFilter.SHOW_ELEMENT);
      for (var n = walker.nextNode(); n && seen < MAX_ELEMENTS;
           n = walker.nextNode()) {
        seen++;
        if (n.shadowRoot) out.push(n.shadowRoot);
      }
    }
    return out;
  }
  function click(el, how) {
    try {
      el.click();
    } catch (e) {
      return false;
    }
    window.__moltConsentKilled = how;
    return true;
  }
  function tryVendors(all) {
    for (var r = 0; r < all.length; r++) {
      for (var i = 0; i < REJECT_SELECTORS.length; i++) {
        var els = all[r].querySelectorAll(REJECT_SELECTORS[i]);
        for (var j = 0; j < els.length; j++) {
          if (visible(els[j]) && safeVendorButton(els[j]) &&
              click(els[j], REJECT_SELECTORS[i])) {
            return true;
          }
        }
      }
    }
    return false;
  }
  function tryText(all) {
    var containers = [];
    for (var r = 0; r < all.length; r++) {
      if (cmpDocument) {
        containers.push(all[r]);
        continue;
      }
      var found = all[r].querySelectorAll(CONTAINER_SELECTORS);
      for (var i = 0; i < found.length; i++) {
        var c = found[i];
        if (!visible(c)) continue;
        var text = (c.textContent || '').slice(0, 5000);
        if (CONSENT_TEXT.test(text)) containers.push(c);
      }
      // A shadow root that is itself a consent UI (e.g. Usercentrics).
      if (all[r] !== document && all[r].host && all[r].host.matches &&
          all[r].host.matches(CONTAINER_SELECTORS)) {
        containers.push(all[r]);
      }
    }
    for (var k = 0; k < containers.length; k++) {
      var buttons = containers[k].querySelectorAll(BUTTONS);
      for (var b = 0; b < buttons.length; b++) {
        var el = buttons[b];
        var t = label(el);
        if (!t || t.length > 60 || !isReject(t) || CUSTOMIZE_TEXT.test(t)) {
          continue;
        }
        if (visible(el) && click(el, 'text:' + t)) return true;
      }
    }
    return false;
  }

  var start = Date.now();
  var runs = 0;
  var done = false;
  var scheduled = false;
  var observer = null;
  function stop() {
    done = true;
    if (observer) observer.disconnect();
  }
  function run() {
    scheduled = false;
    if (done) return;
    runs++;
    var all = roots();
    if (tryVendors(all) || tryText(all)) {
      stop();
      return;
    }
    if (runs >= MAX_RUNS || Date.now() - start > MAX_MS) stop();
  }
  run();
  if (!done && window.MutationObserver) {
    observer = new MutationObserver(function() {
      if (done || scheduled) return;
      scheduled = true;
      setTimeout(run, 200);
    });
    observer.observe(document.documentElement,
                     {childList: true, subtree: true});
    setTimeout(stop, MAX_MS);
  }
})();
)JS";

bool HostMatches(std::string_view host, std::string_view domain) {
  return host == domain ||
         (host.size() > domain.size() && host.ends_with(domain) &&
          host[host.size() - domain.size() - 1] == '.');
}

void InjectInto(content::RenderFrameHost* frame, bool cmp_document) {
  std::string script =
      base::StrCat({"var __moltConsentMode = '",
                    cmp_document ? "cmp-document" : "page", "';\n",
                    kKillerJS});
  frame->ExecuteJavaScriptInIsolatedWorld(base::UTF8ToUTF16(script),
                                          base::DoNothing(), kWorldId);
}

}  // namespace

// static
bool ConsentKillerTabHelper::IsKnownCmpHost(std::string_view host) {
  for (std::string_view domain : kCmpHosts) {
    if (HostMatches(host, domain)) {
      return true;
    }
  }
  return false;
}

ConsentKillerTabHelper::ConsentKillerTabHelper(content::WebContents* contents)
    : content::WebContentsUserData<ConsentKillerTabHelper>(*contents) {
  // Kill switch (chrome://flags/#molt-shield-consent-killer or
  // --disable-molt-shield): never observe the tab, so nothing is injected
  // and the MoltShield service isn't even created on our behalf.
  if (molt_shield::IsConsentKillerEnabled()) {
    Observe(contents);
  }
}

ConsentKillerTabHelper::~ConsentKillerTabHelper() = default;

void ConsentKillerTabHelper::DOMContentLoaded(
    content::RenderFrameHost* render_frame_host) {
  // Only the page the user is looking at (not prerendered / BFCache pages).
  if (!render_frame_host || !render_frame_host->GetPage().IsPrimary()) {
    return;
  }
  const GURL& url = render_frame_host->GetLastCommittedURL();
  // Real web pages only; chrome://, molt:// and file:// have no consent
  // banners and must not get our script.
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  shield::MoltShieldService* shield = shield::MoltShieldService::Get();
  if (!shield->IsConsentKillerEnabled() ||
      !shield::AreShieldsUpForSite(
          web_contents()->GetBrowserContext(),
          render_frame_host->GetOutermostMainFrame()->GetLastCommittedURL())) {
    return;
  }
  const bool cmp_document = IsKnownCmpHost(url.host());
  if (render_frame_host->IsInPrimaryMainFrame()) {
    InjectInto(render_frame_host, cmp_document);
  } else if (cmp_document) {
    // A CMP's cross-origin dialog iframe: the main-frame script can't reach
    // into it, so inject there directly.
    InjectInto(render_frame_host, /*cmp_document=*/true);
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ConsentKillerTabHelper);

}  // namespace consent
}  // namespace molt_ai
