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
#include "chrome/browser/molt_ai/tor/molt_net_language_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_pref_isolation_policy.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "chrome/common/pref_names.h"
#include "components/autofill/core/common/autofill_prefs.h"
#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/browser/website_settings_info.h"
#include "components/content_settings/core/browser/website_settings_registry.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/embedder_support/pref_names.h"
#include "components/language/core/browser/language_prefs.h"
#include "components/language/core/browser/pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/value_map_pref_store.h"
#include "components/privacy_sandbox/privacy_sandbox_prefs.h"
#include "components/sync_preferences/pref_service_syncable.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "components/universal_optout/prefs.h"
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
           prefs::kProtocolHandlerPerOriginAllowedProtocols,
           embedder_support::kAlternateErrorPagesEnabled,
           shield::prefs::kRemoveTrackingParams,
           // New at Chromium 156.
           prefs::kXSLTDeprecationBannerSuppressed,
           universal_optout::prefs::kUniversalOptOutEnabled,
           autofill::prefs::kAutofillAtMemoryShortcut,
       }) {
    EXPECT_TRUE(IsMoltNetPinnedPref(name)) << name;
  }
  // AtMemory's double-Ctrl trigger changes no key event a page sees.
  EXPECT_FALSE(IsMoltNetPinnedPref(
      autofill::prefs::kAutofillAtMemoryDoubleCtrlTriggerEnabled));
  // Related Website Sets are permanently off in off-the-record profiles
  // (FirstPartySetsPolicyService), so their switch isn't pinned.
  EXPECT_FALSE(
      IsMoltNetPinnedPref(prefs::kPrivacySandboxRelatedWebsiteSetsEnabled));
}

// The literals in molt_net_language_policy.h too.
TEST(MoltNetIsolationTest, FixedLanguageNamesMatchChromiumsConstants) {
  EXPECT_TRUE(IsMoltNetFixedLanguagePref(language::prefs::kAcceptLanguages));
  EXPECT_TRUE(IsMoltNetFixedLanguagePref(language::prefs::kSelectedLanguages));
  EXPECT_FALSE(IsMoltNetFixedLanguagePref(language::prefs::kForcedLanguages));
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

  // A change made in a MoltNet window (zooming the default font size up)
  // wins over the pin and stays in MoltNet.
  moltnet->SetInteger(kFontSize, 18);
  EXPECT_EQ(moltnet->GetInteger(kFontSize), 18);
  EXPECT_EQ(regular_.GetInteger(kFontSize), 24);

  // Not the languages: they are fixed above MoltNet's own store
  // (molt_net_language_policy.h), so no write changes them.
  const std::string languages = moltnet->GetString(kLanguages);
  moltnet->SetString(kLanguages, "en-US,en;q=0.9");
  EXPECT_EQ(moltnet->GetString(kLanguages), languages);
  EXPECT_EQ(regular_.GetString(kLanguages), "de-CH,de,fr");

  // Incognito writes the third-party-cookie mode and the bookmarks bar
  // through to the regular profile (its persistent prefs); MoltNet doesn't.
  moltnet->SetInteger(kCookieMode, 2);
  moltnet->SetBoolean(kBookmarkBar, false);
  EXPECT_EQ(regular_.GetInteger(kCookieMode), 0);
  EXPECT_TRUE(regular_.GetBoolean(kBookmarkBar));
}

// "Always allow <site> to open links of this type" from the regular profile
// doesn't let a MoltNet page open an application without asking
// (ExternalProtocolHandler::GetBlockState reads this dictionary), and one
// given in a MoltNet window stays there.
TEST(MoltNetIsolationTest, ExternalProtocolExemptionsStayInTheirProfile) {
  sync_preferences::TestingPrefServiceSyncable regular;
  regular.registry()->RegisterDictionaryPref(
      prefs::kProtocolHandlerPerOriginAllowedProtocols);
  base::DictValue allowed;
  allowed.Set("https://meet.example",
              base::DictValue().Set("examplemeet", true));
  regular.SetDict(prefs::kProtocolHandlerPerOriginAllowedProtocols,
                  allowed.Clone());

  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular, nullptr);
  EXPECT_TRUE(
      moltnet->GetDict(prefs::kProtocolHandlerPerOriginAllowedProtocols)
          .empty());

  moltnet->SetDict(prefs::kProtocolHandlerPerOriginAllowedProtocols,
                   base::DictValue().Set(
                       "https://other.example",
                       base::DictValue().Set("examplemeet", true)));
  EXPECT_EQ(regular.GetDict(prefs::kProtocolHandlerPerOriginAllowedProtocols),
            allowed);
}

