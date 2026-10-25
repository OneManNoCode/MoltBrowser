// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Stand-ins for what the extracted Blink metric code (extracted.inc) calls:
// Skia's SkFont as Fontations answers it (values from skrifa_dump), the
// Blink types it fills, and build_config's BUILDFLAG for the OS chosen with
// -DMOLT_OS_LINUX, -DMOLT_OS_WIN or -DMOLT_OS_MAC. The code under test is
// the extracted code; these only carry data in and out.
#ifndef MOLT_METRICS_STANDINS_H_
#define MOLT_METRICS_STANDINS_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>

#define BUILDFLAG(flag) (BUILDFLAG_INTERNAL_##flag())
#if defined(MOLT_OS_LINUX)
#define BUILDFLAG_INTERNAL_IS_LINUX() (1)
#else
#define BUILDFLAG_INTERNAL_IS_LINUX() (0)
#endif
#if defined(MOLT_OS_WIN)
#define BUILDFLAG_INTERNAL_IS_WIN() (1)
#else
#define BUILDFLAG_INTERNAL_IS_WIN() (0)
#endif
#if defined(MOLT_OS_MAC)
#define BUILDFLAG_INTERNAL_IS_MAC() (1)
#define BUILDFLAG_INTERNAL_IS_APPLE() (1)
#else
#define BUILDFLAG_INTERNAL_IS_MAC() (0)
#define BUILDFLAG_INTERNAL_IS_APPLE() (0)
#endif
#define BUILDFLAG_INTERNAL_IS_CHROMEOS() (0)
#define BUILDFLAG_INTERNAL_IS_ANDROID() (0)
#define BUILDFLAG_INTERNAL_IS_FUCHSIA() (0)

#define DCHECK(x) ((void)0)
#define DCHECK_LE(a, b) ((void)0)
#define CHECK(x)    \
  do {              \
    if (!(x)) {     \
      std::abort(); \
    }               \
  } while (0)
#define UNSAFE_BUFFERS(...) __VA_ARGS__
#define WTF_HEAP_PROFILER_TYPE_NAME(x) #x

using SkScalar = float;
using SkGlyphID = uint16_t;
using Glyph = uint16_t;
using UChar32 = int32_t;
using hb_codepoint_t = uint32_t;
using hb_position_t = int32_t;

