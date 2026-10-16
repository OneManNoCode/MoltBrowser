// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"

#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_exceptions.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "content/public/browser/page.h"
#include "content/public/browser/render_frame_host.h"

namespace molt_ai::shield {

namespace {

constexpr ContentSettingsType kShieldsSetting = kShieldsDownSetting;

}  // namespace

bool AreShieldsUpForSite(content::BrowserContext* context,
                         const GURL& top_frame_url) {
  MoltShieldService* service = MoltShieldService::GetIfExists();
  if (service && !service->IsEnabled()) {
    return false;
  }
  // Unknown top frame (e.g. service workers), non-web pages: filter.
  return !HasShieldsDownException(context, top_frame_url);
}

bool HasShieldsDownException(content::BrowserContext* context,
                             const GURL& url) {
  HostContentSettingsMap* map =
      context ? HostContentSettingsMapFactory::GetForProfile(context) : nullptr;
  return map && HasShieldsDownException(*map, url);
}

std::vector<std::string> GetShieldsDownTopLevelSites(
    content::BrowserContext* context) {
  HostContentSettingsMap* map =
      context ? HostContentSettingsMapFactory::GetForProfile(context) : nullptr;
  if (!map) {
    return {};
  }
  return ShieldsDownTopLevelSites(*map);
}

void SetShieldsUpForSite(content::BrowserContext* context,
                         const GURL& url,
                         bool shields_up) {
  if (!context || !url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(context);
  if (!map) {
    return;
  }
  map->SetContentSettingCustomScope(
      ContentSettingsPattern::FromURLToSchemefulSitePattern(url),
      ContentSettingsPattern::Wildcard(), kShieldsSetting,
      shields_up ? CONTENT_SETTING_DEFAULT : CONTENT_SETTING_ALLOW);
}

std::vector<std::string> GetShieldsDownSites(content::BrowserContext* context) {
  std::vector<std::string> sites;
  HostContentSettingsMap* map =
      context ? HostContentSettingsMapFactory::GetForProfile(context) : nullptr;
  if (!map) {
    return sites;
  }
  for (const ContentSettingPatternSource& entry :
       map->GetSettingsForOneType(kShieldsSetting)) {
    if (entry.GetContentSetting() == CONTENT_SETTING_ALLOW &&
        !entry.primary_pattern.MatchesAllHosts()) {
      sites.push_back(entry.primary_pattern.ToString());
    }
  }
  return sites;
}

void RemoveShieldsDownException(content::BrowserContext* context,
                                std::string_view pattern) {
  HostContentSettingsMap* map =
      context ? HostContentSettingsMapFactory::GetForProfile(context) : nullptr;
  ContentSettingsPattern primary = ContentSettingsPattern::FromString(pattern);
  if (!map || !primary.IsValid() || primary.MatchesAllHosts()) {
    return;
  }
  map->SetContentSettingCustomScope(primary, ContentSettingsPattern::Wildcard(),
                                    kShieldsSetting, CONTENT_SETTING_DEFAULT);
}

ShieldPageData::ShieldPageData(content::Page& page) : PageUserData(page) {}
ShieldPageData::~ShieldPageData() = default;
PAGE_USER_DATA_KEY_IMPL(ShieldPageData);

void RecordBlockedForFrame(content::RenderFrameHost* frame) {
  if (!frame) {
    return;
  }
  ShieldPageData::GetOrCreateForPage(frame->GetPage())->IncrementBlocked();
}

int64_t GetBlockedCountForPage(content::Page& page) {
  ShieldPageData* data = ShieldPageData::GetForPage(page);
  return data ? data->blocked() : 0;
}

}  // namespace molt_ai::shield
