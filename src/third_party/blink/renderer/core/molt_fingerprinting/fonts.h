// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: which installed fonts web content
// may see. The pure logic (only //base, no Blink), tested standalone in
// fonts_unittest.cc; system_fonts.h applies it to Blink's font lookups.
//
//  * kUniform (MoltNet): only the per-platform allow-list
//    (font_allowlists.cc: what the OS installs by default) is visible, so
//    every MoltNet user on an OS sees the same fonts.
//  * kFarble (regular and Incognito windows): the allow-list plus a keyed,
//    per-site sample of every other family name: a name is in the sample iff
//    PRF(site key, name) hits 1 in kExtraFontOneIn. Same site and session =>
//    same sample; another site or session => an unrelated one.
//  * Shields off, non-http(s) top-level sites, generic families
//    (serif, sans-serif, monospace, cursive, fantasy, system-ui, math) and web
//    fonts (src: url()) are not affected.
//
// A lookup is allowed only if both the requested name and the family the
// platform resolved it to are visible: the resolved family stops aliases,
// localized, full and PostScript names from reaching a hidden font; the
// requested name keeps platform-dependent aliasing (fontconfig's metric
// aliases, Blink's Arial/Helvetica alternates) from telling machines apart.
// src: local() names (full or PostScript names of one face) are never looked
// up as such in a restricting context; they are read as a visible family
// plus style words (FontVisibility::ForLocalFaceRequest).
//
// Names are compared in a normalized form: ASCII letters lower-cased and
// ASCII whitespace removed (fontconfig ignores blanks as well); every other
// code point is compared as is. Matching is linear in the name's length and
// allocation-free.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FONTS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FONTS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "base/containers/span.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.h"

namespace blink::molt_fingerprinting {

// The platform this binary was built for (ChromeOS and Fuchsia use the
// Linux list).
FontPlatform CurrentFontPlatform();

// One installed family name in kExtraFontOneIn is in a site's sample.
inline constexpr uint64_t kExtraFontOneIn = 16;

// src: local() names in a restricting context: a longer name (in normalized
// code points) names no face, and at most this many families are looked up
// for one name.
inline constexpr size_t kMaxLocalFaceNameLength = 128;
inline constexpr size_t kMaxLocalFaceCandidates = 4;

// A font name (family, full or PostScript name) in the encoding Blink, Skia
// or the font enumeration hands it over. Doesn't own the characters.
// Malformed UTF-8 or UTF-16 decodes to U+FFFD; nothing here fails.
class FontName {
 public:
  static FontName Utf8(std::string_view name);
  static FontName Latin1(base::span<const uint8_t> name);
  static FontName Utf16(base::span<const char16_t> name);

  // 64-bit FNV-1a over the normalized code points. Not keyed, not secret.
  uint64_t Hash() const;
  // Whether the normalized forms are equal.
  bool Equals(const FontName& other) const;
  // Whether the normalized form is empty.
  bool IsEmpty() const;
  // The name's first `length` bytes (UTF-8, Latin-1) or code units (UTF-16),
  // clamped to its size.
  FontName Prefix(size_t length) const;

 private:
  friend class NormalizedFontNameReader;
  enum class Encoding : uint8_t { kUtf8, kLatin1, kUtf16 };

  FontName(Encoding encoding,
           base::span<const uint8_t> bytes,
           base::span<const char16_t> units);

  Encoding encoding_;
  base::span<const uint8_t> bytes_;   // kUtf8, kLatin1.
  base::span<const char16_t> units_;  // kUtf16.
};

// The list entry `name` matches on `platform` (family or alias), or null.
// `hash` must be name.Hash().
const FontListEntry* FindInFontList(FontPlatform platform,
                                    const FontName& name,
                                    uint64_t hash);
// Whether `family` is a listed family on `platform` (aliases don't count).
bool IsAllowListedFamily(FontPlatform platform,
                         const FontName& family,
                         uint64_t hash);

// What a context may see.
class FontVisibility {
 public:
  enum class Access : uint8_t {
    kAll,                 // Unprotected: every installed font.
    kAllowList,           // The allow-list only.
    kAllowListAndSample,  // The allow-list and the site's sample.
  };

  static FontVisibility All();
  static FontVisibility AllowList(FontPlatform platform);
  static FontVisibility AllowListAndSample(FontPlatform platform,
                                           const Farbler& farbler);

