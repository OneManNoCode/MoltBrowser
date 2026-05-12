// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/consent/consent_killer_tab_helper.h"

#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace consent {

namespace {

// The killer JS. Runs in isolated world 1, idempotent via
// window.__moltConsentKilled. The 3-stage setTimeout cascade is
// because many CMPs lazy-mount their banner via a script tag that
// loads ~1-2s after page load.
constexpr char kKillerJS[] = R"JS(
(function(){
  if (window.__moltConsentKillerInstalled) return;
  window.__moltConsentKillerInstalled = true;

  // Tier 1: curated CMP vendor selectors. Ranked roughly by deploy
  // frequency. We click the FIRST visible match and stop — most
  // sites only ship one CMP at a time.
  var rules = [
    // OneTrust (the biggest single deploy: CNN, BBC, NYT, USA Today, etc.)
    '#onetrust-reject-all-handler',
    'button.ot-pc-refuse-all-handler',
    '#ot-pc-btn-handler',
    // Cookiebot (Volvo, ProtonMail, lots of EU sites)
    '#CybotCookiebotDialogBodyButtonDecline',
    'button#CybotCookiebotDialogBodyLevelButtonCustomize',
    // TrustArc / TRUSTe
    '#truste-consent-required',
    'a.call[href="#"][onclick*="reject" i]',
    // Quantcast / TCF v2
    'button.qc-cmp2-summary-buttons[mode="secondary"]',
    'button[mode="primary"][aria-label*="Disagree" i]',
    // Didomi (Le Monde, lots of French sites)
    '#didomi-notice-disagree-button',
    'button.didomi-components-button--secondary',
    // CookieYes
    'button.cky-btn-reject',
    // Osano
    'button.osano-cm-denyAll',
    // Sourcepoint
    'button.sp_choice_type_REJECT_ALL',
    'button[title="Reject All" i]',
    // Klaro / Common open-source
    '.klaro .cm-btn-decline',
    // Usercentrics
    'button[data-testid="uc-deny-all-button"]',
    'button[data-testid="uc-customize-button"]',
    // Generic patterns that catch many in-house CMPs
    'button[aria-label*="Reject all" i]',
    'button[aria-label*="Reject All" i]',
    'button[aria-label*="Decline all" i]',
    'button[aria-label*="Necessary only" i]',
    'button[aria-label*="Only essential" i]',
    'button[data-testid="reject-all"]',
    'button[data-testid="decline-all"]',
    'button[data-cookieconsent="reject"]',
    'button[data-cy="reject-all"]',
    'a[role="button"][title*="Reject all" i]',
    'a[role="button"][title*="Decline" i]',
  ];

  // Text-based fallback phrases (lowercase, exact-or-prefix match).
  // Ordered most-specific first so e.g. "reject all" outranks the
  // bare "reject" which might match an unrelated form button.
  var rejectPhrases = [
    'reject all', 'reject non-essential', 'decline all',
    'decline non-essential', 'only necessary', 'necessary only',
    'strictly necessary', 'essential only', 'only essential',
    'continue without accepting', 'i decline', 'no thanks',
    'do not accept', 'refuse all',
  ];

  function tryKill() {
    if (window.__moltConsentKilled) return;
    // Tier 1: curated selectors.
    for (var i = 0; i < rules.length; i++) {
      var el = document.querySelector(rules[i]);
      if (el && el.offsetParent !== null) {
        try {
          el.click();
          window.__moltConsentKilled = rules[i];
          return;
        } catch (e) { /* swallow */ }
      }
    }
    // Tier 2: text-based fallback on visible buttons.
    var candidates = document.querySelectorAll(
        'button, a[role="button"], [role="button"]');
    for (var j = 0; j < candidates.length; j++) {
      var c = candidates[j];
      if (c.offsetParent === null) continue;
      var t = (c.innerText || c.textContent || '')
                .toLowerCase().trim();
      if (!t || t.length > 60) continue;  // skip giant text blocks
      for (var k = 0; k < rejectPhrases.length; k++) {
        var phrase = rejectPhrases[k];
        if (t === phrase || t.indexOf(phrase) === 0) {
          try {
            c.click();
            window.__moltConsentKilled = 'text:' + phrase;
            return;
          } catch (e) { /* swallow */ }
        }
      }
    }
  }

  // Three-shot at 600ms / 1800ms / 4000ms — catches lazy-mounted CMPs
  // without spinning a MutationObserver indefinitely.
  setTimeout(tryKill, 600);
  setTimeout(tryKill, 1800);
  setTimeout(tryKill, 4000);
})();
)JS";

}  // namespace

ConsentKillerTabHelper::ConsentKillerTabHelper(
    content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<ConsentKillerTabHelper>(*contents) {}

ConsentKillerTabHelper::~ConsentKillerTabHelper() = default;

void ConsentKillerTabHelper::DocumentOnLoadCompletedInPrimaryMainFrame() {
  InjectKiller();
}

void ConsentKillerTabHelper::InjectKiller() {
  if (!web_contents() || !web_contents()->GetPrimaryMainFrame())
    return;
  // Only run on real web pages — chrome://, molt://, file:// don't
  // have consent banners and we don't want our script tripping any
  // privileged-page heuristics.
  if (!web_contents()->GetLastCommittedURL().SchemeIsHTTPOrHTTPS())
    return;
  web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(kKillerJS)),
      base::DoNothing(),
      /*world_id=*/1);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(ConsentKillerTabHelper);

}  // namespace consent
}  // namespace molt_ai
