// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Per-site Shields switch + per-page blocked counters.
//
// The switch is stored in the profile's HostContentSettingsMap as
// ContentSettingsType::ADS — Chromium's "Intrusive ads" setting, whose
// meaning ("may this site show ads?") is exactly a Shields-down exception:
// ALLOW for a site = MoltShield off there. Exceptions are site-scoped
// (https://[*.]example.com), inherited by Incognito, and only explicit
// per-site exceptions count — the ADS *default* is ignored, so changing the
// global "Intrusive ads" default can't silently turn MoltShield off.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SITE_SETTINGS_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SITE_SETTINGS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "content/public/browser/page_user_data.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
class Page;
class RenderFrameHost;
}  // namespace content

namespace molt_ai::shield {

// UI thread (the content-settings read is thread-safe, but callers are UI).
// False when MoltShield is off globally or the user turned it off for the
// site of `top_frame_url`. Non-http(s) pages are never filtered.
bool AreShieldsUpForSite(content::BrowserContext* context,
                         const GURL& top_frame_url);

// Adds/removes the site-wide exception for `url`'s site.
void SetShieldsUpForSite(content::BrowserContext* context,
                         const GURL& url,
                         bool shields_up);

// Sites with Shields down, as content-settings patterns for display.
std::vector<std::string> GetShieldsDownSites(content::BrowserContext* context);

// Removes a Shields-down exception by the pattern GetShieldsDownSites()
// returned. Invalid patterns are ignored.
void RemoveShieldsDownException(content::BrowserContext* context,
                                std::string_view pattern);

// Requests blocked while a page was loaded (primary, prerendered or in the
// back/forward cache), so counts follow the page.
class ShieldPageData : public content::PageUserData<ShieldPageData> {
 public:
  ShieldPageData(const ShieldPageData&) = delete;
  ShieldPageData& operator=(const ShieldPageData&) = delete;
  ~ShieldPageData() override;

  int64_t blocked() const { return blocked_; }
  void IncrementBlocked() { ++blocked_; }

 private:
  explicit ShieldPageData(content::Page& page);
  friend PageUserData;
  PAGE_USER_DATA_KEY_DECL();

  int64_t blocked_ = 0;
};

// Attributes a blocked request to the page of `frame` (UI thread; no-op if
// the frame is gone).
void RecordBlockedForFrame(content::RenderFrameHost* frame);

// Requests blocked while `page` was loaded (0 if none recorded).
int64_t GetBlockedCountForPage(content::Page& page);

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SITE_SETTINGS_H_
