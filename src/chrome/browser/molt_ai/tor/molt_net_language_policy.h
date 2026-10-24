// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's languages: the same list for every MoltNet user, whatever the
// user, an administrator or a command-line flag sets.
//
// What pages read: the Accept-Language header (ProfileNetworkContextService),
// navigator.language(s) (renderer_preferences_util.cc) and the languages
// Reduce Accept-Language negotiates from all come from the profile's
// "intl.accept_languages" (for an off-the-record profile through
// language::GetIncognitoLanguageList()). MoltNet's settings store pins it
// (molt_net_pref_isolation_policy.h), but at Chromium 156 that pin alone
// doesn't hold:
//
//   * language::LanguagePrefs rewrites "intl.accept_languages" of the
//     PrefService it is made for, whenever it is made and whenever its
//     inputs change, as the policy's forced languages ("intl.forced_languages",
//     the ForcedLanguages policy, per profile and dynamic) followed by the
//     user's own list ("intl.selected_languages"). Every TranslatePrefs makes
//     one, and translate makes a TranslatePrefs for a tab's profile each time
//     it learns a page's language, so a MoltNet tab's first page load made
//     MoltNet's languages the policy's languages plus the regular profile's
//     selected list (MoltNet's store doesn't hold "intl.selected_languages",
//     so it reads the regular profile's).
//   * The --accept-lang switch sets "intl.selected_languages" in the
//     command-line store, which every profile shares; LanguagePrefs then
//     does the same.
//
// So MoltNet holds both prefs in the store that ranks above its own settings
// store (Chromium's extension-controlled layer, which in MoltNet's pref
// service holds no extension's setting: molt_net_isolation.h), for the
// whole life of the profile. Whatever LanguagePrefs or anything else writes
// to MoltNet's own store then has no effect, and the regular profile's list
// is never read. ForcedLanguages and --accept-lang are ignored rather than
// refused: a language is not a routing setting, and nothing else reaches a
// page through them (Chromium reads "intl.forced_languages" only in
// LanguagePrefs, the settings page and the spellcheck policy handler).
// No policy sets "intl.accept_languages" itself.
//
// ReduceAcceptLanguageEnabled (per profile, dynamic) also reaches pages: if
// an administrator sets it to false, Chromium puts
// --disable-reduce-accept-language on the profile's renderer command lines
// and ChromeContentBrowserClient::ShouldReduceAcceptLanguage() answers false,
// so navigator.languages and the header keep the whole list where the
// Reduce Accept-Language trial would cut it to one language. MoltNet
// behaves as if the policy weren't set, as every other MoltNet user's does
// (ReduceAcceptLanguageEnabledFor(), asked by two hooks in
// chrome_content_browser_client.cc).
//
// Header-only and std-only, so molt_ai_unittests tests it.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LANGUAGE_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LANGUAGE_POLICY_H_

#include <array>
#include <string>
#include <string_view>

namespace molt_ai::tor {

// The list every MoltNet window uses while fingerprinting protection is on.
// Off the record, Chromium sends the generic list it builds from the first
// language (language::GetIncognitoLanguageList()), which for "en-US" is
// this one, so pages see en-US in navigator.language(s) and Accept-Language.
inline constexpr std::string_view kMoltNetUniformLanguages = "en-US,en";

// The prefs MoltNet fixes above its own settings store
// (language::prefs::kAcceptLanguages, language::prefs::kSelectedLanguages).
// "intl.selected_languages" gets the same list, so a LanguagePrefs made for
// MoltNet's prefs computes MoltNet's list again (no forced languages) and
// never sees the user's.
inline constexpr auto kMoltNetFixedLanguagePrefs =
    std::to_array<std::string_view>({
        "intl.accept_languages",
        "intl.selected_languages",
    });

// True iff `pref_name` is one of kMoltNetFixedLanguagePrefs (exact, ASCII,
// case-sensitive, like pref names).
constexpr bool IsMoltNetFixedLanguagePref(std::string_view pref_name) {
  for (std::string_view name : kMoltNetFixedLanguagePrefs) {
    if (pref_name == name) {
      return true;
    }
  }
  return false;
}

// MoltNet's language list: kMoltNetUniformLanguages while fingerprinting
// protection is on (`uniform`), otherwise `registered_default`, Chromium's
// registered default of "intl.accept_languages" (the UI language's
// IDS_ACCEPT_LANGUAGES), which is what MoltNet's settings isolation pins
// with the kill switch off. Never the user's list or a policy's.
inline std::string MoltNetLanguageList(bool uniform,
                                       std::string_view registered_default) {
  return std::string(uniform ? kMoltNetUniformLanguages : registered_default);
}

// The value ChromeContentBrowserClient uses for the
// ReduceAcceptLanguageEnabled policy's pref ("enabled": no
// --disable-reduce-accept-language, ShouldReduceAcceptLanguage() true, which
// is also what an unset policy gives). `chromium_value` is what Chromium
// computed for the profile. MoltNet profiles always get the unset policy's
// answer; every other profile gets Chromium's.
constexpr bool ReduceAcceptLanguageEnabledFor(bool is_moltnet_profile,
                                              bool chromium_value) {
  return is_moltnet_profile ? true : chromium_value;
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_LANGUAGE_POLICY_H_
