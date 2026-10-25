// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: MoltNet's bundled font set, the
// rules. Pure (//base spans only; tested standalone like fonts.h); the Blink
// side, which loads the files and makes typefaces, is moltnet_fonts.h, the
// reasoning docs/FINGERPRINTING.md "Fonts".
//
// MoltNet renderers draw text with the fonts MoltBrowser ships, on every OS:
// the same families, files and versions everywhere, so a page can no longer
// tell machines apart by their installed fonts, their versions or where
// per-character fallback lands. This file answers, from the generated table
// (moltnet_font_set_table.cc, made by tools/fonts/generate_tables.py from
// tools/fonts/moltnet_fonts.json):
//
//  * which family a family name asks for: a family of the set (by any of
//    its names), an alias (Arial -> Arimo, the same on every OS), or a
//    generic keyword (serif, sans-serif, monospace, cursive, fantasy, math,
//    system-ui; Blink's -webkit-standard, -webkit-body, BlinkMacSystemFont);
//  * what the generic families map to for text in a script (CJK text gets
//    the Noto Sans CJK face of its region);
//  * which face of a family serves a weight and slope (CSS Fonts 4 font
//    matching), and at which weight a variable face is instantiated;
//  * the order in which families are tried for a character no font of the
//    page's list has (per script, CJK by region, emoji, then every other
//    family of the set), so fallback never leaves the set;
//  * which face a unique (PostScript or full) name names.
//
// Indices are into the table's arrays; everything is linear in the input
// and allocation-free.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_FONT_SET_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_FONT_SET_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "base/containers/span.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.h"

namespace blink::molt_fingerprinting {

class FontName;

namespace moltnet {

// --- The table (generated) ----------------------------------------------------

// A file of the set, installed as molt_fonts/<install_name> (the browser
// sends the files to MoltNet renderers in this order).
struct BundledFontFile {
  std::string_view install_name;
  uint64_t size = 0;
};

// A face: one font of a file (a collection holds several).
struct BundledFontFace {
  uint8_t file = 0;
  uint8_t collection_index = 0;
  uint8_t family = 0;
  bool italic = false;
  // Whether the face has a 'wght' axis (then every weight in [min_weight,
  // max_weight] is instantiated as such; the file's default instance may be
  // another weight, e.g. Thin for Noto Sans CJK).
  bool variable_weight = false;
  uint16_t min_weight = 400;
  uint16_t default_weight = 400;
  uint16_t max_weight = 400;
  std::string_view postscript_name;
  std::string_view full_name;
  std::string_view style;
};

// A family: the name Skia's typefaces report (getFamilyName()), its hash
// (FontName::Hash()) and its faces, faces[first_face, first_face + count).
struct BundledFontFamily {
  std::string_view name;
  uint64_t hash = 0;
  uint8_t first_face = 0;
  uint8_t face_count = 0;
};

// Another name that asks for a family: another name of the family in its
// fonts, or an alias (`alias` true: Arial, Times New Roman, Courier New, ...).
struct BundledFontName {
  std::string_view name;
  uint64_t hash = 0;
  uint8_t family = 0;
  bool alias = false;
};

enum class Generic : uint8_t {
  kStandard,  // Text without font-family, -webkit-standard, -webkit-body.
  kSerif,
  kSansSerif,
  kMonospace,
  kCursive,
  kFantasy,
  kMath,
  kSystemUi,
};
inline constexpr size_t kGenericCount = 8;

enum class HanRegion : uint8_t { kJP, kKR, kSC, kTC, kHK };
inline constexpr size_t kHanRegionCount = 5;

// How text in a CJK script picks its region: fixed, or by the content
// locale (Han ideographs).
struct CjkScript {
  uint16_t script = 0;  // An ICU UScriptCode.
  uint8_t region = 0;   // A HanRegion, unless by_locale.
  bool by_locale = false;
};

// Fallback lists, keyed by ICU UScriptCode, or by these two keys.
inline constexpr uint16_t kFallbackEmoji = 0xffff;      // Emoji presentation.
inline constexpr uint16_t kFallbackTextEmoji = 0xfffe;  // Text presentation.
// In a fallback list: the CJK families, the character's region first.
inline constexpr uint8_t kFallbackCjk = 0xff;
// A family index meaning "none" (cjk_monospace).
inline constexpr uint8_t kNoFamily = 0xfe;

struct ScriptFallback {
  uint16_t key = 0;
  uint16_t first = 0;  // Into fallback_families.
  uint8_t count = 0;
};

struct BundledFontTable {
  std::string_view version;
  base::span<const BundledFontFile> files;
  base::span<const BundledFontFace> faces;
  base::span<const BundledFontFamily> families;
  base::span<const BundledFontName> names;
  std::array<uint8_t, kGenericCount> generic{};
  std::array<uint8_t, kHanRegionCount> cjk_regions{};
  uint8_t cjk_default_region = 0;
  uint8_t cjk_monospace = kNoFamily;
  base::span<const CjkScript> cjk_scripts;
  base::span<const ScriptFallback> fallback;
  base::span<const uint8_t> fallback_families;
  // FontListFor(FontPlatform::kMoltNet): the families (every name) and the
  // aliases, for the visibility rules of fonts.h.
  base::span<const FontListEntry> visibility_list;
};

// The generated table (moltnet_font_set_table.cc).
const BundledFontTable& GetBundledFontTable();

// --- The rules -------------------------------------------------------------------

// Upper bound of FallbackFamilies()'s result: every family once.
inline constexpr size_t kMaxFallbackFamilies = 255;

class BundledFontSet {
 public:
  explicit BundledFontSet(const BundledFontTable& table);

