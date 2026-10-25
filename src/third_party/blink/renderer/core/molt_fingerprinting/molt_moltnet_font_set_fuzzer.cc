// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's bundled font set rules (moltnet_font_set.h) on what a page
// controls: the font family names and src: local() names it asks for, the
// content language its lang attributes give (any bytes), the script of the
// text, and font-weight / font-style.
//
// Checks: no memory or UB error; every answer is inside the set (a family,
// face or region the table has; a generic family never "none"); a name
// finds the same family however its ASCII letters are cased or whitespace
// is put in, and every name in the table finds its family; a generic
// keyword finds the family its generic maps to for Latin text; a unique name
// finds a face carrying that name; face matching keeps to the family, picks
// the asked-for slope when the family has it and a weight the face can
// render (the request itself when a face's range holds it); per-script
// fallback lists every family exactly once; a locale's Han region doesn't
// depend on ASCII case or on '-' versus '_'.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/containers/span.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

// USCRIPT_LATIN in ICU's uscript.h.
constexpr int kLatin = 25;

// `name` with ASCII letters' case flipped and ASCII whitespace inserted
// before ASCII bytes, as `p` decides: the same name in normalized form.
std::string Respell(std::string_view name, FuzzedDataProvider& p) {
  static constexpr char kWhitespace[] = {' ', '\t', '\n', '\f', '\r'};
  std::string out;
  for (char c : name) {
    if (static_cast<uint8_t>(c) < 0x80 && p.ConsumeBool()) {
      out.push_back(kWhitespace[p.ConsumeIntegralInRange<size_t>(0, 4)]);
    }
    if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) &&
        p.ConsumeBool()) {
      c = static_cast<char>(c ^ 0x20);
    }
    out.push_back(c);
  }
  return out;
}

// `locale` with ASCII case flipped and '-' and '_' swapped, as `p` decides.
std::string RespellLocale(std::string_view locale, FuzzedDataProvider& p) {
  std::string out(locale);
  for (char& c : out) {
    if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) &&
        p.ConsumeBool()) {
      c = static_cast<char>(c ^ 0x20);
    } else if ((c == '-' || c == '_') && p.ConsumeBool()) {
      c = c == '-' ? '_' : '-';
    }
  }
  return out;
}

void CheckFamilyIndex(const BundledFontTable& table, size_t family) {
  CHECK(family < table.families.size()) << "a family outside the set";
}

void CheckName(const BundledFontSet& set,
               std::string_view raw,
               FuzzedDataProvider& p) {
  const BundledFontTable& table = set.table();
  const FontName name = FontName::Utf8(raw);
  const std::optional<uint8_t> family = set.FamilyForName(name);
  if (family) {
    CheckFamilyIndex(table, *family);
  }
  const std::string respelled = Respell(raw, p);
  CHECK(set.FamilyForName(FontName::Utf8(respelled)) == family)
      << "respelling a family name changed what it finds";

  Generic generic = Generic::kStandard;
  if (BundledFontSet::IsGenericKeyword(name, &generic)) {
    CHECK(static_cast<size_t>(generic) < kGenericCount);
    // A family of the set named like a keyword would win; there is none.
    CHECK(family == set.FamilyForGeneric(generic, kLatin, ""))
        << "a generic keyword finds another family than its generic";
  }

  const std::optional<uint8_t> face = set.FaceForUniqueName(name);
  if (face) {
    CHECK(*face < table.faces.size());
    const BundledFontFace& f = table.faces[*face];
    CHECK(name.Equals(FontName::Utf8(f.postscript_name)) ||
          name.Equals(FontName::Utf8(f.full_name)))
        << "a unique name found a face without that name";
  }
  CHECK(set.FaceForUniqueName(FontName::Utf8(respelled)) == face);
}

void CheckGenerics(const BundledFontSet& set,
                   int script,
                   std::string_view locale) {
  for (size_t g = 0; g < kGenericCount; ++g) {
    const uint8_t family =
        set.FamilyForGeneric(static_cast<Generic>(g), script, locale);
    CHECK(family != kNoFamily) << "a generic family maps to nothing";
    CheckFamilyIndex(set.table(), family);
  }
}

