// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include "build/build_config.h"

namespace blink::molt_fingerprinting {

namespace {

constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

// PRF domain of the per-site font sample ("fonts" in ASCII), distinct from
// the farbling domains in farbling.cc.
constexpr uint64_t kDomainFontSample = 0x666f6e7473ull;

constexpr char32_t kReplacementCharacter = 0xfffd;

// The longest list in font_allowlists.cc must fit (fonts_unittest.cc checks
// that every entry is found).
constexpr size_t kMaxFontListSize = 512;

bool IsAsciiWhitespace(char32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

struct IndexedName {
  uint64_t hash = 0;
  uint16_t entry = 0;
};

// A platform's list, sorted by the hash of the normalized name.
struct FontListIndex {
  std::array<IndexedName, kMaxFontListSize> names;
  size_t size = 0;
};

FontListIndex BuildIndex(FontPlatform platform) {
  FontListIndex index;
  const base::span<const FontListEntry> list = FontListFor(platform);
  index.size = std::min(list.size(), index.names.size());
  for (size_t i = 0; i < index.size; ++i) {
    index.names[i].hash = FontName::Utf8(list[i].name).Hash();
    index.names[i].entry = static_cast<uint16_t>(i);
  }
  std::ranges::sort(base::span<IndexedName>(index.names).first(index.size),
                    [](const IndexedName& a, const IndexedName& b) {
                      return a.hash < b.hash ||
                             (a.hash == b.hash && a.entry < b.entry);
                    });
  return index;
}

// Built once per platform, on first use (thread-safe static init); only the
// current platform's index is ever built outside tests.
const FontListIndex& IndexFor(FontPlatform platform) {
  switch (platform) {
    case FontPlatform::kWindows: {
      static const FontListIndex index = BuildIndex(FontPlatform::kWindows);
      return index;
    }
    case FontPlatform::kMac: {
      static const FontListIndex index = BuildIndex(FontPlatform::kMac);
      return index;
    }
    case FontPlatform::kAndroid: {
      static const FontListIndex index = BuildIndex(FontPlatform::kAndroid);
      return index;
    }
    case FontPlatform::kMoltNetBundled: {
      static const FontListIndex index =
          BuildIndex(FontPlatform::kMoltNetBundled);
      return index;
    }
    case FontPlatform::kLinux:
      break;
  }
  static const FontListIndex index = BuildIndex(FontPlatform::kLinux);
  return index;
}

// Style words in local() names (FontVisibility::ForLocalFaceRequest), in
// normalized form, longest first so the first match is the longest one.
enum class StyleWordKind : uint8_t { kWeight, kSlope, kSuffixMT, kSuffixPS };
struct StyleWord {
  std::string_view text;
  StyleWordKind kind;
  uint16_t weight = 0;  // kWeight: what to ask for ...
  uint16_t min_weight = 0;  // ... and the face weights it names.
  uint16_t max_weight = 0;
};
constexpr StyleWord kStyleWords[] = {
    {"extralight", StyleWordKind::kWeight, 200, 200, 200},
    {"ultralight", StyleWordKind::kWeight, 200, 200, 200},
    {"extrablack", StyleWordKind::kWeight, 950, 950, 1000},
    {"ultrablack", StyleWordKind::kWeight, 950, 950, 1000},
    {"semilight", StyleWordKind::kWeight, 350, 350, 350},
    {"demilight", StyleWordKind::kWeight, 350, 350, 350},
    {"extrabold", StyleWordKind::kWeight, 800, 800, 800},
    {"ultrabold", StyleWordKind::kWeight, 800, 800, 800},
    {"hairline", StyleWordKind::kWeight, 100, 100, 100},
    {"semibold", StyleWordKind::kWeight, 600, 600, 600},
    {"demibold", StyleWordKind::kWeight, 600, 600, 600},
    {"regular", StyleWordKind::kWeight, 400, 400, 400},
    {"oblique", StyleWordKind::kSlope},
    {"normal", StyleWordKind::kWeight, 400, 400, 400},
    {"medium", StyleWordKind::kWeight, 500, 500, 500},
    {"italic", StyleWordKind::kSlope},
    {"light", StyleWordKind::kWeight, 300, 300, 300},
    {"roman", StyleWordKind::kWeight, 400, 400, 400},
    {"plain", StyleWordKind::kWeight, 400, 400, 400},
    {"heavy", StyleWordKind::kWeight, 900, 800, 900},
    {"black", StyleWordKind::kWeight, 900, 900, 900},
    {"thin", StyleWordKind::kWeight, 100, 100, 100},
    {"book", StyleWordKind::kWeight, 400, 350, 400},
    {"demi", StyleWordKind::kWeight, 600, 600, 600},
    {"bold", StyleWordKind::kWeight, 700, 700, 700},
    {"it", StyleWordKind::kSlope},
    {"mt", StyleWordKind::kSuffixMT},
    {"ps", StyleWordKind::kSuffixPS},
};

bool IsStyleWordSeparator(char32_t c) {
  return c == '-' || c == '_';
}

// Reads `rest` (normalized code points after the family) as style words
// into `face`; false if anything else is there.
bool ReadStyleWords(base::span<const char32_t> rest,
                    FontVisibility::LocalFace& face) {
  face.styled = !rest.empty();
  bool weight = false;
  bool slope = false;
  bool suffix_mt = false;
  bool suffix_ps = false;
  size_t i = 0;
  while (i < rest.size()) {
    if (IsStyleWordSeparator(rest[i])) {
      ++i;
      continue;
    }
    const StyleWord* match = nullptr;
    for (const StyleWord& word : kStyleWords) {
      if (rest.size() - i < word.text.size()) {
        continue;
      }
      bool equal = true;
      for (size_t k = 0; k < word.text.size() && equal; ++k) {
        equal = rest[i + k] == static_cast<char32_t>(word.text[k]);
      }
      if (equal) {
        match = &word;
        break;
      }
    }
    if (!match) {
      return false;
    }
    bool* seen = nullptr;
    switch (match->kind) {
      case StyleWordKind::kWeight:
        seen = &weight;
        face.weight = match->weight;
        face.min_weight = match->min_weight;
        face.max_weight = match->max_weight;
        break;
      case StyleWordKind::kSlope:
        seen = &slope;
        face.italic = true;
        break;
      case StyleWordKind::kSuffixMT:
        seen = &suffix_mt;
        break;
      case StyleWordKind::kSuffixPS:
        seen = &suffix_ps;
        break;
    }
    if (*seen) {
      return false;
    }
    *seen = true;
    i += match->text.size();
  }
  return true;
}

}  // namespace

// Decodes a FontName into normalized code points: ASCII letters lower-cased,
// ASCII whitespace skipped, malformed sequences as U+FFFD.
class NormalizedFontNameReader {
 public:
  explicit NormalizedFontNameReader(const FontName& name) : name_(name) {}

