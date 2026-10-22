// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_pref_isolation.h"

#include <memory>
#include <string>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/shield/molt_shield_prefs.h"
#include "chrome/browser/molt_ai/tor/molt_net_pref_isolation_policy.h"
#include "chrome/common/pref_names.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/browser/website_settings_info.h"
#include "components/content_settings/core/browser/website_settings_registry.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/language/core/browser/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/privacy_sandbox/privacy_sandbox_prefs.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai::tor {
namespace {

// The literals in molt_net_pref_isolation_policy.h are Chromium's pref names;
// a rename at a rebase must not silently unpin a pref.
TEST(MoltNetIsolationTest, PinnedNamesMatchChromiumsConstants) {
  for (const char* name : {
           prefs::kWebKitStandardFontFamily,
           prefs::kWebKitSansSerifFontFamilyJapanese,
           prefs::kWebKitDefaultFontSize,
           prefs::kWebKitMinimumFontSize,
           prefs::kWebKitJavascriptEnabled,
           prefs::kWebkitTabsToLinks,
           prefs::kEnableEncryptedMedia,
           prefs::kDefaultCharset,
           prefs::kEnableDoNotTrack,
           prefs::kWebRTCIPHandlingPolicy,
           // (prefs::kAnimationPolicy only exists with extensions.)
           prefs::kCaretBrowsingEnabled,
#if !BUILDFLAG(IS_CHROMEOS)
           prefs::kAccessibilityFocusHighlightEnabled,
#endif
           prefs::kPageColorsBlockList,
           prefs::kPrefersDefaultScrollbarStyles,
           prefs::kCookieControlsMode,
           language::prefs::kAcceptLanguages,
           prefs::kPrivacySandboxM1TopicsEnabled,
           prefs::kPrivacySandboxM1FledgeEnabled,
           prefs::kPrivacySandboxM1AdMeasurementEnabled,
           shield::prefs::kRemoveTrackingParams,
       }) {
    EXPECT_TRUE(IsMoltNetPinnedPref(name)) << name;
  }
  // Related Website Sets are permanently off in off-the-record profiles
  // (FirstPartySetsPolicyService), so their switch isn't pinned.
  EXPECT_FALSE(
      IsMoltNetPinnedPref(prefs::kPrivacySandboxRelatedWebsiteSetsEnabled));
}

// Every content and website setting type, present and future, has its
// exceptions and its default pinned.
TEST(MoltNetIsolationTest, EveryContentSettingTypeIsPinned) {
  // Content settings register themselves in the website settings registry.
  content_settings::ContentSettingsRegistry::GetInstance();
  for (const content_settings::WebsiteSettingsInfo* info :
       *content_settings::WebsiteSettingsRegistry::GetInstance()) {
    EXPECT_TRUE(IsMoltNetPinnedPref(info->pref_name())) << info->name();
    EXPECT_TRUE(IsMoltNetPinnedPref(info->partitioned_pref_name()))
        << info->name();
    EXPECT_TRUE(IsMoltNetPinnedPref(info->default_value_pref_name()))
        << info->name();
  }
}

class MoltNetPrefServiceTest : public testing::Test {
 protected:
  // Stand-ins with the shapes of the real registrations; the names decide
  // what is pinned.
  static constexpr char kFontSize[] = "webkit.webprefs.default_font_size";
  static constexpr char kDoNotTrack[] = "enable_do_not_track";
  static constexpr char kLanguages[] = "intl.accept_languages";
  static constexpr char kCookieMode[] = "profile.cookie_controls_mode";
  static constexpr char kBookmarkBar[] = "bookmark_bar.show_on_all_tabs";

  void SetUp() override {
    user_prefs::PrefRegistrySyncable* registry = regular_.registry();
    registry->RegisterIntegerPref(kFontSize, 16);
    registry->RegisterBooleanPref(kDoNotTrack, true);
    // A MoltBrowser-style default override (molt_privacy_prefs.cc): MoltNet
    // must get the overridden default, not the original one.
    registry->SetDefaultPrefValue(kDoNotTrack, base::Value(false));
    registry->RegisterStringPref(kLanguages, "en-US,en");
    registry->RegisterIntegerPref(kCookieMode, 1);
    registry->RegisterBooleanPref(kBookmarkBar, false);

    // The user's own settings in the regular profile.
    regular_.SetInteger(kFontSize, 24);
    regular_.SetBoolean(kDoNotTrack, true);
    regular_.SetString(kLanguages, "de-CH,de,fr");
    regular_.SetInteger(kCookieMode, 0);
    regular_.SetBoolean(kBookmarkBar, true);
  }

