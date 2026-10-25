// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"

namespace blink::molt_fingerprinting::moltnet {

namespace {

// ICU UScriptCode values this file needs (moltnet_fonts.cc checks them
// against ICU's).
constexpr int kScriptCommon = 0;

struct GenericKeyword {
  std::string_view name;
  Generic generic;
};
// Blink's generic family keywords, and the names Blink itself uses for
// generic families (FontFamily names it looks up as is).
constexpr GenericKeyword kGenericKeywords[] = {
    {"serif", Generic::kSerif},
    {"sans-serif", Generic::kSansSerif},
    {"monospace", Generic::kMonospace},
    {"cursive", Generic::kCursive},
    {"fantasy", Generic::kFantasy},
    {"math", Generic::kMath},
    {"system-ui", Generic::kSystemUi},
    {"-webkit-standard", Generic::kStandard},
    {"-webkit-body", Generic::kStandard},
    // Blink's name for the platform UI font (macOS), which pages also use.
    {"BlinkMacSystemFont", Generic::kSystemUi},
};

// ASCII lower case of the BCP 47 subtag of `locale` that starts at `pos`
// (subtags are separated by '-' or '_'), and where the next one starts.
std::string_view Subtag(std::string_view locale,
                        size_t pos,
                        std::array<char, 8>& buffer,
                        size_t* next) {
  size_t end = pos;
  while (end < locale.size() && locale[end] != '-' && locale[end] != '_') {
    ++end;
  }
  *next = end < locale.size() ? end + 1 : end;
  const size_t length = end - pos;
  if (length == 0 || length > buffer.size()) {
    return {};
  }
  for (size_t i = 0; i < length; ++i) {
    char c = locale[pos + i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    buffer[i] = c;
  }
  return std::string_view(buffer.data(), length);
}

}  // namespace

BundledFontSet::BundledFontSet(const BundledFontTable& table) : table_(table) {}

// static
bool BundledFontSet::IsGenericKeyword(const FontName& name, Generic* generic) {
  for (const GenericKeyword& keyword : kGenericKeywords) {
    if (name.Equals(FontName::Utf8(keyword.name))) {
      if (generic) {
        *generic = keyword.generic;
      }
      return true;
    }
  }
  return false;
}

std::optional<uint8_t> BundledFontSet::FamilyForName(
    const FontName& name) const {
  const uint64_t hash = name.Hash();
  for (size_t i = 0; i < table_.families.size(); ++i) {
    const BundledFontFamily& family = table_.families[i];
    if (family.hash == hash && name.Equals(FontName::Utf8(family.name))) {
      return static_cast<uint8_t>(i);
    }
  }
  for (const BundledFontName& other : table_.names) {
    if (other.hash == hash && name.Equals(FontName::Utf8(other.name))) {
      return other.family;
    }
  }
  Generic generic = Generic::kStandard;
  if (IsGenericKeyword(name, &generic)) {
    return table_.generic[static_cast<size_t>(generic)];
  }
  return std::nullopt;
}

uint8_t BundledFontSet::FamilyForGeneric(Generic generic,
                                         int script,
                                         std::string_view locale) const {
  const uint8_t latin = table_.generic[static_cast<size_t>(generic)];
  if (generic == Generic::kMath) {
    return latin;
  }
  for (const CjkScript& cjk : table_.cjk_scripts) {
    if (cjk.script != script) {
      continue;
    }
    if (generic == Generic::kMonospace) {
      return table_.cjk_monospace != kNoFamily ? table_.cjk_monospace : latin;
    }
    const HanRegion region = cjk.by_locale ? RegionForLocale(locale)
                                           : static_cast<HanRegion>(cjk.region);
    return table_.cjk_regions[static_cast<size_t>(region)];
  }
  return latin;
}

BundledFontSet::FaceMatch BundledFontSet::MatchFace(uint8_t family_index,
                                                    int weight,
                                                    bool italic) const {
  FaceMatch match;
  if (family_index >= table_.families.size()) {
    return match;
  }
  const BundledFontFamily& family = table_.families[family_index];
  match.face = family.first_face;
  if (weight < 1 || weight > 1000) {
    weight = 400;
  }
  // The slope: faces of the asked-for slope, or else of the other one.
  bool slope = italic;
  bool any = false;
  for (size_t i = 0; i < family.face_count; ++i) {
    any |= table_.faces[family.first_face + i].italic == slope;
  }
  if (!any) {
    slope = !slope;
  }
  // The weight (CSS Fonts 4, 5.2 step 4.c): a face whose range holds it;
  // else, for 400..500, the nearest heavier face up to 500, then the
  // nearest lighter one, then heavier ones beyond 500; for less than 400
  // lighter first, then heavier; above 500 heavier first, then lighter.
  // Each face is scored; the lowest score wins, the first face on a tie.
  int best_score = 0;
  bool found = false;
  for (size_t i = 0; i < family.face_count; ++i) {
    const uint8_t index = static_cast<uint8_t>(family.first_face + i);
    const BundledFontFace& face = table_.faces[index];
    if (face.italic != slope) {
      continue;
    }
    const int lo = face.min_weight;
    const int hi = face.max_weight;
    int score = 0;
    if (weight >= lo && weight <= hi) {
      score = 0;
    } else if (weight >= 400 && weight <= 500) {
      if (lo > weight && lo <= 500) {
        score = 1000 + (lo - weight);
      } else if (hi < weight) {
        score = 3000 + (weight - hi);
      } else {
        score = 5000 + (lo - weight);
      }
    } else if (weight < 400) {
      score = hi < weight ? 1000 + (weight - hi) : 3000 + (lo - weight);
    } else {
      score = lo > weight ? 1000 + (lo - weight) : 3000 + (weight - hi);
    }
    if (!found || score < best_score) {
      found = true;
      best_score = score;
      match.face = index;
    }
  }
  const BundledFontFace& chosen = table_.faces[match.face];
  if (chosen.variable_weight) {
    const int clamped =
        weight < chosen.min_weight
            ? chosen.min_weight
            : (weight > chosen.max_weight ? chosen.max_weight : weight);
    match.weight = static_cast<uint16_t>(clamped);
  } else {
    match.weight = chosen.default_weight;
  }
  return match;
}

HanRegion BundledFontSet::RegionForLocale(std::string_view locale) const {
  const HanRegion fallback = static_cast<HanRegion>(table_.cjk_default_region);
  std::array<char, 8> buffer{};
  size_t next = 0;
  const std::string_view language = Subtag(locale, 0, buffer, &next);
  if (language == "ja") {
    return HanRegion::kJP;
  }
  if (language == "ko") {
    return HanRegion::kKR;
  }
  if (language != "zh" && language != "yue" && language != "cmn") {
    return fallback;
  }
  // zh-[script]-[region]: the script decides, else the region.
  HanRegion region = language == "yue" ? HanRegion::kHK : HanRegion::kSC;
  bool script_given = false;
  size_t pos = next;
  for (int subtags = 0; pos < locale.size() && subtags < 4; ++subtags) {
    std::array<char, 8> tag_buffer{};
    const std::string_view tag = Subtag(locale, pos, tag_buffer, &next);
    pos = next;
    if (tag == "hant") {
      region = HanRegion::kTC;
      script_given = true;
    } else if (tag == "hans") {
      region = HanRegion::kSC;
      script_given = true;
    } else if (tag == "hk" || tag == "mo") {
      // Hong Kong and Macau write Traditional Chinese with HK glyphs;
      // zh-Hant-HK too.
      if (!script_given || region == HanRegion::kTC) {
        region = HanRegion::kHK;
      }
    } else if (tag == "tw") {
      if (!script_given) {
        region = HanRegion::kTC;
      }
    } else if (tag == "cn" || tag == "sg") {
      if (!script_given) {
        region = HanRegion::kSC;
      }
    }
  }
  return region;
}

HanRegion BundledFontSet::RegionForScript(int script,
                                          HanRegion han_region) const {
  for (const CjkScript& cjk : table_.cjk_scripts) {
    if (cjk.script == script && !cjk.by_locale) {
      return static_cast<HanRegion>(cjk.region);
    }
  }
  return han_region;
}

size_t BundledFontSet::FallbackFamilies(
    int script,
    HanRegion region,
    Presentation presentation,
    base::span<uint8_t, kMaxFallbackFamilies> out) const {
  std::array<bool, kMaxFallbackFamilies> used{};
  size_t count = 0;
  auto add = [&](uint8_t family) {
    if (family >= table_.families.size() || family >= used.size() ||
        used[family] || count >= out.size()) {
      return;
    }
    used[family] = true;
    out[count++] = family;
  };
  auto add_cjk = [&](HanRegion first) {
    add(table_.cjk_regions[static_cast<size_t>(first)]);
    for (const uint8_t family : table_.cjk_regions) {
      add(family);
    }
  };
  auto add_list = [&](uint16_t key) -> bool {
    for (const ScriptFallback& entry : table_.fallback) {
      if (entry.key != key) {
        continue;
      }
      const base::span<const uint8_t> families =
          table_.fallback_families.subspan(entry.first, entry.count);
      for (const uint8_t family : families) {
        if (family == kFallbackCjk) {
          add_cjk(RegionForScript(script, region));
        } else {
          add(family);
        }
      }
      return true;
    }
    return false;
  };
  switch (presentation) {
    case Presentation::kEmoji:
      add_list(kFallbackEmoji);
      break;
    case Presentation::kTextEmoji:
      add_list(kFallbackTextEmoji);
      break;
    case Presentation::kText:
      break;
  }
  const uint16_t key = script >= 0 && script < kFallbackTextEmoji
                           ? static_cast<uint16_t>(script)
                           : uint16_t{kScriptCommon};
  if (!add_list(key) && key != kScriptCommon) {
    // A script without a list of its own: the common list, then every
    // family.
    add_list(kScriptCommon);
  }
  for (size_t i = 0; i < table_.families.size(); ++i) {
    add(static_cast<uint8_t>(i));
  }
  return count;
}

std::optional<uint8_t> BundledFontSet::FaceForUniqueName(
    const FontName& name) const {
  for (size_t i = 0; i < table_.faces.size(); ++i) {
    const BundledFontFace& face = table_.faces[i];
    if (name.Equals(FontName::Utf8(face.postscript_name)) ||
        name.Equals(FontName::Utf8(face.full_name))) {
      return static_cast<uint8_t>(i);
    }
  }
  return std::nullopt;
}

}  // namespace blink::molt_fingerprinting::moltnet