void CheckMatch(const BundledFontSet& set,
                uint8_t family_index,
                int weight,
                bool italic) {
  const BundledFontTable& table = set.table();
  const BundledFontFamily& family = table.families[family_index];
  const BundledFontSet::FaceMatch match =
      set.MatchFace(family_index, weight, italic);
  CHECK(match.face >= family.first_face &&
        match.face < family.first_face + family.face_count)
      << "a face of another family";
  const BundledFontFace& face = table.faces[match.face];
  bool has_slope = false;
  for (size_t i = 0; i < family.face_count; ++i) {
    has_slope |= table.faces[family.first_face + i].italic == italic;
  }
  if (has_slope) {
    CHECK(face.italic == italic) << "the family has the slope; not chosen";
  }
  CHECK(match.weight >= face.min_weight && match.weight <= face.max_weight)
      << "a weight the face can't render";
  if (!face.variable_weight) {
    CHECK(match.weight == face.default_weight);
  }
  const int asked = weight >= 1 && weight <= 1000 ? weight : 400;
  if (asked >= face.min_weight && asked <= face.max_weight) {
    CHECK(match.weight == asked) << "the face holds the weight; not used";
  }
  // A face of the slope whose range holds the weight is always chosen.
  for (size_t i = 0; i < family.face_count; ++i) {
    const BundledFontFace& other = table.faces[family.first_face + i];
    if (other.italic == face.italic && asked >= other.min_weight &&
        asked <= other.max_weight) {
      CHECK(asked >= face.min_weight && asked <= face.max_weight)
          << "a face holding the weight was passed over";
      break;
    }
  }
}

void CheckFallback(const BundledFontSet& set,
                   int script,
                   HanRegion region,
                   BundledFontSet::Presentation presentation) {
  const BundledFontTable& table = set.table();
  std::array<uint8_t, kMaxFallbackFamilies> out{};
  const size_t count = set.FallbackFamilies(script, region, presentation,
                                            base::span(out));
  CHECK(count == table.families.size())
      << "fallback doesn't end with every family";
  std::array<bool, 256> seen{};
  for (size_t i = 0; i < count; ++i) {
    CheckFamilyIndex(table, out[i]);
    CHECK(!seen[out[i]]) << "a family twice in a fallback list";
    seen[out[i]] = true;
  }
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace blink::molt_fingerprinting;
  using namespace blink::molt_fingerprinting::moltnet;
  FuzzedDataProvider p(data, size);
  static const BundledFontSet set(GetBundledFontTable());
  const BundledFontTable& table = set.table();

  // A name the page made up, or one of the table's respelled.
  std::string name = p.ConsumeRandomLengthString(128);
  switch (p.ConsumeIntegralInRange<int>(0, 3)) {
    case 1: {
      const BundledFontFamily& f = table.families[
          p.ConsumeIntegralInRange<size_t>(0, table.families.size() - 1)];
      name = Respell(f.name, p);
      CHECK(set.FamilyForName(FontName::Utf8(name)) ==
            static_cast<uint8_t>(&f - table.families.data()))
          << "a family's own name doesn't find it";
      break;
    }
    case 2:
      if (!table.names.empty()) {
        const BundledFontName& n = table.names[
            p.ConsumeIntegralInRange<size_t>(0, table.names.size() - 1)];
        name = Respell(n.name, p);
        CHECK(set.FamilyForName(FontName::Utf8(name)) == n.family)
            << "another name of a family doesn't find it";
      }
      break;
    case 3: {
      const BundledFontFace& f = table.faces[
          p.ConsumeIntegralInRange<size_t>(0, table.faces.size() - 1)];
      name = Respell(p.ConsumeBool() ? f.postscript_name : f.full_name, p);
      CHECK(set.FaceForUniqueName(FontName::Utf8(name)).has_value())
          << "a face's unique name doesn't find a face";
      break;
    }
    default:
      break;
  }
  CheckName(set, name, p);

  const std::string locale = p.ConsumeRandomLengthString(64);
  const HanRegion region = set.RegionForLocale(locale);
  CHECK(static_cast<size_t>(region) < kHanRegionCount);
  CHECK(set.RegionForLocale(RespellLocale(locale, p)) == region)
      << "a locale's case or separators changed its region";

  const int script = p.ConsumeBool() ? p.ConsumeIntegralInRange<int>(0, 200)
                                     : p.ConsumeIntegral<int>();
  const HanRegion script_region = set.RegionForScript(script, region);
  CHECK(static_cast<size_t>(script_region) < kHanRegionCount);
  CheckGenerics(set, script, locale);

  const uint8_t family = static_cast<uint8_t>(
      p.ConsumeIntegralInRange<size_t>(0, table.families.size() - 1));
  const int weight = p.ConsumeBool() ? p.ConsumeIntegralInRange<int>(1, 1000)
                                     : p.ConsumeIntegral<int>();
  CheckMatch(set, family, weight, p.ConsumeBool());
  // An index outside the table is refused, not read.
  (void)set.MatchFace(static_cast<uint8_t>(table.families.size() +
                                           p.ConsumeIntegral<uint8_t>() %
                                               (256 - table.families.size())),
                      weight, false);

  CheckFallback(set, script, script_region,
                static_cast<BundledFontSet::Presentation>(
                    p.ConsumeIntegralInRange<int>(0, 2)));
  return 0;
}