  const BundledFontTable& table() const { return table_; }

  // The family a non-generic family name asks for (a family's own or other
  // name, or an alias), or the generic family a generic keyword stands for
  // (as for Latin text), or nullopt: a name the set doesn't have.
  std::optional<uint8_t> FamilyForName(const FontName& name) const;
  // Whether `name` is a generic keyword this set maps (serif, sans-serif,
  // monospace, cursive, fantasy, math, system-ui, -webkit-standard,
  // -webkit-body, BlinkMacSystemFont); stores which in `generic`.
  static bool IsGenericKeyword(const FontName& name, Generic* generic);

  // The family `generic` maps to for text in `script` (an ICU UScriptCode,
  // the script of the content's locale) in `locale` (BCP 47, for the region
  // of Han text).
  uint8_t FamilyForGeneric(Generic generic,
                           int script,
                           std::string_view locale) const;

  // The face of `family` that serves `weight` (1..1000) and slope `italic`
  // (italic or oblique asked for), by CSS Fonts 4 font matching: the slope
  // first (the other slope only if the family has none of this one), then
  // the weight. `weight` is what to instantiate a variable face at: the
  // request, clamped to the face's range; a static face's own weight.
  struct FaceMatch {
    uint8_t face = 0;
    uint16_t weight = 400;
  };
  FaceMatch MatchFace(uint8_t family, int weight, bool italic) const;

  // The families to try, in order and each once, for a character of
  // `script` (ICU UScriptCode; USCRIPT_COMMON for punctuation, symbols and
  // digits) that the page's fonts lack: its script's list (or the emoji
  // list), with the CJK families expanded region first, then every other
  // family in table order. Returns how many were stored in `out`.
  enum class Presentation : uint8_t { kText, kEmoji, kTextEmoji };
  size_t FallbackFamilies(int script,
                          HanRegion region,
                          Presentation presentation,
                          base::span<uint8_t, kMaxFallbackFamilies> out) const;

  // The region of Han text in `locale` (BCP 47: ja -> JP, ko -> KR,
  // zh-Hant / zh-TW / zh-MO -> TC, zh-HK -> HK, other zh -> SC), or the
  // table's default for a locale that names none.
  HanRegion RegionForLocale(std::string_view locale) const;
  // The region of characters of `script`, where the script fixes it
  // (kana -> JP, hangul -> KR, bopomofo -> TC), else `han_region`.
  HanRegion RegionForScript(int script, HanRegion han_region) const;

  // The face whose PostScript or full name is `name`, or nullopt.
  std::optional<uint8_t> FaceForUniqueName(const FontName& name) const;

 private:
  const BundledFontTable& table_;
};

}  // namespace moltnet
}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_FONT_SET_H_
