// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_site_exceptions.h"

#include <algorithm>
#include <string_view>

#include "base/strings/strcat.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "net/base/schemeful_site.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace molt_ai::shield {

bool HasShieldsDownException(const HostContentSettingsMap& map,
                             const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return false;
  }
  content_settings::SettingInfo info;
  const ContentSetting setting =
      map.GetContentSetting(url, GURL(), kShieldsDownSetting, &info);
  // Only an explicit per-site ALLOW lowers Shields; ignore the default.
  return setting == CONTENT_SETTING_ALLOW &&
         !info.primary_pattern.MatchesAllHosts();
}

std::vector<std::string> ShieldsDownTopLevelSites(
    const HostContentSettingsMap& map) {
  std::vector<std::string> sites;
  for (const ContentSettingPatternSource& entry :
       map.GetSettingsForOneType(kShieldsDownSetting)) {
    if (entry.GetContentSetting() != CONTENT_SETTING_ALLOW ||
        entry.primary_pattern.MatchesAllHosts() ||
        entry.primary_pattern.GetHost().empty()) {
      continue;
    }
    std::vector<std::string_view> schemes;
    switch (entry.primary_pattern.GetSchemeType()) {
      case ContentSettingsPattern::SCHEME_HTTPS:
        schemes = {url::kHttpsScheme};
        break;
      case ContentSettingsPattern::SCHEME_HTTP:
        schemes = {url::kHttpScheme};
        break;
      case ContentSettingsPattern::SCHEME_WILDCARD:
        schemes = {url::kHttpsScheme, url::kHttpScheme};
        break;
      default:
        continue;
    }
    for (std::string_view scheme : schemes) {
      const GURL candidate(
          base::StrCat({scheme, url::kStandardSchemeSeparator,
                        entry.primary_pattern.GetHost(), "/"}));
      if (!candidate.is_valid()) {
        continue;
      }
      // The pattern's host may be a subdomain: check the site's own URL, the
      // same lookup AreShieldsUpForSite does for a page on that site.
      const net::SchemefulSite site(candidate);
      if (HasShieldsDownException(map, site.GetURL())) {
        sites.push_back(site.Serialize());
      }
    }
  }
  std::sort(sites.begin(), sites.end());
  sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
  return sites;
}

}  // namespace molt_ai::shield
