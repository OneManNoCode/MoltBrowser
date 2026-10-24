// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which of the regular profile's settings a MoltNet window must not use.
// Pure (std only) so molt_ai_unittests tests it standalone; the browser side
// that applies it is molt_net_pref_isolation.{h,cc}.
//
// MoltNet runs in an off-the-record profile of the user's regular profile,
// and an off-the-record profile reads every pref it hasn't written itself
// from its parent. For the prefs below, that let pages see the user's own
// settings: the same values in every MoltNet session and after every New
// Identity, and the same as in the user's regular windows, so a stable
// per-user identifier that also links MoltNet to the regular identity. When
// MoltNet's profile is created, each of these prefs gets MoltBrowser's
// registered default in MoltNet's own in-memory store instead, so the
// regular profile's value never shows through.
//
// A pref is listed when web content can observe its value (directly,
// through a request, or through what a page is allowed to do) and the user
// (a setting, a site's page-info controls, a keyboard shortcut) or Chromium
// on the user's behalf (permission embargo, engagement) can change it in the
// regular profile. Values an administrator sets by policy are not the user's
// own and still apply; a pin couldn't override them anyway. Prefs pages
// can't observe (bookmarks bar, download folder, spell checking, Safe
// Browsing level, ...) keep following the regular profile.
//
// Kept: what changes inside the MoltNet session (in memory, gone with the
// profile), and what MoltNet itself sets afterwards (molt_net_routing.cc:
// proxy, WebRTC policy, prediction, HTTPS-Only). MoltNet's languages are
// fixed above the profile's own store (molt_net_language_policy.h).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PREF_ISOLATION_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PREF_ISOLATION_POLICY_H_

#include <array>
#include <string_view>

namespace molt_ai::tor {

// Every pref whose name starts with one of these. Each ends in '.', so a
// prefix only matches whole name components.
inline constexpr auto kMoltNetPinnedPrefPrefixes =
    std::to_array<std::string_view>({
        // WebPreferences: the font family of every generic family for every
        // script, default and minimum font sizes, the JavaScript, image and
        // plugin switches, force-dark, Tab-to-links, resizable text areas,
        // Encrypted Media.
        "webkit.webprefs.",
        // Content and website settings of every type: per-site exceptions
        // (permissions, cookies, JavaScript, pop-ups, sound, MoltShield's
        // "Shields off" (ADS), ...) and per-site data Chromium keeps there
        // (permission embargo, engagement scores, ...).
        "profile.content_settings.exceptions.",
        "profile.content_settings.partitioned_exceptions.",
        // The default of every content setting type.
        "profile.default_content_setting_values.",
        // MoltShield's per-profile switches ("Strip tracking parameters").
        "molt_shield.",
    });

// Single prefs.
inline constexpr auto kMoltNetPinnedPrefNames =
    std::to_array<std::string_view>({
        // Permission prompt history (adaptive quiet prompts).
        "profile.content_settings.permission_actions",
        // Third-party cookie blocking: the mode (block, allow, or block in
        // Incognito only, which CookieSettings answers with the profile's
        // Incognito flag; cookie_settings_factory.cc gives MoltNet
        // Incognito's). Pinned, so the regular profile's mode never
        // decides. (Incognito writes it through to the regular profile;
        // MoltNet's pref service writes nothing through.)
        "profile.cookie_controls_mode",
        // Accept-Language and navigator.languages. MoltNet's pref service
        // then also holds MoltNet's own list (en-US while fingerprinting
        // protection is on) in the store above this one, where neither
        // LanguagePrefs (the ForcedLanguages policy, the user's selected
        // languages) nor anything else can change it
        // (molt_net_language_policy.h).
        "intl.accept_languages",
        // The encoding of pages that don't declare one (document.characterSet).
        "intl.charset_default",
        // The DNT request header and navigator.doNotTrack.
        "enable_do_not_track",
        // Which addresses WebRTC may use. MoltNet sets
        // disable_non_proxied_udp itself right after creating the profile.
        "webrtc.ip_handling_policy",
        // Accessibility settings pages can detect: whether animated images
        // animate, caret browsing (the selection follows the arrow keys),
        // focus rings on mouse focus (:focus-visible), sites exempt from
        // forced colors, and whether pages may style scrollbars.
        "settings.a11y.animation_policy",
        "settings.a11y.caretbrowsing.enabled",
        "settings.a11y.focus_highlight",
        "settings.a11y.page_colors_block_list",
        "settings.a11y.prefers_default_scrollbar_styles",
        // PaymentRequest.canMakePayment() and hasEnrolledInstrument().
        "payments.can_make_payment_enabled",
        // The sites allowed to open an application without asking ("Always
        // allow <site> to open links of this type"). In MoltNet every
        // launch asks: the application is outside Tor, and whether a page
        // can open it without a prompt shows a choice made in the regular
        // profile.
        "protocol_handler.allowed_origin_protocol_pairs",
        // "Use a web service to help resolve navigation errors", which
        // switches on captive-portal detection. Its checks would go through
        // Tor, where no network sign-in page can be reached, and whether
        // they happen shows the regular profile's choice. Pinned to
        // MoltBrowser's default (off, molt_privacy_prefs.cc), so MoltNet
        // never checks for a captive portal; and MoltNet never opens a
        // sign-in page (chrome_security_blocking_page_factory.cc).
        "alternate_error_pages.enabled",
        // Protected Audience, Attribution Reporting and Topics. Removed
        // upstream at 156: no web API reads these prefs any more (only the
        // settings page and policy do). Still pinned, which costs nothing.
        "privacy_sandbox.m1.topics_enabled",
        "privacy_sandbox.m1.fledge_enabled",
        "privacy_sandbox.m1.ad_measurement_enabled",
        // Whether pages that use XSLT show Blink's deprecation banner (new
        // at 156, WebPreferences::is_xslt_deprecation_banner_suppressed).
        // Blink inserts the banner into the page's DOM, so a page can tell
        // whether the user chose "Never show again" in a regular window.
        // Pinned to the default, banner shown, as in every new profile; its
        // text is one of the renderer strings MoltNet pins to en-US
        // (chrome/renderer/molt_ai/fingerprinting/
        // moltnet_locale_strings_table.cc). "Never show again" in a MoltNet
        // window lasts for that session.
        "profile.xslt_deprecation_banner_suppressed",
        // The Global Privacy Control setting (new at 156, behind flags):
        // RendererPreferences::is_global_privacy_control_setting_enabled,
        // the Sec-GPC header and navigator.globalPrivacyControl. MoltBrowser
        // sends GPC from every window anyway (kGlobalPrivacyControlForce,
        // molt_feature_overrides.cc), which this setting can't undo; if that
        // flag is turned off, MoltNet follows the default, not the regular
        // profile's choice.
        "universal_optout.enabled",
        // The keyboard shortcut the user set for Autofill's AtMemory (new at
        // 156, RendererPreferences::autofill_shortcut_key_code). The renderer
        // cancels that key's keydown in text fields, which a page can see.
        "autofill.at_memory.shortcut",
    });

// True iff MoltNet must use MoltBrowser's registered default of
// `pref_name`, not the regular profile's value: an exact entry of
// kMoltNetPinnedPrefNames, or a name that starts with an entry of
// kMoltNetPinnedPrefPrefixes and goes on after it. ASCII, case-sensitive,
// like pref names.
bool IsMoltNetPinnedPref(std::string_view pref_name);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PREF_ISOLATION_POLICY_H_