  // The bytes (UTF-8, Latin-1) or code units (UTF-16) read so far: after
  // Next() returned a code point, where it ends.
  size_t position() const { return pos_; }

  // Stores the next normalized code point in `out`; false at the end.
  bool Next(char32_t& out) {
    char32_t c = 0;
    while (NextRaw(c)) {
      if (IsAsciiWhitespace(c)) {
        continue;
      }
      if (c >= 'A' && c <= 'Z') {
        c += 'a' - 'A';
      }
      out = c;
      return true;
    }
    return false;
  }

 private:
  bool NextRaw(char32_t& out) {
    switch (name_.encoding_) {
      case FontName::Encoding::kLatin1:
        if (pos_ >= name_.bytes_.size()) {
          return false;
        }
        out = name_.bytes_[pos_++];
        return true;
      case FontName::Encoding::kUtf8:
        return NextUtf8(out);
      case FontName::Encoding::kUtf16:
        return NextUtf16(out);
    }
    return false;
  }

  // Well-formed UTF-8 only (no overlongs, surrogates or values above
  // U+10FFFF); anything else yields U+FFFD and skips one byte.
  bool NextUtf8(char32_t& out) {
    const base::span<const uint8_t> bytes = name_.bytes_;
    if (pos_ >= bytes.size()) {
      return false;
    }
    const uint8_t lead = bytes[pos_];
    if (lead < 0x80) {
      out = lead;
      ++pos_;
      return true;
    }
    size_t length = 0;
    char32_t c = 0;
    uint8_t second_min = 0x80;
    uint8_t second_max = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf) {
      length = 2;
      c = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      length = 3;
      c = lead & 0x0f;
      if (lead == 0xe0) {
        second_min = 0xa0;
      } else if (lead == 0xed) {
        second_max = 0x9f;
      }
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      length = 4;
      c = lead & 0x07;
      if (lead == 0xf0) {
        second_min = 0x90;
      } else if (lead == 0xf4) {
        second_max = 0x8f;
      }
    }
    if (length == 0 || bytes.size() - pos_ < length) {
      out = kReplacementCharacter;
      ++pos_;
      return true;
    }
    for (size_t i = 1; i < length; ++i) {
      const uint8_t continuation = bytes[pos_ + i];
      const uint8_t min = i == 1 ? second_min : uint8_t{0x80};
      const uint8_t max = i == 1 ? second_max : uint8_t{0xbf};
      if (continuation < min || continuation > max) {
        out = kReplacementCharacter;
        ++pos_;
        return true;
      }
      c = (c << 6) | (continuation & 0x3f);
    }
    pos_ += length;
    out = c;
    return true;
  }

