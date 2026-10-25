/*
 * Copyright (C) 2011 Brent Fulgham
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "third_party/blink/renderer/platform/fonts/font_platform_data.h"

#include <atomic>   // MoltBrowser
#include <string>   // MoltBrowser
#include <utility>  // MoltBrowser

#include "base/bit_cast.h"
#include "base/compiler_specific.h"
#include "base/feature_list.h"
#include "build/build_config.h"
#include "hb-ot.h"
#include "hb.h"
#include "skia/ext/skia_utils_base.h"
#include "third_party/blink/public/platform/linux/web_sandbox_support.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/fonts/shaping/harfbuzz_face.h"
#include "third_party/blink/renderer/platform/fonts/shaping/harfbuzz_font_cache.h"
#include "third_party/blink/renderer/platform/fonts/shaping/harfbuzz_font_data.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/text/character.h"
#include "third_party/blink/renderer/platform/web_test_support.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/text/character_names.h"
#include "third_party/blink/renderer/platform/wtf/text/string_hash.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkString.h"  // MoltBrowser
#include "third_party/skia/include/core/SkTypeface.h"

#if BUILDFLAG(IS_MAC)
#include "third_party/skia/include/ports/SkTypeface_mac.h"
#endif

namespace blink {

namespace {
// MoltBrowser: SetUniformTextRendering().
std::atomic<bool> g_uniform_geometric_precision{false};
std::atomic<const char*> g_uniform_emoji_family{nullptr};
}  // namespace

// static
void FontPlatformData::SetUniformTextRendering(  // MoltBrowser
    const UniformTextRendering& rendering) {
  g_uniform_emoji_family.store(rendering.emoji_family,
                               std::memory_order_release);
  g_uniform_geometric_precision.store(rendering.geometric_precision,
                                      std::memory_order_release);
}

// static
FontPlatformData::UniformTextRendering
FontPlatformData::GetUniformTextRendering() {  // MoltBrowser
  UniformTextRendering rendering;
  rendering.geometric_precision =
      g_uniform_geometric_precision.load(std::memory_order_acquire);
  rendering.emoji_family =
      g_uniform_emoji_family.load(std::memory_order_acquire);
  return rendering;
}

// static
const SimpleFontData* FontPlatformData::UniformEmojiFontData(  // MoltBrowser
    const FontDescription& description) {
  const char* family = g_uniform_emoji_family.load(std::memory_order_acquire);
  return family ? FontCache::Get().GetFontData(description,
                                               AtomicString(family))
                : nullptr;
}

// MoltBrowser: MoltNet's bundled fonts (molt_fingerprinting/moltnet_fonts.h).
namespace {
std::atomic<const FontPlatformData::UniformFontSet*> g_uniform_font_set{
    nullptr};
}  // namespace

// static
void FontPlatformData::SetUniformFontSet(const UniformFontSet* set) {
  g_uniform_font_set.store(set, std::memory_order_release);
}

// static
const FontPlatformData::UniformFontSet* FontPlatformData::GetUniformFontSet() {
  return g_uniform_font_set.load(std::memory_order_acquire);
}

// static
const FontPlatformData* FontPlatformData::CreateUniformFontPlatformData(
    const UniformFontSet& set,
    const FontDescription& description,
    const FontFaceCreationParams& creation_params,
    float font_size,
    AlternateFontName alternate_name) {
  // Only names: never a fontconfig id or a file of the system.
  if (creation_params.CreationType() != kCreateFontByFamily) {
    return nullptr;
  }
  sk_sp<SkTypeface> typeface =
      alternate_name == AlternateFontName::kLocalUniqueFace
          ? set.MatchUniqueName(creation_params.Family())
          : set.MatchFamily(creation_params.Family(), description);
  if (!typeface) {
    return nullptr;
  }
  // Synthetic bold and italic as font_cache_skia.cc decides them, on every
  // OS.
  const bool synthetic_bold =
      (description.Weight() >
           FontSelectionValue(200) +
               FontSelectionValue(typeface->fontStyle().weight()) ||
       description.IsSyntheticBold()) &&
      description.GetFontSynthesisWeight() ==
          FontDescription::kAutoFontSynthesisWeight;
  const bool synthetic_italic =
      ((description.Style() == kItalicSlopeValue && !typeface->isItalic()) ||
       description.IsSyntheticItalic()) &&
      description.GetFontSynthesisStyle() ==
          FontDescription::kAutoFontSynthesisStyle;
  SkString family;
  typeface->getFamilyName(&family);
  FontPlatformData* platform_data = MakeGarbageCollected<FontPlatformData>(
      std::move(typeface), std::string(family.c_str(), family.size()),
      font_size, synthetic_bold, synthetic_italic, description.TextRendering(),
      description.ResolveFontFeatures(), description.Orientation());
  platform_data->SetAvoidEmbeddedBitmaps(true);
  return platform_data;
}

// static
const SimpleFontData* FontPlatformData::UniformFallbackFontForCharacter(
    const UniformFontSet& set,
    const FontDescription& description,
    UChar32 character,
    FontFallbackPriority fallback_priority) {
  const AtomicString family =
      set.FallbackFamily(description, character, fallback_priority);
  if (family.IsNull()) {
    return nullptr;
  }
  return FontCache::Get().GetFontData(description, family,
                                      AlternateFontName::kNoAlternate);
}

// static
const SimpleFontData* FontPlatformData::UniformLastResortFontData(
    const UniformFontSet& set,
    const FontDescription& description) {
  FontCache& font_cache = FontCache::Get();
  if (const SimpleFontData* font_data = font_cache.GetFontData(
          description, set.LastResortFamily(description),
          AlternateFontName::kNoAlternate)) {
    return font_data;
  }
  // The set's files haven't arrived (or were refused): a font without
  // glyphs rather than one of the system's.
  return font_cache.FontDataFromFontPlatformData(
      MakeGarbageCollected<FontPlatformData>(
          SkTypeface::MakeEmpty(), std::string(),
          description.EffectiveFontSize(), false, false,
          description.TextRendering(), ResolvedFontFeatures(),
          description.Orientation()));
}

// static
WebFontRenderStyle FontPlatformData::UniformRenderStyle() {
  WebFontRenderStyle style;
  style.use_bitmaps = 0;
  style.use_auto_hint = 0;
  style.use_hinting = 0;
  style.hint_style = 0;  // SkFontHinting::kNone.
  style.use_anti_alias = 1;
  style.use_subpixel_rendering = 0;
  style.use_subpixel_positioning = 1;
  return style;
}

// static
void FontPlatformData::ApplyUniformRendering(SkFont& font) {
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setHinting(SkFontHinting::kNone);
  font.setForceAutoHinting(false);
  font.setSubpixel(true);
  font.setLinearMetrics(true);
  font.setEmbeddedBitmaps(false);
}

FontPlatformData::FontPlatformData(HashTableDeletedValueType)
    : is_hash_table_deleted_value_(true) {}

FontPlatformData::FontPlatformData() = default;

FontPlatformData::FontPlatformData(const FontPlatformData& source)
    : typeface_(source.typeface_),
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
      family_(source.family_),
#endif
      text_size_(source.text_size_),
      synthetic_bold_(source.synthetic_bold_),
      synthetic_italic_(source.synthetic_italic_),
      avoid_embedded_bitmaps_(source.avoid_embedded_bitmaps_),
      text_rendering_(source.text_rendering_),
      orientation_(source.orientation_),
      resolved_font_features_(source.resolved_font_features_)
#if !BUILDFLAG(IS_MAC)
      ,
      style_(source.style_)
#endif
{
}

FontPlatformData::FontPlatformData(const FontPlatformData& src, float text_size)
    : FontPlatformData(src.typeface_,
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
                       src.family_.data(),
#else
                       std::string(),
#endif
                       text_size,
                       src.synthetic_bold_,
                       src.synthetic_italic_,
                       src.text_rendering_,
                       src.resolved_font_features_,
                       src.orientation_) {
}

FontPlatformData::FontPlatformData(sk_sp<SkTypeface> typeface,
                                   const std::string& family,
                                   float text_size,
                                   bool synthetic_bold,
                                   bool synthetic_italic,
                                   TextRenderingMode text_rendering,
                                   ResolvedFontFeatures resolved_font_features,
                                   FontOrientation orientation)
    : typeface_(typeface),
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
      family_(family),
#endif  // !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
      text_size_(text_size),
      synthetic_bold_(synthetic_bold),
      synthetic_italic_(synthetic_italic),
      text_rendering_(text_rendering),
      orientation_(orientation),
      resolved_font_features_(std::move(resolved_font_features)) {
#if !BUILDFLAG(IS_MAC)
  // MoltBrowser: with MoltNet's bundled fonts, one fixed style on every OS,
  // never the desktop's.
  if (GetUniformFontSet()) {
    style_ = UniformRenderStyle();
    return;
  }
  style_ = WebFontRenderStyle::GetDefault();
#if !BUILDFLAG(IS_WIN)
  WebFontRenderStyle system_style;
  system_style = QuerySystemRenderStyle(family, text_size,
                                        typeface_->fontStyle(), text_rendering);

  // In web tests, ignore system preference for subpixel positioning,
  // or explicitly disable if requested.
  if (WebTestSupport::IsRunningWebTest()) {
    system_style.use_subpixel_positioning =
        WebTestSupport::IsTextSubpixelPositioningAllowedForTest()
            ? WebFontRenderStyle::kNoPreference
            : 0;
  }
#else
  auto system_style = QuerySystemForRenderStyle();
#endif  // !BUILDFLAG(IS_WIN)
  style_.OverrideWith(system_style);
#endif  // !BUILDFLAG(IS_MAC)
}

FontPlatformData::~FontPlatformData() = default;

void FontPlatformData::Trace(Visitor* visitor) const {
  visitor->Trace(harfbuzz_face_);
}

#if BUILDFLAG(IS_MAC)
CTFontRef FontPlatformData::CtFont() const {
  return SkTypeface_GetCTFontRef(typeface_.get());
}
#endif

bool FontPlatformData::operator==(const FontPlatformData& a) const {
  // If either of the typeface pointers are null then we test for pointer
  // equality. Otherwise, we call SkTypeface::Equal on the valid pointers.
  bool typefaces_equal = false;
  if (!Typeface() || !a.Typeface())
    typefaces_equal = Typeface() == a.Typeface();
  else
    typefaces_equal = SkTypeface::Equal(Typeface(), a.Typeface());

  return typefaces_equal && text_size_ == a.text_size_ &&
         is_hash_table_deleted_value_ == a.is_hash_table_deleted_value_ &&
         synthetic_bold_ == a.synthetic_bold_ &&
         synthetic_italic_ == a.synthetic_italic_ &&
         avoid_embedded_bitmaps_ == a.avoid_embedded_bitmaps_ &&
         text_rendering_ == a.text_rendering_ &&
         resolved_font_features_ == a.resolved_font_features_ &&
#if !BUILDFLAG(IS_MAC)
         style_ == a.style_ &&
#endif
         orientation_ == a.orientation_;
}

SkTypefaceID FontPlatformData::UniqueID() const {
  return Typeface()->uniqueID();
}

String FontPlatformData::FontFamilyName() const {
  DCHECK(Typeface());
  SkTypeface::LocalizedStrings* font_family_iterator =
      Typeface()->createFamilyNameIterator();
  SkTypeface::LocalizedString localized_string;
  while (font_family_iterator->next(&localized_string) &&
         !localized_string.fString.size()) {
  }
  font_family_iterator->unref();
  return String::FromUtf8(base::as_byte_span(localized_string.fString));
}

SkTypeface* FontPlatformData::Typeface() const {
  return typeface_.get();
}

HarfBuzzFace* FontPlatformData::GetHarfBuzzFace() const {
  if (!harfbuzz_face_) {
    harfbuzz_face_ = MakeGarbageCollected<HarfBuzzFace>(this, UniqueID());
  }

  return harfbuzz_face_.Get();
}

bool FontPlatformData::HasSpaceInLigaturesOrKerning(
    TypesettingFeatures features) const {
  HarfBuzzFace* hb_face = GetHarfBuzzFace();
  if (!hb_face)
    return false;

  return hb_face->HasSpaceInLigaturesOrKerning(features);
}

uint32_t FontPlatformData::GetHash() const {
  uint32_t h = UniqueID();
  h ^= 0x01010101 * ((static_cast<int>(is_hash_table_deleted_value_) << 3) |
                     (static_cast<int>(orientation_) << 2) |
                     (static_cast<int>(synthetic_bold_) << 1) |
                     static_cast<int>(synthetic_italic_));

  h ^= base::bit_cast<uint32_t>(text_size_);

  return h;
}

#if !BUILDFLAG(IS_MAC)
bool FontPlatformData::FontContainsCharacter(UChar32 character) const {
  return CreateSkFont().unicharToGlyph(character);
}
#endif

#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN)
// static
WebFontRenderStyle FontPlatformData::QuerySystemRenderStyle(
    const std::string& family,
    float text_size,
    SkFontStyle font_style,
    TextRenderingMode text_rendering) {
  WebFontRenderStyle result;

#if !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_FUCHSIA) && !BUILDFLAG(IS_IOS)
  // If the font name is missing (i.e. probably a web font) or the sandbox is
  // disabled, use the system defaults.
  if (family.length() && Platform::Current()->GetSandboxSupport()) {
    bool is_bold = font_style.weight() >= SkFontStyle::kSemiBold_Weight;
    bool is_italic = font_style.slant() != SkFontStyle::kUpright_Slant;
    Platform::Current()->GetSandboxSupport()->GetWebFontRenderStyleForStrike(
        family.data(), text_size, is_bold, is_italic,
        FontCache::DeviceScaleFactor(), &result);
  }

  // If `text-rendering: geometric-precision`, use subpixel positioning.
  // https://svgwg.org/svg2-draft/painting.html#TextRenderingProperty
  //
  // TODO(crbug.com/1327530): For now, do this only for Linux/Cros to minimize
  // the impact, and because they're the only platform that adjust these
  // settings by different device scale factors. See the doc link in the crbug.
  // MoltBrowser: MoltNet renders all text so, anti-aliased whatever the
  // desktop's settings (molt_fingerprinting).
  const bool uniform = GetUniformTextRendering().geometric_precision;
  if (uniform) {
    result.use_anti_alias = 1;
  }
  if ((text_rendering == TextRenderingMode::kGeometricPrecision || uniform) &&
      result.use_anti_alias) {
    result.use_subpixel_positioning = true;
    result.use_hinting = false;
    // 0 means HINTING_NONE, see |ConvertHinting| in font_service_app.cc.
    result.hint_style = 0;
  }
#endif  // !BUILDFLAG(IS_ANDROID) && !BUILDFLAG(IS_FUCHSIA) &&
        // !BUILDFLAG(IS_IOS)

  return result;
}
#endif  // !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN)

#if !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_IOS)
SkFont FontPlatformData::CreateSkFont(const FontDescription*) const {
  SkFont font(typeface_);
  style_.ApplyToSkFont(&font);

  const float ts = text_size_ >= 0 ? text_size_ : 12;
  font.setSize(SkFloatToScalar(ts));
  font.setEmbolden(synthetic_bold_);
  font.setSkewX(synthetic_italic_ ? -SK_Scalar1 / 4 : 0);

  font.setEmbeddedBitmaps(!avoid_embedded_bitmaps_);

  if (RuntimeEnabledFeatures::NoFontAntialiasingEnabled() &&
      !WebTestSupport::IsFontAntialiasingEnabledForTest()) {
    font.setEdging(SkFont::Edging::kAlias);
  }

  // MoltBrowser: MoltNet's bundled fonts render alike on every OS.
  if (GetUniformFontSet()) {
    ApplyUniformRendering(font);
  }
  return font;
}
#endif  // !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_IOS)

String FontPlatformData::GetPostScriptName() const {
  if (!typeface_)
    return String();

  SkString postscript_name;
  bool success = typeface_->getPostScriptName(&postscript_name);
  return success ? String(base::as_byte_span(postscript_name)) : String();
}

}  // namespace blink
