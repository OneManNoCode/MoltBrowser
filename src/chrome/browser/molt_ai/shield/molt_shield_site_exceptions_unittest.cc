// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_site_exceptions.h"

#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai::shield {
namespace {

using Sites = std::vector<std::string>;

class MoltShieldSiteExceptionsTest : public testing::Test {
 protected:
  void SetUp() override {
    HostContentSettingsMap::RegisterProfilePrefs(prefs_.registry());
    map_ = base::MakeRefCounted<HostContentSettingsMap>(
        &prefs_, /*is_off_the_record=*/false, /*store_last_modified=*/false,
        /*restore_session=*/false, /*should_record_metrics=*/false);
  }
  void TearDown() override { map_->ShutdownOnUIThread(); }

  void Set(const ContentSettingsPattern& pattern, ContentSetting setting) {
    map_->SetContentSettingCustomScope(pattern,
                                       ContentSettingsPattern::Wildcard(),
                                       kShieldsDownSetting, setting);
  }
  // What the MoltShield UI writes (SetShieldsUpForSite).
  void ShieldsDownFor(const char* url) {
    Set(ContentSettingsPattern::FromURLToSchemefulSitePattern(GURL(url)),
        CONTENT_SETTING_ALLOW);
  }

  base::test::TaskEnvironment task_environment_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
  scoped_refptr<HostContentSettingsMap> map_;
};

TEST_F(MoltShieldSiteExceptionsTest, NoExceptionsNoSites) {
  EXPECT_EQ(ShieldsDownTopLevelSites(*map_), Sites());
  EXPECT_FALSE(HasShieldsDownException(*map_, GURL("https://example.com/")));
}

TEST_F(MoltShieldSiteExceptionsTest, TheUiExceptionCoversTheWholeSite) {
  ShieldsDownFor("https://www.example.com/page");
  EXPECT_EQ(ShieldsDownTopLevelSites(*map_), Sites({"https://example.com"}));
  EXPECT_TRUE(HasShieldsDownException(*map_, GURL("https://example.com/")));
  EXPECT_TRUE(
      HasShieldsDownException(*map_, GURL("https://shop.example.com/x")));
  // Schemeful: the http:// site keeps its Shields.
  EXPECT_FALSE(HasShieldsDownException(*map_, GURL("http://example.com/")));
  EXPECT_FALSE(HasShieldsDownException(*map_, GURL("https://example.org/")));
}

TEST_F(MoltShieldSiteExceptionsTest, SchemelessDomainPatternsCoverBothSchemes) {
  Set(ContentSettingsPattern::FromString("[*.]example.org"),
      CONTENT_SETTING_ALLOW);
  EXPECT_EQ(ShieldsDownTopLevelSites(*map_),
            Sites({"http://example.org", "https://example.org"}));
}

TEST_F(MoltShieldSiteExceptionsTest, NarrowerPatternsDontCoverTheSite) {
  // One subdomain only: pages there have Shields off, but the site's key is
  // shared with the rest of example.net, so farbling stays on.
  Set(ContentSettingsPattern::FromString("https://sub.example.net"),
      CONTENT_SETTING_ALLOW);
  Set(ContentSettingsPattern::FromString("[*.]deep.example.net"),
      CONTENT_SETTING_ALLOW);
  EXPECT_TRUE(HasShieldsDownException(*map_, GURL("https://sub.example.net/")));
  EXPECT_EQ(ShieldsDownTopLevelSites(*map_), Sites());
}

TEST_F(MoltShieldSiteExceptionsTest, OnlyExplicitAllowExceptionsCount) {
  map_->SetDefaultContentSetting(kShieldsDownSetting, CONTENT_SETTING_ALLOW);
  EXPECT_FALSE(HasShieldsDownException(*map_, GURL("https://example.com/")));
  EXPECT_EQ(ShieldsDownTopLevelSites(*map_), Sites());
  Set(ContentSettingsPattern::FromString("[*.]blocked.example"),
      CONTENT_SETTING_BLOCK);
  EXPECT_EQ(ShieldsDownTopLevelSites(*map_), Sites());
}

TEST_F(MoltShieldSiteExceptionsTest, SitesAreSortedAndUnique) {
  ShieldsDownFor("https://b.example/");
  ShieldsDownFor("https://www.a.example/");
  ShieldsDownFor("https://a.example/other");
  Set(ContentSettingsPattern::FromString("https://[*.]b.example"),
      CONTENT_SETTING_ALLOW);
  EXPECT_EQ(ShieldsDownTopLevelSites(*map_),
            Sites({"https://a.example", "https://b.example"}));
}

TEST_F(MoltShieldSiteExceptionsTest, NonWebUrlsNeverHaveShieldsDown) {
  ShieldsDownFor("https://example.com/");
  EXPECT_FALSE(HasShieldsDownException(*map_, GURL("chrome://settings")));
  EXPECT_FALSE(HasShieldsDownException(*map_, GURL("file:///tmp/x")));
  EXPECT_FALSE(HasShieldsDownException(*map_, GURL()));
}

}  // namespace
}  // namespace molt_ai::shield
