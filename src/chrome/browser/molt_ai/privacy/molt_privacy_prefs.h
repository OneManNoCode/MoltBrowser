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
//
// HTTPS-First "balanced" mode can't be expressed as a default (Chromium
// only honors kHttpsFirstBalancedMode once a user value exists), so
// ApplyPrivacyPrefMigrations() turns it on ONCE for profiles where none of
// the HTTPS-First prefs was ever set.
//
// Global Privacy Control is not available at the pinned Chromium (no
// Sec-GPC / navigator.globalPrivacyControl support upstream), so it is not
// set here — see docs/MOLTNET_COMPLIANCE.md.

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