// Captive-portal detection follows "Use a web service to help resolve
// navigation errors" (CaptivePortalService reads it from the tab's
// profile). With MoltBrowser's defaults, a regular profile that switched it
// on doesn't switch it on in MoltNet, and a MoltNet window can't switch it
// on for the regular profile.
TEST(MoltNetIsolationTest, CaptivePortalDetectionStaysOffInMoltNet) {
  static constexpr const char* kAlternateErrorPages =
      embedder_support::kAlternateErrorPagesEnabled;
  sync_preferences::TestingPrefServiceSyncable regular;
  // Chromium registers it on (ProfileNetworkContextService); MoltBrowser
  // makes off the default (molt_privacy_prefs.cc), which MoltNet pins.
  regular.registry()->RegisterBooleanPref(kAlternateErrorPages, true);
  regular.registry()->SetDefaultPrefValue(kAlternateErrorPages,
                                          base::Value(false));
  regular.SetBoolean(kAlternateErrorPages, true);

  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular, nullptr);
  EXPECT_FALSE(moltnet->GetBoolean(kAlternateErrorPages));
  // Held in MoltNet's own store, so later changes don't show either.
  EXPECT_TRUE(
      moltnet->FindPreference(kAlternateErrorPages)->IsUserControlled());
  regular.SetBoolean(kAlternateErrorPages, false);
  regular.SetBoolean(kAlternateErrorPages, true);
  EXPECT_FALSE(moltnet->GetBoolean(kAlternateErrorPages));

  regular.SetBoolean(kAlternateErrorPages, false);
  moltnet->SetBoolean(kAlternateErrorPages, true);
  EXPECT_FALSE(regular.GetBoolean(kAlternateErrorPages));
}

// "Never show again" on Blink's XSLT deprecation banner
// (ChromeContentBrowserClient::SuppressXSLTDeprecationBanner sets the pref
// on the frame's profile). One clicked in a regular window doesn't hide the
// banner from MoltNet pages, whose DOM would show it; one clicked in a
// MoltNet window stays in that MoltNet session.
TEST(MoltNetIsolationTest, XsltBannerChoiceStaysInItsProfile) {
  static constexpr const char* kSuppressed =
      prefs::kXSLTDeprecationBannerSuppressed;
  sync_preferences::TestingPrefServiceSyncable regular;
  // As ChromeContentBrowserClient::RegisterProfilePrefs does.
  regular.registry()->RegisterBooleanPref(kSuppressed, false);
  regular.SetBoolean(kSuppressed, true);

  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular, nullptr);
  EXPECT_FALSE(moltnet->GetBoolean(kSuppressed));

  regular.SetBoolean(kSuppressed, false);
  moltnet->SetBoolean(kSuppressed, true);
  EXPECT_TRUE(moltnet->GetBoolean(kSuppressed));
  EXPECT_FALSE(regular.GetBoolean(kSuppressed));
}

