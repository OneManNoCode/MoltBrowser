// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: limits which installed ("system")
// fonts web content can see (docs/FINGERPRINTING.md, "Fonts"). fonts.h has
// the rules and the allow-lists; this file applies them to Blink's lookups.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   FontFallbackList::GetFontData, system lookup of a family name
//     -> FontSelector::GetSystemFontData (new virtual; the default is the
//        old FontCache lookup) -> CSSFontSelectorBase override (documents,
//        canvas, OffscreenCanvas and FontFace in workers)
//                                                  -> GetSystemFontData
//   CSSFontSelectorBase::IsPlatformFamilyMatchAvailable
//     (document.fonts.check())           -> IsSystemFontFamilyAvailable
//   LocalFontFaceSource::IsLocalNonBlocking, IsLocalFontAvailable,
//     CreateFontData (@font-face / FontFace src: local())
//                                                  -> RestrictsLocalFontFaces,
//                                                     GetLocalFontFaceData
//   FontAccess::DidGetEnumerationResponse
//     (window.queryLocalFonts())                   -> AllowEnumeratedFont
//
// Per execution context (see SystemFontVisibility):
//   * protection off, non-http(s) top-level site, or Shields off (kFarble):
//     every installed font, exactly as upstream;
//   * kUniform (MoltNet): the allow-list;
//   * kFarble with a stable per-site key: the allow-list plus the site's
//     sample (FontVisibility::IsSampled);
//   * kFarble with a random per-context key (opaque top-level sites, SVG
//     images, third-party shared workers, ...): the allow-list only, so
//     creating many such contexts can't add up their samples.
// Generic families (serif, sans-serif, monospace, cursive, fantasy,
// system-ui, math, -webkit-body) and the fonts the user picked for them in
// settings are never restricted; neither are web fonts (src: url()) or the
// per-character system fallback.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SYSTEM_FONTS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SYSTEM_FONTS_H_

#include <optional>
#include <string_view>

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/css_value_keywords.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"

namespace blink {

class AtomicString;
class ExecutionContext;
class FontDescription;
class FontFamily;
class SimpleFontData;

namespace molt_fingerprinting {

// The fonts `context` may see. Cached per worker and per document (a
// window's entry is recomputed for its next document; MoltShield reloads
// the page when Shields are toggled). A null context in a protecting
// process gets the allow-list.
CORE_EXPORT FontVisibility SystemFontVisibility(ExecutionContext* context);

// The installed font that `family` names, as FontCache::GetFontData() finds
// it, or null when there is none or `context` may not see it. Generic
// families are never restricted.
CORE_EXPORT const SimpleFontData* GetSystemFontData(
    ExecutionContext* context,
    const FontDescription& font_description,
    const FontFamily& family);

// document.fonts.check(): whether the non-generic `family` names a font
// `context` may see, or nullopt when `context` sees every font (then the
// caller keeps its own answer).
CORE_EXPORT std::optional<bool> IsSystemFontFamilyAvailable(
    ExecutionContext* context,
    const FontDescription& font_description,
    const FontFamily& family);

// src: local(). Whether `context` only sees some installed fonts; then the
// unique (full or PostScript) name in local() is never looked up as such,
// installed hidden and missing faces cost the same, and the unique-name
// lookup's caches (FontCache's typeface cache, the platform's table) are
// never used, so they can't tell one document what another looked up.
CORE_EXPORT bool RestrictsLocalFontFaces(ExecutionContext* context);
// Only where RestrictsLocalFontFaces(context): the face local(`unique_name`)
// stands for, found by the family lookups FontVisibility::
// ForLocalFaceRequest() allows (only families `context` may look up in CSS
// anyway), or null. `font_description` is the description the caller would
// look the unique name up with; its weight and slope are replaced by those
// the name gives, if any.
CORE_EXPORT const SimpleFontData* GetLocalFontFaceData(
    ExecutionContext* context,
    const FontDescription& font_description,
    const AtomicString& unique_name);

// queryLocalFonts(): whether to report a font of family `family` (UTF-8, as
// the browser enumerated it).
CORE_EXPORT bool AllowEnumeratedFont(ExecutionContext* context,
                                     std::string_view family);

// CSS system fonts (font: menu, caption, status-bar, ...). In MoltNet every
// keyword resolves as `caption` does, which is the same on every OS (the
// default GUI font at the default size, layout_theme_font_provider*.cc);
// otherwise Windows' menu, small-caption and status-bar fonts (family and
// size from the user's display settings and DPI) would show. Elsewhere the
// keyword itself.
CORE_EXPORT CSSValueID UniformSystemFontKeyword(CSSValueID system_font_id);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_SYSTEM_FONTS_H_
