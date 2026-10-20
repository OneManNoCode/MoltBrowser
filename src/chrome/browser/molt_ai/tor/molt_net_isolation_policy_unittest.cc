// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_isolation_policy.h"

#include <string>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

// The names are Chromium 152's (the constant each comes from is noted);
// molt_net_isolation_unittest.cc checks the ones it can against the
// constants themselves.

TEST(MoltNetIsolationPolicyTest, WebPreferencesArePinned) {
  // prefs::kWebKitStandardFontFamily and its per-script siblings, which
  // PrefsTabHelper registers as "<family>.<ISO 15924 script>".
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.fonts.standard.Zyyy"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.fonts.sansserif.Jpan"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.fonts.fixed.Hang"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.fonts.math.Zyyy"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.fonts.cursive.Hans"));
  // Sizes.
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.default_font_size"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.default_fixed_font_size"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.minimum_font_size"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.minimum_logical_font_size"));
  // Switches.
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.javascript_enabled"));
  EXPECT_TRUE(
      IsMoltNetPinnedPref("webkit.webprefs.loads_images_automatically"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.tabs_to_links"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.force_dark_mode_enabled"));
  EXPECT_TRUE(IsMoltNetPinnedPref("webkit.webprefs.encrypted_media_enabled"));
}

TEST(MoltNetIsolationPolicyTest, EveryContentSettingIsPinned) {
  // WebsiteSettingsInfo::pref_name(), partitioned_pref_name() and
  // default_value_pref_name() ('-' in the type's name becomes '_').
  // "subresource_filter" is ContentSettingsType::ADS, MoltShield's per-site
  // "Shields off"; "window_placement" is WINDOW_MANAGEMENT.
  for (const char* type :
       {"javascript", "images", "popups", "subresource_filter", "cookies",
        "geolocation", "notifications", "media_stream_camera",
        "window_placement", "permission_autoblocking_data", "site_engagement",
        "storage_access", "a_type_added_after_chromium_152"}) {
    SCOPED_TRACE(type);
    EXPECT_TRUE(IsMoltNetPinnedPref(
        std::string("profile.content_settings.exceptions.") + type));
    EXPECT_TRUE(IsMoltNetPinnedPref(
        std::string("profile.content_settings.partitioned_exceptions.") +
        type));
    EXPECT_TRUE(IsMoltNetPinnedPref(
        std::string("profile.default_content_setting_values.") + type));
  }
  // permissions::prefs::kPermissionActions.
  EXPECT_TRUE(
      IsMoltNetPinnedPref("profile.content_settings.permission_actions"));
}

TEST(MoltNetIsolationPolicyTest, OtherWebVisiblePrefsArePinned) {
  for (const char* name : {
           "profile.cookie_controls_mode",    // prefs::kCookieControlsMode
           "intl.accept_languages",           // language::prefs::
           "intl.charset_default",            // prefs::kDefaultCharset
           "enable_do_not_track",             // prefs::kEnableDoNotTrack
           "webrtc.ip_handling_policy",       // prefs::kWebRTCIPHandling...
           "settings.a11y.animation_policy",  // prefs::kAnimationPolicy
           "settings.a11y.caretbrowsing.enabled",
           "settings.a11y.focus_highlight",
           "settings.a11y.page_colors_block_list",
           "settings.a11y.prefers_default_scrollbar_styles",
           "payments.can_make_payment_enabled",  // payments::kCanMake...
           "privacy_sandbox.m1.topics_enabled",
           "privacy_sandbox.m1.fledge_enabled",
           "privacy_sandbox.m1.ad_measurement_enabled",
           "molt_shield.remove_tracking_params",
       }) {
    EXPECT_TRUE(IsMoltNetPinnedPref(name)) << name;
  }
}

