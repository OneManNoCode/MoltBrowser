// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_site_exceptions.h"

#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "url/gurl.h"

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

}  // namespace molt_ai::shield
