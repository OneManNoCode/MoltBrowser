// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield's per-site "Shields off" semantics on a HostContentSettingsMap,
// without a profile (molt_shield_site_settings.h has the BrowserContext
// wrappers the rest of the browser uses). Split out so it can be unit-tested
// in molt_ai_unittests.
//
// The switch is stored as ContentSettingsType::ADS -- Chromium's "Intrusive
// ads" setting, whose meaning ("may this site show ads?") is exactly a
// Shields-down exception: ALLOW for a site = MoltShield off there. Only
// explicit per-site exceptions count; the ADS *default* is ignored, so
// changing the global "Intrusive ads" default can't silently turn MoltShield
// (or fingerprinting protection) off.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SITE_EXCEPTIONS_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SITE_EXCEPTIONS_H_

#include "components/content_settings/core/common/content_settings_types.h"

class GURL;
class HostContentSettingsMap;

namespace molt_ai::shield {

inline constexpr ContentSettingsType kShieldsDownSetting =
    ContentSettingsType::ADS;

// True if an explicit per-site ALLOW exception matches `url` (http(s) only).
// Fingerprinting protection asks it about a site's own URL
// ("https://example.com/", chrome/browser/molt_ai/fingerprinting/
// molt_shields_decision.h), so the site-scoped "https://[*.]example.com"
// exception the MoltShield UI writes turns farbling off on the whole site,
// while one for "sub.example.com" alone doesn't.
bool HasShieldsDownException(const HostContentSettingsMap& map,
                             const GURL& url);

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SITE_EXCEPTIONS_H_
