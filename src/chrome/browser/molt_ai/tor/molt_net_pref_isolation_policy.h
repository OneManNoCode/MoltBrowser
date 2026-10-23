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
// proxy, WebRTC policy, prediction, HTTPS-Only, en-US languages).

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
        // Third-party cookie blocking. CookieSettings only treats the
        // primary Incognito profile as Incognito, so in MoltNet this pref
        // alone decides. (Incognito writes it through to the regular
        // profile; MoltNet's pref service writes nothing through.)
        "profile.cookie_controls_mode",
        // Accept-Language and navigator.languages. MoltNet then sets en-US
        // itself while fingerprinting protection is on.
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
        // Protected Audience, Attribution Reporting and Topics. Like
        // CookieSettings, PrivacySandboxSettings only switches them off for
        // the primary Incognito profile, so in MoltNet these prefs decide.
        "privacy_sandbox.m1.topics_enabled",
        "privacy_sandbox.m1.fledge_enabled",
        "privacy_sandbox.m1.ad_measurement_enabled",
    });

// True iff MoltNet must use MoltBrowser's registered default of
// `pref_name`, not the regular profile's value: an exact entry of
// kMoltNetPinnedPrefNames, or a name that starts with an entry of
// kMoltNetPinnedPrefPrefixes and goes on after it. ASCII, case-sensitive,
// like pref names.
bool IsMoltNetPinnedPref(std::string_view pref_name);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PREF_ISOLATION_POLICY_H_