  sync_preferences::TestingPrefServiceSyncable regular_;
};

TEST_F(MoltNetPrefServiceTest, PagesSeeDefaultsNotTheUsersSettings) {
  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular_, nullptr);

  EXPECT_EQ(moltnet->GetInteger(kFontSize), 16);
  EXPECT_FALSE(moltnet->GetBoolean(kDoNotTrack));
  EXPECT_EQ(moltnet->GetString(kLanguages), "en-US,en");
  EXPECT_EQ(moltnet->GetInteger(kCookieMode), 1);
  // What pages can't see keeps following the regular profile.
  EXPECT_TRUE(moltnet->GetBoolean(kBookmarkBar));

  // The regular profile keeps its own values.
  EXPECT_EQ(regular_.GetInteger(kFontSize), 24);
  EXPECT_TRUE(regular_.GetBoolean(kDoNotTrack));
}

TEST_F(MoltNetPrefServiceTest,
       LaterChangesInTheRegularProfileDontReachMoltNet) {
  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular_, nullptr);

  regular_.SetInteger(kFontSize, 30);
  regular_.ClearPref(kDoNotTrack);
  regular_.SetBoolean(kBookmarkBar, false);

  EXPECT_EQ(moltnet->GetInteger(kFontSize), 16);
  EXPECT_FALSE(moltnet->GetBoolean(kDoNotTrack));
  EXPECT_FALSE(moltnet->GetBoolean(kBookmarkBar));
}

TEST_F(MoltNetPrefServiceTest, MoltNetWritesStayInMoltNet) {
  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular_, nullptr);

  // MoltNet's own override (MoltNetRouting::ApplyRoutingPrefs) wins over the
  // pin and stays in MoltNet.
  moltnet->SetString(kLanguages, "en-US,en;q=0.9");
  EXPECT_EQ(moltnet->GetString(kLanguages), "en-US,en;q=0.9");
  EXPECT_EQ(regular_.GetString(kLanguages), "de-CH,de,fr");

  // Incognito writes the third-party-cookie mode and the bookmarks bar
  // through to the regular profile (its persistent prefs); MoltNet doesn't.
  moltnet->SetInteger(kCookieMode, 2);
  moltnet->SetBoolean(kBookmarkBar, false);
  EXPECT_EQ(regular_.GetInteger(kCookieMode), 0);
  EXPECT_TRUE(regular_.GetBoolean(kBookmarkBar));
}

TEST_F(MoltNetPrefServiceTest, PolicyStillWins) {
  regular_.SetManagedPref(kFontSize, base::Value(20));
  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular_, nullptr);
  EXPECT_EQ(moltnet->GetInteger(kFontSize), 20);
}

TEST_F(MoltNetPrefServiceTest, PinsOnlyPinnedPrefs) {
  sync_preferences::TestingPrefServiceSyncable other;
  other.registry()->RegisterBooleanPref(kBookmarkBar, false);
  other.registry()->RegisterIntegerPref(kFontSize, 16);
  EXPECT_EQ(PinMoltNetPrefsToDefaults(&other), 1u);
  EXPECT_TRUE(other.FindPreference(kFontSize)->IsUserControlled());
  EXPECT_FALSE(other.FindPreference(kBookmarkBar)->IsUserControlled());
  EXPECT_EQ(PinMoltNetPrefsToDefaults(nullptr), 0u);
}

// Chromium registers the default fonts, the minimum logical font size and
// the default encoding from the UI language's resources; MoltNet uses fixed
// per-OS values instead (molt_net_locale_prefs.h). Here, the registry is the
// one a Japanese UI gives on Windows.
TEST(MoltNetLocalePinTest, LocaleDependentDefaultsAreFixed) {
  static constexpr char kStandard[] = "webkit.webprefs.fonts.standard.Zyyy";
  static constexpr char kStandardJapanese[] =
      "webkit.webprefs.fonts.standard.Jpan";
  static constexpr char kStandardThai[] = "webkit.webprefs.fonts.standard.Thai";
  static constexpr char kMinimumLogical[] =
      "webkit.webprefs.minimum_logical_font_size";
  static constexpr char kEncoding[] = "intl.charset_default";
  static constexpr char kDefaultSize[] = "webkit.webprefs.default_font_size";

  sync_preferences::TestingPrefServiceSyncable japanese_ui;
  user_prefs::PrefRegistrySyncable* registry = japanese_ui.registry();
  registry->RegisterStringPref(kStandard, "Meiryo");
  // The UI language's own script gets no per-script default.
  registry->RegisterStringPref(kStandardJapanese, "");
  registry->RegisterStringPref(kStandardThai, "");
  registry->RegisterIntegerPref(kMinimumLogical, 10);
  registry->RegisterStringPref(kEncoding, "Shift_JIS");
  registry->RegisterIntegerPref(kDefaultSize, 16);
  japanese_ui.SetString(kStandard, "MS Mincho");
  japanese_ui.SetInteger(kDefaultSize, 20);

  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&japanese_ui, nullptr);

  EXPECT_EQ(moltnet->GetString(kEncoding), "windows-1252");
  EXPECT_EQ(moltnet->GetInteger(kDefaultSize), 16);
  EXPECT_EQ(moltnet->GetString(kStandardThai), "");
