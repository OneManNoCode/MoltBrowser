// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Components that Chromium registers with the component updater at every
// start although only Google features MoltBrowser doesn't ship use them
// (docs/NETWORK_TRAFFIC.md, "Changes made in this audit"). The hook in
// chrome/browser/component_updater/registration.cc skips them unless
// kMoltGoogleOnlyComponents is on:
//   * Actor safety lists: the site lists of Gemini in Chrome's browser
//     actor (actor::SafetyListManager). MoltBrowser's agent mode is its own
//     (molt_ai/automation, molt_ai/agents) and never reads them.
//   * CAPTCHA providers: a list of CAPTCHA scripts, read only by
//     CaptchaMetricsObserver for usage statistics, which MoltBrowser never
//     sends.
//   * Amount extraction regexes: find a checkout total for Autofill's
//     buy-now-pay-later offers, which come from Google Payments.
//   * Commerce heuristics: cart and checkout detection for the shopping
//     cart module of Google's New Tab Page (registered only in the US).
//     MoltBrowser's new tab page is molt://home.
// None of them is a field-trial feature, so the testing config cannot
// bring them back; --enable-features=MoltGoogleOnlyComponents does.

#ifndef CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_COMPONENT_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_COMPONENT_POLICY_H_

#include "base/feature_list.h"

namespace molt_ai {
namespace privacy {

// Off by default: the components above are not registered.
BASE_DECLARE_FEATURE(kMoltGoogleOnlyComponents);

// Whether registration.cc registers the components above.
bool ShouldRegisterGoogleOnlyComponents();

}  // namespace privacy
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_COMPONENT_POLICY_H_
