/*
 * Copyright (c) 2006, 2007, 2008, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_PLATFORM_DATA_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_PLATFORM_DATA_H_

#include "build/build_config.h"
#include "third_party/blink/public/platform/web_font_render_style.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"
#include "third_party/blink/renderer/platform/fonts/font_orientation.h"
#include "third_party/blink/renderer/platform/fonts/resolved_font_features.h"
#include "third_party/blink/renderer/platform/fonts/small_caps_iterator.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/hash_table_deleted_value_type.h"
#include "third_party/blink/renderer/platform/wtf/text/string_impl.h"
#include "third_party/blink/renderer/platform/wtf/threading.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "third_party/skia/include/core/SkTypeface.h"

#if BUILDFLAG(IS_MAC)
using CTFontRef = const struct __CTFont*;
#endif  // BUILDFLAG(IS_MAC)

namespace blink {

class HarfBuzzFace;
class SimpleFontData;  // MoltBrowser
// MoltBrowser: UniformFontSet (font_cache.h, font_fallback_priority.h).
class FontFaceCreationParams;
enum class AlternateFontName;
enum class FontFallbackPriority;

class PLATFORM_EXPORT FontPlatformData
    : public GarbageCollected<FontPlatformData> {
 public:
  // Used for deleted values in the font cache's hash tables. The hash table
  // will create us with this structure, and it will compare other values
  // to this "Deleted" one. It expects the Deleted one to be differentiable
  // from the 0 one (created with the empty constructor), so we can't just
  // set everything to 0.
  FontPlatformData(HashTableDeletedValueType);
  FontPlatformData();
  FontPlatformData(const FontPlatformData&);
  FontPlatformData(const FontPlatformData& src, float text_size);
  FontPlatformData(const sk_sp<SkTypeface>,
                   const std::string& name,
                   float text_size,
                   bool synthetic_bold,
                   bool synthetic_italic,
                   TextRenderingMode text_rendering,
                   ResolvedFontFeatures resolved_font_features,
                   FontOrientation = FontOrientation::kHorizontal);
  ~FontPlatformData();

  void Trace(Visitor*) const;

#if BUILDFLAG(IS_MAC)
  // Returns nullptr for FreeType backed SkTypefaces, compare
  // FontCustomPlatformData, which are used for variable fonts on Mac OS
  // <10.12. It should not return nullptr otherwise. So it allows distinguishing
  // which backend the SkTypeface is using.
  CTFontRef CtFont() const;
#endif

  String FontFamilyName() const;
  float size() const { return text_size_; }
  bool SyntheticBold() const { return synthetic_bold_; }
  bool SyntheticItalic() const { return synthetic_italic_; }

  SkTypeface* Typeface() const;
  sk_sp<SkTypeface> TypefaceSp() const { return typeface_; }
  HarfBuzzFace* GetHarfBuzzFace() const;
  bool HasSpaceInLigaturesOrKerning(TypesettingFeatures) const;
  SkTypefaceID UniqueID() const;
  uint32_t GetHash() const;

  FontOrientation Orientation() const { return orientation_; }
  const ResolvedFontFeatures& ResolvedFeatures() const {
    return resolved_font_features_;
  }
  bool IsVerticalAnyUpright() const {
    return blink::IsVerticalAnyUpright(orientation_);
  }
  bool IsVerticalNonCJKUpright() const {
    return blink::IsVerticalNonCJKUpright(orientation_);
  }
  void SetOrientation(FontOrientation orientation) {
    orientation_ = orientation;
  }
  void SetSyntheticBold(bool synthetic_bold) {
    synthetic_bold_ = synthetic_bold;
  }
  void SetSyntheticItalic(bool synthetic_italic) {
    synthetic_italic_ = synthetic_italic;
  }
  void SetAvoidEmbeddedBitmaps(bool embedded_bitmaps) {
    avoid_embedded_bitmaps_ = embedded_bitmaps;
  }
  bool operator==(const FontPlatformData&) const;
  FontPlatformData& operator=(const FontPlatformData&) = delete;

  bool IsHashTableDeletedValue() const { return is_hash_table_deleted_value_; }
#if !BUILDFLAG(IS_MAC)
  bool FontContainsCharacter(UChar32 character) const;
#endif

#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
  const WebFontRenderStyle& GetFontRenderStyle() const { return style_; }
#endif

  SkFont CreateSkFont(const FontDescription* = nullptr) const;

  // Gets the postscript name from the typeface.
  String GetPostScriptName() const;

  // MoltBrowser: how MoltNet renderers render text (Linux and ChromeOS), set
  // once on the main thread before any font is created
  // (molt_fingerprinting/moltnet_text.h).
  struct UniformTextRendering {
    // Every font as for text-rendering: geometricPrecision.
    bool geometric_precision = false;
    // The family emoji fall back to first (a string literal), or null.
    const char* emoji_family = nullptr;
  };
  static void SetUniformTextRendering(const UniformTextRendering& rendering);
  static UniformTextRendering GetUniformTextRendering();
  // The emoji family's font for `description`, or null when there is no
  // such family or it isn't installed.
  static const SimpleFontData* UniformEmojiFontData(
      const FontDescription& description);

  // MoltBrowser: MoltNet's bundled fonts (molt_fingerprinting/
  // moltnet_fonts.h). In a MoltNet renderer that has them, every font the
  // process finds, by family, by unique name, by per-character fallback or
  // as the last resort, comes from this set and from nothing else, on every
  // OS: each platform's FontCache::CreateFontPlatformData,
  // PlatformFallbackFontForCharacter and GetLastResortFallbackFont, and
  // FontSelector::FamilyNameFromSettings, ask it first. Installed once on
  // the main thread before any font is created; never destroyed; its
  // methods may be called on any thread.
  class UniformFontSet {
   public:
    // The typeface the family name `family` (a family of the set, another
    // of its names, an alias such as Arial, or a generic keyword such as
    // sans-serif or system-ui) asks for in the weight and slope of
    // `description`, or null.
    virtual sk_sp<SkTypeface> MatchFamily(
        const AtomicString& family,
        const FontDescription& description) const = 0;
    // The face whose PostScript or full name is `name`, or null.
    virtual sk_sp<SkTypeface> MatchUniqueName(
        const AtomicString& name) const = 0;
    // What FontSelector::FamilyNameFromSettings returns: the family of the
    // set that `generic_family_name` (or `description`'s standard font)
    // maps to for text in `description`'s script, or the empty atom for a
    // generic family the settings don't map (system-ui).
    virtual AtomicString GenericFamily(
        const FontDescription& description,
        const AtomicString& generic_family_name) const = 0;
    // The family of the set to draw `character` with when no font of the
    // page's list has it, or null when none of the set has it.
    virtual AtomicString FallbackFamily(
        const FontDescription& description,
        UChar32 character,
        FontFallbackPriority fallback_priority) const = 0;
    // The family of the last-resort font for `description`.
    virtual AtomicString LastResortFamily(
        const FontDescription& description) const = 0;

   protected:
    virtual ~UniformFontSet() = default;
  };
  static void SetUniformFontSet(const UniformFontSet* set);
  static const UniformFontSet* GetUniformFontSet();
  // FontCache::CreateFontPlatformData with the set, the same on every OS.
  static const FontPlatformData* CreateUniformFontPlatformData(
      const UniformFontSet& set,
      const FontDescription& description,
      const FontFaceCreationParams& creation_params,
      float font_size,
      AlternateFontName alternate_name);
  // FontCache::PlatformFallbackFontForCharacter with the set.
  static const SimpleFontData* UniformFallbackFontForCharacter(
      const UniformFontSet& set,
      const FontDescription& description,
      UChar32 character,
      FontFallbackPriority fallback_priority);
  // FontCache::GetLastResortFallbackFont with the set: never a system font,
  // an empty one until the set's files have arrived.
  static const SimpleFontData* UniformLastResortFontData(
      const UniformFontSet& set,
      const FontDescription& description);
  // How every font is rasterised and measured with the set, on every OS:
  // anti-aliased (not LCD), unhinted, subpixel positioned, linear metrics,
  // no embedded bitmaps.
  static WebFontRenderStyle UniformRenderStyle();
  static void ApplyUniformRendering(SkFont& font);

 private:
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
  WebFontRenderStyle QuerySystemRenderStyle(const std::string& family,
                                            float text_size,
                                            SkFontStyle,
                                            TextRenderingMode text_rendering);
#endif
#if BUILDFLAG(IS_WIN)
  // TODO(https://crbug.com/808221): Remove and use QuerySystemRenderStyle()
  // instead.
  WebFontRenderStyle QuerySystemForRenderStyle();
#endif

  const sk_sp<SkTypeface> typeface_;
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
  std::string family_;
#endif

 public:
  float text_size_ = 0;
  bool synthetic_bold_ = false;
  bool synthetic_italic_ = false;
  bool avoid_embedded_bitmaps_ = false;
  TextRenderingMode text_rendering_ = TextRenderingMode::kAutoTextRendering;
  FontOrientation orientation_ = FontOrientation::kHorizontal;
  ResolvedFontFeatures resolved_font_features_;

 private:
#if !BUILDFLAG(IS_MAC)
  WebFontRenderStyle style_;
#endif

  mutable Member<HarfBuzzFace> harfbuzz_face_;
  bool is_hash_table_deleted_value_ = false;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_FONTS_FONT_PLATFORM_DATA_H_