#if BUILDFLAG(IS_WIN)
  EXPECT_EQ(moltnet->GetString(kStandard), "Times New Roman");
  EXPECT_EQ(moltnet->GetString(kStandardJapanese), "Yu Gothic");
  EXPECT_EQ(moltnet->GetInteger(kMinimumLogical), 6);
#elif BUILDFLAG(IS_MAC)
  EXPECT_EQ(moltnet->GetString(kStandard), "Times");
  EXPECT_EQ(moltnet->GetString(kStandardJapanese),
            "Hiragino Kaku Gothic ProN");
  EXPECT_EQ(moltnet->GetInteger(kMinimumLogical), 9);
#elif BUILDFLAG(IS_LINUX)
  EXPECT_EQ(moltnet->GetString(kStandard), "Liberation Serif");
  EXPECT_EQ(moltnet->GetString(kStandardJapanese), "Liberation Serif");
  EXPECT_EQ(moltnet->GetInteger(kMinimumLogical), 6);
#else
  // No MoltNet font table here: the registered defaults.
  EXPECT_EQ(moltnet->GetString(kStandard), "Meiryo");
  EXPECT_EQ(moltnet->GetInteger(kMinimumLogical), 10);
#endif
  // The regular profile keeps the user's own settings and its defaults.
  EXPECT_EQ(japanese_ui.GetString(kStandard), "MS Mincho");
  EXPECT_EQ(japanese_ui.GetString(kEncoding), "Shift_JIS");
}

// A fixed value of the wrong type for what is registered (a retyped pref)
// leaves the registered default.
TEST(MoltNetLocalePinTest, TypeMismatchKeepsTheRegisteredDefault) {
  sync_preferences::TestingPrefServiceSyncable prefs;
  prefs.registry()->RegisterIntegerPref("intl.charset_default", 3);
  prefs.registry()->RegisterStringPref(
      "webkit.webprefs.minimum_logical_font_size", "10");
  EXPECT_EQ(PinMoltNetPrefsToDefaults(&prefs), 2u);
  EXPECT_EQ(prefs.GetInteger("intl.charset_default"), 3);
  EXPECT_EQ(prefs.GetString("webkit.webprefs.minimum_logical_font_size"),
            "10");
}

// Content settings live in prefs, so MoltNet's HostContentSettingsMap starts
// from the registered defaults: no site exception, changed default, embargo
// or "Shields off" from the regular profile.
class MoltNetContentSettingsTest : public testing::Test {
 protected:
  void SetUp() override {
    HostContentSettingsMap::RegisterProfilePrefs(regular_.registry());
    regular_map_ = MakeMap(&regular_, /*off_the_record=*/false);

    // The user's choices in regular windows.
    regular_map_->SetDefaultContentSetting(ContentSettingsType::JAVASCRIPT,
                                           CONTENT_SETTING_BLOCK);
    regular_map_->SetContentSettingDefaultScope(
        GURL(kSite), GURL(), ContentSettingsType::WINDOW_MANAGEMENT,
        CONTENT_SETTING_BLOCK);
    regular_map_->SetContentSettingCustomScope(
        ContentSettingsPattern::FromURLToSchemefulSitePattern(GURL(kSite)),
        ContentSettingsPattern::Wildcard(), ContentSettingsType::ADS,
        CONTENT_SETTING_ALLOW);
    // What PermissionDecisionAutoBlocker stores for an embargoed origin.
    base::DictValue dismissals;
    dismissals.Set("dismiss_count", 3);
    base::DictValue embargo;
    embargo.Set("geolocation", std::move(dismissals));
    regular_map_->SetWebsiteSettingDefaultScope(
        GURL(kSite), GURL(), ContentSettingsType::PERMISSION_AUTOBLOCKER_DATA,
        base::Value(std::move(embargo)));
  }

  void TearDown() override {
    for (const scoped_refptr<HostContentSettingsMap>& map :
         {moltnet_map_, incognito_map_, regular_map_}) {
      if (map) {
        map->ShutdownOnUIThread();
      }
    }
  }

  static scoped_refptr<HostContentSettingsMap> MakeMap(PrefService* prefs,
                                                       bool off_the_record) {
    return base::MakeRefCounted<HostContentSettingsMap>(
        prefs, off_the_record, /*store_last_modified=*/false,
        /*restore_session=*/false, /*should_record_metrics=*/false);
  }

