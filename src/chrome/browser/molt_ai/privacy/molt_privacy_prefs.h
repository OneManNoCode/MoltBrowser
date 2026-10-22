// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Privacy-by-default settings for regular MoltBrowser profiles.
//
// Implemented as new DEFAULT values (PrefRegistry::SetDefaultPrefValue), so
// new and existing users get them automatically while anyone who explicitly
// chose a value keeps it (a user-set pref always wins over the default):
//   * Third-party cookies blocked          (profile.cookie_controls_mode = 1)
//   * WebRTC IP policy default_public_interface_only (no local/VPN-hidden
//     interface IPs in ICE candidates)
//   * Network prediction (DNS prefetch / preconnect / prerender) off
//   * Do Not Track off — DNT is ignored by sites and is one more
//     fingerprinting bit (explicit; it is also upstream's default)
//   * "Alternate error pages" off — this pref gates the DNS probe that
//     queries Google Public DNS (8.8.8.8 / 8.8.4.4) for google.com after a
//     DNS error, and captive-portal checks against
//     (connectivitycheck.)gstatic.com/generate_204 after SSL errors and
//     timeouts. Error pages still show; they are just less specific.
//   * Password leak detection off — for signed-out users the check is
//     authenticated only by the Google API key, which MoltBrowser does not
//     have, so it sends hashed credential material to
//     passwordsleakcheck-pa.googleapis.com with a placeholder key that
//     Google is expected to reject (docs/NETWORK_TRAFFIC.md lists it for
//     the traffic capture). Turn this default back on if GenEye ever ships
//     an API key.
//   * The Translator web API off (TranslatorAPIAllowed) — upstream's
//     Translator has the component updater fetch Google's TranslateKit and
//     language packs, outside Tor, when a page asks for it
//     (docs/design/WEB_AI_APIS.md). The policy can still turn it on.
//
// HTTPS-First "balanced" mode can't be expressed as a default (Chromium
// only honors kHttpsFirstBalancedMode once a user value exists), so
// ApplyPrivacyPrefMigrations() turns it on ONCE for profiles where none of
// the HTTPS-First prefs was ever set.
//
// Global Privacy Control is a feature flag, not a pref, at Chromium 152: it is
// turned on in molt_feature_overrides.cc (docs/MOLTNET_COMPLIANCE.md).

#ifndef CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_PRIVACY_PREFS_H_
#define CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_PRIVACY_PREFS_H_

class PrefService;

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace molt_ai {
namespace privacy {

// Call right after all profile prefs are registered (ProfileImpl, after
// ::RegisterProfilePrefs). Every pref touched here must already be
// registered.
void SetPrivacyPrefDefaults(user_prefs::PrefRegistrySyncable* registry);

// Call once per regular profile after its prefs are loaded. Idempotent.
void ApplyPrivacyPrefMigrations(PrefService* prefs);

}  // namespace privacy
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_PRIVACY_PREFS_H_
