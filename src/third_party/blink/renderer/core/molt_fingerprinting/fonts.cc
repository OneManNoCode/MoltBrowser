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
    case FontPlatform::kLinux:
      break;
  }
  static const FontListIndex index = BuildIndex(FontPlatform::kLinux);
  return index;
}

}  // namespace

// Decodes a FontName into normalized code points: ASCII letters lower-cased,
// ASCII whitespace skipped, malformed sequences as U+FFFD.
class NormalizedFontNameReader {
 public:
  explicit NormalizedFontNameReader(const FontName& name) : name_(name) {}

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

}  // namespace blink::molt_fingerprinting