TEST(MoltNetIsolationPolicyTest, BrowserOnlyPrefsFollowTheRegularProfile) {
  for (const char* name : {
           // MoltNet sets these itself (molt_net_routing.cc).
           "proxy",
           "net.network_prediction_options",
           "https_only_mode_enabled",
           // Not visible to pages.
           "bookmark_bar.show_on_all_tabs",
           "download.default_directory",
           "browser.enable_spellchecking",
           "translate.enabled",
           "safebrowsing.enabled",
           "browser.theme.color_scheme2",
           // Privacy choices about the browser's own prompts and services:
           // pinning them to the default could undo a user's opt-out.
           "profile.content_settings.enable_cpss.notifications",
           "profile.content_settings.enable_quiet_permission_ui.notifications",
           // Policy-only (managed store, not the user's).
           "profile.managed_default_content_settings.javascript",
           "webrtc.udp_port_range",
           // Related Website Sets are off in every off-the-record profile.
           "privacy_sandbox.first_party_sets_enabled",
           // Caption styles stay inside the video's own UI.
           "accessibility.captions.text_size",
       }) {
    EXPECT_FALSE(IsMoltNetPinnedPref(name)) << name;
  }
}

TEST(MoltNetIsolationPolicyTest, PrefixesMatchWholeComponentsOnly) {
  // The bare prefix (no name after it) is not a pref.
  EXPECT_FALSE(IsMoltNetPinnedPref("webkit.webprefs."));
  EXPECT_FALSE(IsMoltNetPinnedPref("webkit.webprefs"));
  EXPECT_FALSE(IsMoltNetPinnedPref("molt_shield."));
  EXPECT_FALSE(IsMoltNetPinnedPref("profile.content_settings.exceptions"));
  // Look-alikes that only share the start of a component.
  EXPECT_FALSE(IsMoltNetPinnedPref("webkit.webprefsx.fonts"));
  EXPECT_FALSE(IsMoltNetPinnedPref("molt_shield_extra.remove_tracking_params"));
  EXPECT_FALSE(IsMoltNetPinnedPref("profile.content_settings.exceptionsx.ads"));
  EXPECT_FALSE(IsMoltNetPinnedPref("xwebkit.webprefs.fonts.standard.Zyyy"));
  // Exact names are exact.
  EXPECT_FALSE(IsMoltNetPinnedPref("enable_do_not_track.x"));
  EXPECT_FALSE(IsMoltNetPinnedPref("intl.accept_languages_extra"));
  EXPECT_FALSE(IsMoltNetPinnedPref("intl"));
  EXPECT_FALSE(IsMoltNetPinnedPref("Enable_Do_Not_Track"));
  EXPECT_FALSE(IsMoltNetPinnedPref(""));
  EXPECT_FALSE(IsMoltNetPinnedPref("."));
  // An embedded NUL doesn't cut a name short.
  EXPECT_FALSE(
      IsMoltNetPinnedPref(std::string_view("enable_do_not_track\0x", 21)));
  EXPECT_TRUE(
      IsMoltNetPinnedPref(std::string_view("webkit.webprefs.a\0b", 19)));
}

TEST(MoltNetIsolationPolicyTest, ListsAreWellFormed) {
  for (std::string_view prefix : kMoltNetPinnedPrefPrefixes) {
    ASSERT_FALSE(prefix.empty());
    EXPECT_EQ(prefix.back(), '.') << prefix;
    // A prefix isn't also matched by another (redundant) prefix.
    for (std::string_view other : kMoltNetPinnedPrefPrefixes) {
      if (other != prefix) {
        EXPECT_NE(prefix.substr(0, other.size()), other) << prefix;
      }
    }
  }
  for (std::string_view name : kMoltNetPinnedPrefNames) {
    ASSERT_FALSE(name.empty());
    EXPECT_NE(name.back(), '.') << name;
    // An exact name isn't already covered by a prefix.
    for (std::string_view prefix : kMoltNetPinnedPrefPrefixes) {
      EXPECT_NE(name.substr(0, prefix.size()), prefix) << name;
    }
    size_t count = 0;
    for (std::string_view other : kMoltNetPinnedPrefNames) {
      if (other == name) {
        ++count;
      }
    }
    EXPECT_EQ(count, 1u) << name;
  }
}

}  // namespace
}  // namespace molt_ai::tor
