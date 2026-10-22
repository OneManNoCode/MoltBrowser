// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the browser's UI language in MoltNet
// windows. Configure(kUniform) already pins navigator.language(s) (through
// the profile's Accept-Language), Intl and Date (LocaleController) and the
// formats of form controls (moltnet_uniformity.h) to en-US. Three more
// things followed the UI language:
//
//   * The default text locale. DefaultLanguage() (platform/language.h) is
//     the renderer's --lang, the browser's UI language. Text without a lang
//     attribute is laid out with it (LayoutLocale::GetDefault()): the Han
//     glyph variants of CJK text when Accept-Language doesn't decide
//     (LayoutLocale::LocaleForHan), hyphenation, line breaking, case
//     mapping and the quotes of <q>. It is also the language of the date
//     picker and the only entry of UserPreferredLanguages(), which picks
//     the default text track of a video for users who asked for captions.
//     PinUniformTextLocale() makes it en-US for the whole process, on every
//     thread (one hook in language.cc).
//   * Strings Blink asks the embedder for (Platform::QueryLocalizedString:
//     validationMessage, the file input's labels, the <details> and button
//     defaults, ...). They come from the embedder's resource bundle, so the
//     embedder pins them: chrome/renderer/molt_ai/fingerprinting/
//     moltnet_locale_strings.h, also in kUniform only.
//   * The font settings of generic families, font sizes and the default
//     encoding. Those are browser prefs: MoltNet's profile uses fixed per-OS
//     values (chrome/browser/molt_ai/tor/molt_net_locale_prefs.h).
//
// kUniform only, like the time zone and the Intl locale: with the kill
// switch off MoltNet's Accept-Language is the UI language's default list
// again, which names the language outright.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_LOCALE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_LOCALE_H_

#include "third_party/blink/renderer/core/core_export.h"

namespace blink::molt_fingerprinting {

// DefaultLanguage() and UserPreferredLanguages() return kUniformLocale
// ("en-US") from now on, on every thread. Called once by Configure(kUniform)
// on the main thread, before any document or worker exists and before any
// text is laid out (LayoutLocale caches the default per thread).
CORE_EXPORT void PinUniformTextLocale();

// Undoes PinUniformTextLocale() (tests only; LayoutLocale's per-thread
// caches are the caller's to clear).
CORE_EXPORT void UnpinTextLocaleForTesting();

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_LOCALE_H_