  // Lone surrogates yield U+FFFD.
  bool NextUtf16(char32_t& out) {
    const base::span<const char16_t> units = name_.units_;
    if (pos_ >= units.size()) {
      return false;
    }
    const char16_t lead = units[pos_++];
    if (lead < 0xd800 || lead > 0xdfff) {
      out = lead;
      return true;
    }
    if (lead <= 0xdbff && pos_ < units.size()) {
      const char16_t trail = units[pos_];
      if (trail >= 0xdc00 && trail <= 0xdfff) {
        ++pos_;
        out = 0x10000 + ((static_cast<char32_t>(lead) - 0xd800) << 10) +
              (static_cast<char32_t>(trail) - 0xdc00);
        return true;
      }
    }
    out = kReplacementCharacter;
    return true;
  }

  const FontName name_;
  size_t pos_ = 0;
};

FontPlatform CurrentFontPlatform() {
#if BUILDFLAG(IS_WIN)
  return FontPlatform::kWindows;
#elif BUILDFLAG(IS_MAC)
  return FontPlatform::kMac;
#elif BUILDFLAG(IS_ANDROID)
  return FontPlatform::kAndroid;
#else
  return FontPlatform::kLinux;
#endif
}

FontName::FontName(Encoding encoding,
                   base::span<const uint8_t> bytes,
                   base::span<const char16_t> units)
    : encoding_(encoding), bytes_(bytes), units_(units) {}

// static
FontName FontName::Utf8(std::string_view name) {
  return FontName(Encoding::kUtf8, base::as_byte_span(name), {});
}

// static
FontName FontName::Latin1(base::span<const uint8_t> name) {
  return FontName(Encoding::kLatin1, name, {});
}

// static
FontName FontName::Utf16(base::span<const char16_t> name) {
  return FontName(Encoding::kUtf16, {}, name);
}

uint64_t FontName::Hash() const {
  NormalizedFontNameReader reader(*this);
  uint64_t hash = kFnvOffsetBasis;
  char32_t c = 0;
  while (reader.Next(c)) {
    hash ^= c;
    hash *= kFnvPrime;
  }
  return hash;
}

bool FontName::Equals(const FontName& other) const {
  NormalizedFontNameReader a(*this);
  NormalizedFontNameReader b(other);
  while (true) {
    char32_t ca = 0;
    char32_t cb = 0;
    const bool more_a = a.Next(ca);
    const bool more_b = b.Next(cb);
    if (more_a != more_b || ca != cb) {
      return false;
    }
    if (!more_a) {
      return true;
    }
  }
}

bool FontName::IsEmpty() const {
  NormalizedFontNameReader reader(*this);
  char32_t c = 0;
  return !reader.Next(c);
}

FontName FontName::Prefix(size_t length) const {
  if (encoding_ == Encoding::kUtf16) {
    return FontName(encoding_, {},
                    units_.first(std::min(length, units_.size())));
  }
  return FontName(encoding_, bytes_.first(std::min(length, bytes_.size())),
                  {});
}

const FontListEntry* FindInFontList(FontPlatform platform,
                                    const FontName& name,
                                    uint64_t hash) {
  const FontListIndex& index = IndexFor(platform);
  const base::span<const IndexedName> names =
      base::span<const IndexedName>(index.names).first(index.size);
  const base::span<const FontListEntry> list = FontListFor(platform);
  for (auto it = std::ranges::lower_bound(names, hash, {}, &IndexedName::hash);
       it != names.end() && it->hash == hash; ++it) {
    const FontListEntry& entry = list[it->entry];
    if (name.Equals(FontName::Utf8(entry.name))) {
      return &entry;
    }
  }
  return nullptr;
}

bool IsAllowListedFamily(FontPlatform platform,
                         const FontName& family,
                         uint64_t hash) {
  const FontListEntry* entry = FindInFontList(platform, family, hash);
  return entry && entry->alias_of.empty();
}

FontVisibility::FontVisibility(Access access,
                               FontPlatform platform,
                               std::optional<Farbler> farbler)
    : access_(access), platform_(platform), farbler_(std::move(farbler)) {}

// static
FontVisibility FontVisibility::All() {
  return FontVisibility(Access::kAll, CurrentFontPlatform(), std::nullopt);
}

// static
FontVisibility FontVisibility::AllowList(FontPlatform platform) {
  return FontVisibility(Access::kAllowList, platform, std::nullopt);
}

// static
FontVisibility FontVisibility::AllowListAndSample(FontPlatform platform,
                                                  const Farbler& farbler) {
  return FontVisibility(Access::kAllowListAndSample, platform, farbler);
}

FontVisibility::Request FontVisibility::ForFamilyRequest(
    const FontName& requested) const {
  using Kind = Request::Kind;
  if (!restricts() || requested.Equals(FontName::Utf8("system-ui"))) {
    return {Kind::kUnrestricted, {}};
  }
  const uint64_t hash = requested.Hash();
  const FontListEntry* entry = FindInFontList(platform_, requested, hash);
  if (entry && !entry->alias_of.empty()) {
    // In this site's sample, the name may reach the real font (if this
    // machine has it); otherwise every machine gets the listed family.
    if (IsSampled(hash)) {
      return {Kind::kLookUp, {}};
    }
    return {Kind::kLookUpInstead, entry->alias_of};
  }
  if (entry || IsSampled(hash)) {
    return {Kind::kLookUp, {}};
  }
  return {Kind::kBlocked, {}};
}

bool FontVisibility::IsFamilyVisible(const FontName& family) const {
  if (!restricts()) {
    return true;
  }
  if (family.IsEmpty()) {
    return false;
  }
  const uint64_t hash = family.Hash();
  return IsFamilyVisible(hash, IsAllowListedFamily(platform_, family, hash));
}

bool FontVisibility::IsFamilyVisible(uint64_t family_hash,
                                     bool allow_listed) const {
  return !restricts() || allow_listed || IsSampled(family_hash);
}

bool FontVisibility::IsSampled(uint64_t hash) const {
  return access_ == Access::kAllowListAndSample && farbler_ &&
         farbler_->Prf(kDomainFontSample, hash) % kExtraFontOneIn == 0;
}

bool FontVisibility::LocalFace::StyleMatches(int face_weight,
                                             bool face_italic) const {
  return !styled || (face_weight >= min_weight && face_weight <= max_weight &&
                     face_italic == italic);
}

FontVisibility::LocalFaceRequest FontVisibility::ForLocalFaceRequest(
    const FontName& unique_name) const {
  LocalFaceRequest request;
  if (!restricts()) {
    return request;
  }
  // The normalized code points, where each ends in `unique_name`, and the
  // hash of the name up to and including it (FontName::Hash() of that
  // prefix).
  std::array<char32_t, kMaxLocalFaceNameLength> code_points{};
  std::array<size_t, kMaxLocalFaceNameLength> ends{};
  std::array<uint64_t, kMaxLocalFaceNameLength> hashes{};
  size_t length = 0;
  NormalizedFontNameReader reader(unique_name);
  uint64_t hash = kFnvOffsetBasis;
  char32_t c = 0;
  while (reader.Next(c)) {
    if (length == kMaxLocalFaceNameLength) {
      return request;  // No face has a name that long.
    }
    hash ^= c;
    hash *= kFnvPrime;
    code_points[length] = c;
    ends[length] = reader.position();
    hashes[length] = hash;
    ++length;
  }
  const base::span<const char32_t> name =
      base::span<const char32_t>(code_points).first(length);
  // Longest family first: "DejaVu Sans Condensed Bold" is DejaVu Sans
  // Condensed's bold face.
  for (size_t family_length = length;
       family_length > 0 && request.size < kMaxLocalFaceCandidates;
       --family_length) {
    LocalFace face;
    if (!ReadStyleWords(name.subspan(family_length), face)) {
      continue;
    }
    const size_t end = ends[family_length - 1];
    const uint64_t family_hash = hashes[family_length - 1];
    const FontName family = unique_name.Prefix(end);
    // Blink's name for the platform UI font, not a face.
    if (family.Equals(FontName::Utf8("system-ui"))) {
      continue;
    }
    const FontListEntry* entry = FindInFontList(platform_, family, family_hash);
    if (entry && entry->alias_of.empty()) {
      face.listed = entry->name;
    } else if (IsSampled(family_hash)) {
      // A sampled name, or an alias in the sample (which CSS looks up as
      // is, ForFamilyRequest()).
      face.prefix_length = end;
    } else {
      // Hidden, or an alias outside the sample: CSS would look up the
      // listed family instead, which is not this face's family.
      continue;
    }
    face.family_hash = family_hash;
    request.faces[request.size++] = face;
  }
  return request;
}

}  // namespace blink::molt_fingerprinting