// The ForcedLanguages policy, the user's selected languages and the
// --accept-lang switch reach "intl.accept_languages" only through
// language::LanguagePrefs, which every TranslatePrefs makes for the tab's
// profile (each time translate learns a page's language). For MoltNet, none
// of them changes the list pages see, also while the window is open.
TEST(MoltNetLanguagesTest, NoPolicyOrUserListReachesMoltNet) {
  sync_preferences::TestingPrefServiceSyncable regular;
  language::LanguagePrefs::RegisterProfilePrefs(regular.registry());
  // The user's own languages, as the settings page stores them.
  regular.SetString(language::prefs::kSelectedLanguages, "de-CH,de,fr");
  regular.SetString(language::prefs::kAcceptLanguages, "de-CH,de,fr");
  // The ForcedLanguages policy.
  regular.SetManagedPref(language::prefs::kForcedLanguages,
                         base::ListValue().Append("ja"));

  std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
      CreateMoltNetPrefService(&regular, nullptr);
  const base::Value* registered =
      moltnet->GetDefaultPrefValue(language::prefs::kAcceptLanguages);
  ASSERT_TRUE(registered && registered->is_string());
  const std::string expected = MoltNetLanguageList(
      fingerprinting::IsFingerprintingProtectionEnabled(),
      registered->GetString());
  EXPECT_EQ(moltnet->GetString(language::prefs::kAcceptLanguages), expected);
  EXPECT_EQ(moltnet->GetString(language::prefs::kSelectedLanguages), expected);

  {
    // What TranslatePrefs does for a MoltNet tab.
    language::LanguagePrefs language_prefs(moltnet.get());
    EXPECT_EQ(moltnet->GetString(language::prefs::kAcceptLanguages),
              expected);
    // A policy change while MoltNet is open, and a list changed in it.
    regular.SetManagedPref(language::prefs::kForcedLanguages,
                           base::ListValue().Append("ko").Append("pt-BR"));
    moltnet->SetString(language::prefs::kSelectedLanguages, "it");
    language_prefs.SetUserSelectedLanguagesList({"es", "fr"});
    EXPECT_EQ(moltnet->GetString(language::prefs::kAcceptLanguages),
              expected);
    // The regular profile's list is never read.
    regular.SetString(language::prefs::kSelectedLanguages, "nl");
    EXPECT_EQ(moltnet->GetString(language::prefs::kAcceptLanguages),
              expected);
  }

  // The regular profile keeps Chromium's behaviour: the policy's languages
  // first, then the user's, and nothing MoltNet did shows there.
  language::LanguagePrefs regular_language_prefs(&regular);
  EXPECT_EQ(regular.GetString(language::prefs::kAcceptLanguages),
            "ko,pt-BR,nl");
}

// The list is en-US with fingerprinting protection on, else the registered
// default; it lands in the store above MoltNet's own and only for registered
// string prefs.
TEST(MoltNetLanguagesTest, FixedListFollowsTheKillSwitchOnly) {
  for (const bool uniform : {true, false}) {
    sync_preferences::TestingPrefServiceSyncable regular;
    // A German UI's default (IDS_ACCEPT_LANGUAGES).
    regular.registry()->RegisterStringPref(language::prefs::kAcceptLanguages,
                                           "de-DE,de,en-US,en");
    regular.registry()->RegisterStringPref(
        language::prefs::kSelectedLanguages, std::string());
    regular.SetString(language::prefs::kAcceptLanguages, "fr");
    auto top_store = base::MakeRefCounted<ValueMapPrefStore>();
    std::unique_ptr<sync_preferences::PrefServiceSyncable> moltnet =
        regular.CreateIncognitoPrefService(top_store, {});
    EXPECT_EQ(FixMoltNetLanguages(moltnet.get(), top_store.get(), uniform),
              2u);
    const char* expected = uniform ? "en-US,en" : "de-DE,de,en-US,en";
    EXPECT_EQ(moltnet->GetString(language::prefs::kAcceptLanguages),
              expected);
    EXPECT_EQ(moltnet->GetString(language::prefs::kSelectedLanguages),
              expected);
    EXPECT_TRUE(moltnet->FindPreference(language::prefs::kAcceptLanguages)
                    ->IsExtensionControlled());
    moltnet->SetString(language::prefs::kAcceptLanguages, "fr");
    EXPECT_EQ(moltnet->GetString(language::prefs::kAcceptLanguages),
              expected);
  }
  // Unregistered (or not a string): nothing written.
  sync_preferences::TestingPrefServiceSyncable other;
  other.registry()->RegisterIntegerPref(language::prefs::kSelectedLanguages,
                                        0);
  auto top_store = base::MakeRefCounted<ValueMapPrefStore>();
  EXPECT_EQ(FixMoltNetLanguages(&other, top_store.get(), true), 0u);
  EXPECT_TRUE(top_store->GetValues().empty());
  EXPECT_EQ(FixMoltNetLanguages(nullptr, top_store.get(), true), 0u);
  EXPECT_EQ(FixMoltNetLanguages(&other, nullptr, true), 0u);
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