  Access access() const { return access_; }
  bool restricts() const { return access_ != Access::kAll; }

  // What to do with a request for the non-generic family name `requested`.
  struct Request {
    enum class Kind : uint8_t {
      // Not visible: don't look it up at all.
      kBlocked,
      // Look `requested` up; use the result only if IsFamilyVisible() of
      // the family it resolved to.
      kLookUp,
      // Look up the listed family `instead` (a family `requested` is an
      // alias of); use the result only if its family is visible.
      kLookUpInstead,
      // Look it up and use it unchecked: protection is off, or `requested`
      // is "system-ui", which Blink also uses as the family name of the
      // platform UI font (quoted "system-ui", macOS's BlinkMacSystemFont).
      kUnrestricted,
    };
    Kind kind = Kind::kBlocked;
    std::string_view instead;
  };
  Request ForFamilyRequest(const FontName& requested) const;

  // src: local(name) names one face by its full or PostScript name. A
  // restricting context never looks such a name up: that lookup would cost
  // more for an installed hidden face than for a missing one, and fill
  // caches other documents can time. Instead the name is read as a family
  // this context may look up anyway (a listed family, or a name in the
  // site's sample) followed by style words, and the face is looked up the
  // way CSS would ask for it: `font-family: <family>; font-weight: <w>;
  // font-style: <s>`. So local() shows nothing a page couldn't learn from
  // font-family, and a hidden face is never looked up, installed or not.
  //
  // Style words (any order, each kind at most once, ASCII case and
  // whitespace ignored, '-' and '_' between them): one weight (thin,
  // hairline, extralight, ultralight, light, semilight, demilight, book,
  // regular, normal, roman, plain, medium, semibold, demibold, demi, bold,
  // extrabold, ultrabold, heavy, black, extrablack, ultrablack), one slope
  // (italic, it, oblique), and the PostScript suffixes "MT" and "PS"
  // (ArialMT, TimesNewRomanPS-BoldMT). Width words are not style words: a
  // condensed or narrow face is only found through a listed family that
  // names it (DejaVu Sans Condensed).
  struct LocalFace {
    // The family to look up: `listed` (a list entry's own spelling) when
    // non-empty, else the requested name's first `prefix_length` bytes or
    // code units (a name in the site's sample).
    std::string_view listed;
    size_t prefix_length = 0;
    // FontName(family).Hash(). A face found for a sampled family is used
    // only if its own family has this hash (no aliases, substitutes or
    // generic families).
    uint64_t family_hash = 0;
    // Whether the name had style words after the family. If not, the face
    // the family lookup finds is the one; otherwise ask for `weight` and
    // `italic`, and use the face only if StyleMatches() its own style.
    bool styled = false;
    uint16_t weight = 400;
    uint16_t min_weight = 400;
    uint16_t max_weight = 400;
    bool italic = false;

    bool sampled() const { return listed.empty(); }
    // Whether a face of weight `weight` that is italic or oblique iff
    // `italic` is the one the name asks for.
    bool StyleMatches(int face_weight, bool face_italic) const;
  };
  struct LocalFaceRequest {
    std::array<LocalFace, kMaxLocalFaceCandidates> faces;
    size_t size = 0;
    base::span<const LocalFace> candidates() const {
      return base::span(faces).first(size);
    }
  };
  // The family lookups for local(`unique_name`), longest family first: the
  // first face one of them finds (and the rules above accept) is the one.
  // None when the name doesn't read as a visible family and style words, or
  // when this context sees every font (then the caller looks the unique
  // name up as Chromium does).
  LocalFaceRequest ForLocalFaceRequest(const FontName& unique_name) const;

  // Whether an installed font of family `family` may be used or listed.
  // Empty names are never visible in a restricting context.
  bool IsFamilyVisible(const FontName& family) const;
  // The same, with family.Hash() and IsAllowListedFamily() precomputed.
  bool IsFamilyVisible(uint64_t family_hash, bool allow_listed) const;

  // Whether the name with `hash` is in this context's sample (false unless
  // access() is kAllowListAndSample).
  bool IsSampled(uint64_t hash) const;

  FontPlatform platform() const { return platform_; }

 private:
  FontVisibility(Access access,
                 FontPlatform platform,
                 std::optional<Farbler> farbler);

  Access access_;
  FontPlatform platform_;
  std::optional<Farbler> farbler_;
};

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FONTS_H_