  static ContentSetting Get(HostContentSettingsMap& map,
                            ContentSettingsType type) {
    return map.GetContentSetting(GURL(kSite), GURL(kSite), type);
  }

  static constexpr char kSite[] = "https://example.com/";

  base::test::TaskEnvironment task_environment_;
  sync_preferences::TestingPrefServiceSyncable regular_;
  std::unique_ptr<sync_preferences::PrefServiceSyncable> incognito_prefs_;
  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet_prefs_;
  scoped_refptr<HostContentSettingsMap> regular_map_;
  scoped_refptr<HostContentSettingsMap> incognito_map_;
  scoped_refptr<HostContentSettingsMap> moltnet_map_;
};

TEST_F(MoltNetContentSettingsTest, IncognitoInheritsWhatMoltNetDoesNot) {
  // The control: a plain Incognito-style overlay inherits all four.
  incognito_prefs_ = regular_.CreateIncognitoPrefService(nullptr, {});
  incognito_map_ = MakeMap(incognito_prefs_.get(), /*off_the_record=*/true);
  EXPECT_EQ(Get(*incognito_map_, ContentSettingsType::JAVASCRIPT),
            CONTENT_SETTING_BLOCK);
  EXPECT_EQ(Get(*incognito_map_, ContentSettingsType::WINDOW_MANAGEMENT),
            CONTENT_SETTING_BLOCK);
  EXPECT_EQ(Get(*incognito_map_, ContentSettingsType::ADS),
            CONTENT_SETTING_ALLOW);
  EXPECT_FALSE(
      incognito_map_
          ->GetWebsiteSetting(GURL(kSite), GURL(),
                              ContentSettingsType::PERMISSION_AUTOBLOCKER_DATA)
          .is_none());

  moltnet_prefs_ = CreateMoltNetPrefService(&regular_, nullptr);
  moltnet_map_ = MakeMap(moltnet_prefs_.get(), /*off_the_record=*/true);
  EXPECT_EQ(Get(*moltnet_map_, ContentSettingsType::JAVASCRIPT),
            CONTENT_SETTING_ALLOW);
  EXPECT_EQ(Get(*moltnet_map_, ContentSettingsType::WINDOW_MANAGEMENT),
            CONTENT_SETTING_ASK);
  // Shields stay up: ADS's default (BLOCK) applies.
  EXPECT_EQ(Get(*moltnet_map_, ContentSettingsType::ADS),
            CONTENT_SETTING_BLOCK);
  EXPECT_TRUE(
      moltnet_map_
          ->GetWebsiteSetting(GURL(kSite), GURL(),
                              ContentSettingsType::PERMISSION_AUTOBLOCKER_DATA)
          .is_none());
  // Only the default rule is left, no per-site rule.
  for (const ContentSettingPatternSource& rule :
       moltnet_map_->GetSettingsForOneType(ContentSettingsType::ADS)) {
    EXPECT_TRUE(rule.primary_pattern.MatchesAllHosts())
        << rule.primary_pattern.ToString();
  }
}

TEST_F(MoltNetContentSettingsTest, SessionChangesStayInTheSession) {
  moltnet_prefs_ = CreateMoltNetPrefService(&regular_, nullptr);
  moltnet_map_ = MakeMap(moltnet_prefs_.get(), /*off_the_record=*/true);

  // Shields off for the site, set in a MoltNet window.
  moltnet_map_->SetContentSettingCustomScope(
      ContentSettingsPattern::FromURLToSchemefulSitePattern(
          GURL("https://session.example/")),
      ContentSettingsPattern::Wildcard(), ContentSettingsType::ADS,
      CONTENT_SETTING_ALLOW);
  EXPECT_EQ(moltnet_map_->GetContentSetting(GURL("https://session.example/"),
                                            GURL(), ContentSettingsType::ADS),
            CONTENT_SETTING_ALLOW);
  EXPECT_EQ(regular_map_->GetContentSetting(GURL("https://session.example/"),
                                            GURL(), ContentSettingsType::ADS),
            CONTENT_SETTING_BLOCK);

  // And a change in the regular profile after MoltNet started doesn't show.
  regular_map_->SetContentSettingDefaultScope(
      GURL("https://later.example/"), GURL(), ContentSettingsType::JAVASCRIPT,
      CONTENT_SETTING_BLOCK);
  EXPECT_EQ(
      moltnet_map_->GetContentSetting(GURL("https://later.example/"), GURL(),
                                      ContentSettingsType::JAVASCRIPT),
      CONTENT_SETTING_ALLOW);
}

}  // namespace
}  // namespace molt_ai::tor