inline SkScalar SkScalarRoundToScalar(SkScalar x) {
  return std::floor(x + 0.5f);
}
inline int SkScalarRoundToInt(SkScalar x) {
  return static_cast<int>(std::floor(x + 0.5f));
}
constexpr uint32_t SkSetFourByteTag(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
         (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

struct SkIRect {
  int fLeft = 0, fTop = 0, fRight = 0, fBottom = 0;
};

struct SkRect {
  float fLeft = 0, fTop = 0, fRight = 0, fBottom = 0;
  static SkRect MakeLTRB(float l, float t, float r, float b) {
    return {l, t, r, b};
  }
  float width() const { return fRight - fLeft; }
  float height() const { return fBottom - fTop; }
  void roundOut(SkIRect* r) const {
    *r = {int(std::floor(fLeft)), int(std::floor(fTop)), int(std::ceil(fRight)),
          int(std::ceil(fBottom))};
  }
  SkIRect roundOut() const {
    SkIRect r;
    roundOut(&r);
    return r;
  }
  void set(const SkIRect& r) {
    *this = {float(r.fLeft), float(r.fTop), float(r.fRight), float(r.fBottom)};
  }
};

struct SkFontMetrics {
  enum FontMetricsFlags {
    kUnderlineThicknessIsValid_Flag = 1 << 0,
    kUnderlinePositionIsValid_Flag = 1 << 1,
  };
  uint32_t fFlags = 0;
  float fTop = 0, fAscent = 0, fDescent = 0, fBottom = 0, fLeading = 0;
  float fAvgCharWidth = 0, fMaxCharWidth = 0, fXMin = 0, fXMax = 0;
  float fXHeight = 0, fCapHeight = 0;
  float fUnderlineThickness = 0, fUnderlinePosition = 0;
  bool hasUnderlineThickness(float* t) const {
    if (fFlags & kUnderlineThicknessIsValid_Flag) {
      *t = fUnderlineThickness;
      return true;
    }
    return false;
  }
  bool hasUnderlinePosition(float* p) const {
    if (fFlags & kUnderlinePositionIsValid_Flag) {
      *p = fUnderlinePosition;
      return true;
    }
    return false;
  }
};

enum class SkFontHinting { kNone, kSlight, kNormal, kFull };

struct SkTypeface {
  size_t getTableSize(uint32_t) const { return 0; }
  size_t getTableData(uint32_t, size_t, size_t, void*) const { return 0; }
};

struct GlyphInfo {  // One glyph of a case.
  float advance = 0;
  bool has_path = false;
  SkRect path;  // SkPath::getBounds(), y down.
};

struct FakePath {
  SkRect bounds;
  SkRect getBounds() const { return bounds; }
};

// SkFont over a Fontations typeface, unhinted, subpixel positioned
// (FontPlatformData::ApplyUniformRendering on every OS).
class SkFont {
 public:
  SkFontMetrics metrics;
  std::map<uint16_t, GlyphInfo> glyphs;
  bool subpixel = true;
  SkFontHinting hinting = SkFontHinting::kNone;

  SkScalar getMetrics(SkFontMetrics* out) const {
    *out = metrics;
    return metrics.fDescent - metrics.fAscent + metrics.fLeading;
  }
  SkTypeface* getTypeface() const { return const_cast<SkTypeface*>(&face_); }
  bool isForceAutoHinting() const { return false; }
  SkFontHinting getHinting() const { return hinting; }
  bool isSubpixel() const { return subpixel; }
  std::optional<FakePath> getPath(uint16_t glyph) const {
    auto it = glyphs.find(glyph);
    if (it == glyphs.end() || !it->second.has_path) {
      return std::nullopt;
    }
    return FakePath{it->second.path};
  }
  // An outline glyph's SkGlyph bounds: its path's, rounded out.
  SkRect getBounds(uint16_t glyph, std::nullptr_t) const {
    auto it = glyphs.find(glyph);
    if (it == glyphs.end() || !it->second.has_path) {
      return SkRect();
    }
    SkRect r;
    r.set(it->second.path.roundOut());
    return r;
  }
  SkScalar getWidth(uint16_t glyph) const {
    auto it = glyphs.find(glyph);
    return it == glyphs.end() ? 0 : it->second.advance;
  }

 private:
  SkTypeface face_;
};

namespace base {
template <typename T>
struct span {
  T* data;
  size_t size;
};
template <typename T>
span(T*, size_t) -> span<T>;
}  // namespace base

namespace blink {

using String = std::string;

template <typename T, typename U>
T ClampTo(U value) {
  return static_cast<T>(value);
}

struct Partitions {
  static void* FastMalloc(size_t n, const char*) { return std::malloc(n); }
  static void FastFree(void* p) { std::free(p); }
};
inline bool ParseVDMX(int*, int*, base::span<uint8_t>, int) {
  return false;
}

namespace font_family_names {
inline const String kTimes = "Times";
inline const String kHelvetica = "Helvetica";
inline const String kCourier = "Courier";
}  // namespace font_family_names

inline constexpr hb_codepoint_t kUnmatchedVSGlyphId = 0x10FFFF + 1;
struct hb_glyph_extents_t {
  hb_position_t x_bearing, y_bearing, width, height;
};
inline hb_position_t SkiaScalarToHarfBuzzPosition(SkScalar value) {
  constexpr int kHbPosition1 = 1 << 16;
  return ClampTo<int>(value * kHbPosition1);
}

struct WebFontRenderStyle {
  int use_subpixel_positioning = 1;
};

enum class FontOrientation { kHorizontal };
struct HarfBuzzFace;

// Whether MoltNet's bundled fonts are active (the harness's switch).
extern bool g_uniform;

class FontPlatformData {
 public:
  class UniformFontSet {};
  float text_size = 16;
  String family;
  WebFontRenderStyle style;

  float size() const { return text_size; }
  String FontFamilyName() const { return family; }
  const WebFontRenderStyle& GetFontRenderStyle() const { return style; }
  HarfBuzzFace* GetHarfBuzzFace() const { return nullptr; }
  FontOrientation Orientation() const { return FontOrientation::kHorizontal; }
  static const UniformFontSet* GetUniformFontSet() {
    static UniformFontSet set;
    return g_uniform ? &set : nullptr;
  }
};

struct OpenTypeBaselineMetrics {
  OpenTypeBaselineMetrics(HarfBuzzFace*, FontOrientation) {}
  std::optional<float> OpenTypeIdeographicBaseline() { return std::nullopt; }
  std::optional<float> OpenTypeAlphabeticBaseline() { return std::nullopt; }
  std::optional<float> OpenTypeHangingBaseline() { return std::nullopt; }
};

struct FontMetricsOverride {
  std::optional<float> ascent_override;
  std::optional<float> descent_override;
  std::optional<float> line_gap_override;
};

class FontMetrics {
 public:
  static void AscentDescentWithHacks(float& ascent,
                                     float& descent,
                                     const FontPlatformData&,
                                     const SkFont&,
                                     bool subpixel_ascent_descent = false,
                                     std::optional<float> ascent_override = {},
                                     std::optional<float> descent_override = {});
  void Reset() { *this = FontMetrics(); }
  void SetAscent(float v) { ascent = v; }
  void SetDescent(float v) { descent = v; }
  void SetCapHeight(float v) { cap_height = v; }
  void SetUnderlinePosition(float v) { underline_position = v; }
  void SetUnderlineThickness(float v) { underline_thickness = v; }
  void SetXHeight(float v) { x_height = v; }
  void SetHasXHeight(bool v) { has_x_height = v; }
  void SetLineGap(float v) { line_gap = v; }
  void SetLineSpacing(float v) { line_spacing = v; }
  void SetIdeographicBaseline(std::optional<float>) {}
  void SetAlphabeticBaseline(std::optional<float>) {}
  void SetHangingBaseline(std::optional<float>) {}
  float FloatAscent() const { return ascent; }

  float ascent = 0, descent = 0, cap_height = 0, underline_position = 0,
        underline_thickness = 0, x_height = 0, line_gap = 0,
        line_spacing = 0;
  bool has_x_height = true;
};

namespace gfx {
struct RectF {
  float x_ = 0, y_ = 0, w_ = 0, h_ = 0;
  float x() const { return x_; }
  float y() const { return y_; }
  float width() const { return w_; }
  float height() const { return h_; }
};
inline RectF SkRectToRectF(const SkRect& r) {
  return {r.fLeft, r.fTop, r.width(), r.height()};
}
}  // namespace gfx

void SkFontGetBoundsForGlyph(const SkFont& font, Glyph glyph, SkRect* bounds);
float SkFontGetWidthForGlyph(const SkFont& font, Glyph glyph);
void SkFontGetGlyphExtentsForHarfBuzz(const SkFont& font,
                                      hb_codepoint_t codepoint,
                                      hb_glyph_extents_t* extents);

class SimpleFontData {
 public:
  SimpleFontData(const FontPlatformData* platform_data,
                 const SkFont& font,
                 std::map<UChar32, Glyph> cmap)
      : platform_data_(platform_data), font_(font), cmap_(std::move(cmap)) {}

  void PlatformInit(bool subpixel_ascent_descent,
                    const FontMetricsOverride& metrics_override);
  Glyph GlyphForCharacter(UChar32 c) const {
    auto it = cmap_.find(c);
    return it == cmap_.end() ? 0 : it->second;
  }
  gfx::RectF PlatformBoundsForGlyph(Glyph glyph) const;
  gfx::RectF BoundsForGlyph(Glyph glyph) const {
    return PlatformBoundsForGlyph(glyph);
  }
  float WidthForGlyph(Glyph glyph) const;
  const FontPlatformData& PlatformData() const { return *platform_data_; }

  const FontPlatformData* platform_data_;
  SkFont font_;
  std::map<UChar32, Glyph> cmap_;
  FontMetrics font_metrics_;
  float avg_char_width_ = 0;
  float max_char_width_ = 0;
};

}  // namespace blink

#endif  // MOLT_METRICS_STANDINS_H_
