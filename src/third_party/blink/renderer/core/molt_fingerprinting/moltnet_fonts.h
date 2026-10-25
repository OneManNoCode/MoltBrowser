// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: MoltNet's bundled fonts, the Blink
// side (docs/FINGERPRINTING.md, "Fonts"). The rules are moltnet_font_set.h;
// this file loads the files, makes the typefaces and answers Blink's font
// code through FontPlatformData::UniformFontSet.
//
// A MoltNet renderer whose browser has the set (the browser puts
// --molt-net-fonts on its command line) calls ExpectBundledFonts() at start:
// from then on every font of the process comes from the set, on every OS:
// family lookups, local() faces, per-character fallback and the last-resort
// font (font_platform_data.h lists the hooks). The files arrive right after
// launch, over the channel, as read-only handles (SetBundledFontFiles()):
// the renderer never opens a font file itself, on any OS. Until they do (in
// practice: never, as they come before any navigation), lookups find
// nothing and text is drawn with an empty font, never a system one.
//
// Typefaces are made by Skia's Fontations backend from the mapped files, on
// every OS (no CoreText, DirectWrite or fontconfig), so the same files are
// read by the same code everywhere; variable faces are instantiated at the
// weight asked for.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_FONTS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_FONTS_H_

#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace blink::molt_fingerprinting {

namespace moltnet {
struct BundledFontTable;
}  // namespace moltnet

// Installs the (empty) bundled set as the process's only fonts. Main
// thread, in a kUniform renderer, before any font is created; only the
// first call has an effect.
CORE_EXPORT void ExpectBundledFonts();

// Whether ExpectBundledFonts() was called: every font of this process comes
// from the bundled set (visibility uses FontPlatform::kMoltNetBundled).
CORE_EXPORT bool BundledFontsActive();

// The set's files, in the order of the generated table (the browser sends
// them so). Main thread. Accepted once, and only if every file has its
// table's size and is a font; then fonts are looked up again. Returns
// whether they were accepted.
CORE_EXPORT bool SetBundledFontFiles(std::vector<base::File> files);

// queryLocalFonts() in MoltNet: the set's faces, the same on every OS.
struct BundledFontFaceInfo {
  std::string_view postscript_name;
  std::string_view full_name;
  std::string_view family;
  std::string_view style;
};
CORE_EXPORT std::vector<BundledFontFaceInfo> BundledFontFaces();

CORE_EXPORT void ResetBundledFontsForTesting();
// Tests: resets, then uses `table` (which must outlive its use) instead of
// the generated one, so tests can load small fonts of their own.
CORE_EXPORT void UseBundledFontTableForTesting(
    const moltnet::BundledFontTable& table);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_FONTS_H_
