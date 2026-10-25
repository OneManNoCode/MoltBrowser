// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

// ICU UScriptCode values (uscript.h).
constexpr int kCommon = 0;
constexpr int kInherited = 1;
constexpr int kArabic = 2;
constexpr int kBopomofo = 5;
constexpr int kCyrillic = 8;
constexpr int kDevanagari = 10;
constexpr int kHan = 17;
constexpr int kHangul = 18;
constexpr int kHebrew = 19;
constexpr int kHiragana = 20;
constexpr int kKatakana = 22;
constexpr int kLatin = 25;
constexpr int kThai = 38;
constexpr int kKatakanaOrHiragana = 54;
constexpr int kSimplifiedHan = 73;
constexpr int kTraditionalHan = 74;
constexpr int kJapanese = 105;
constexpr int kKorean = 119;
constexpr int kGothic = 13;  // A script the set has no font for.

const BundledFontSet& Set() {
  static const BundledFontSet set(GetBundledFontTable());
  return set;
}

std::string_view FamilyName(uint8_t family) {
  return Set().table().families[family].name;
}

std::optional<std::string_view> Resolve(std::string_view name) {
  const std::optional<uint8_t> family =
      Set().FamilyForName(FontName::Utf8(name));
  if (!family) {
    return std::nullopt;
  }
  return FamilyName(*family);
}

uint8_t FamilyIndex(std::string_view name) {
  const std::optional<uint8_t> family =
      Set().FamilyForName(FontName::Utf8(name));
  EXPECT_TRUE(family.has_value()) << name;
  return family.value_or(0);
}

std::vector<std::string_view> Fallback(
    int script,
    HanRegion region,
    BundledFontSet::Presentation presentation =
        BundledFontSet::Presentation::kText) {
  std::array<uint8_t, kMaxFallbackFamilies> buffer{};
  const size_t count =
      Set().FallbackFamilies(script, region, presentation, buffer);
  std::vector<std::string_view> names;
  for (size_t i = 0; i < count; ++i) {
    names.push_back(FamilyName(buffer[i]));
  }
  return names;
}

const BundledFontFace& Face(uint8_t index) {
  return Set().table().faces[index];
}

TEST(MoltNetFontSetTest, TableIsConsistent) {
  const BundledFontTable& table = GetBundledFontTable();
  ASSERT_FALSE(table.files.empty());
  ASSERT_FALSE(table.families.empty());
  EXPECT_FALSE(table.version.empty());
  std::set<std::string> install_names;
  for (const BundledFontFile& file : table.files) {
    EXPECT_GT(file.size, 0u) << file.install_name;
    EXPECT_TRUE(install_names.insert(std::string(file.install_name)).second);
    EXPECT_EQ(file.install_name.find('/'), std::string_view::npos);
    EXPECT_EQ(file.install_name.find('\\'), std::string_view::npos);
  }
  std::vector<int> faces_seen(table.faces.size(), 0);
  std::set<uint64_t> names;
  for (size_t i = 0; i < table.families.size(); ++i) {
    const BundledFontFamily& family = table.families[i];
    EXPECT_EQ(FontName::Utf8(family.name).Hash(), family.hash) << family.name;
    EXPECT_TRUE(names.insert(family.hash).second) << family.name;
    ASSERT_GT(family.face_count, 0) << family.name;
    for (size_t f = 0; f < family.face_count; ++f) {
      const size_t face = family.first_face + f;
      ASSERT_LT(face, table.faces.size());
      EXPECT_EQ(table.faces[face].family, i) << family.name;
      ++faces_seen[face];
    }
  }
  for (size_t i = 0; i < table.faces.size(); ++i) {
    const BundledFontFace& face = table.faces[i];
    EXPECT_EQ(faces_seen[i], 1) << "every face in exactly one family";
    EXPECT_LT(face.file, table.files.size());
    EXPECT_LE(face.min_weight, face.default_weight);
    EXPECT_LE(face.default_weight, face.max_weight);
    EXPECT_EQ(face.variable_weight, face.min_weight != face.max_weight);
    EXPECT_FALSE(face.postscript_name.empty());
    EXPECT_FALSE(face.full_name.empty());
  }
  for (const BundledFontName& name : table.names) {
    EXPECT_EQ(FontName::Utf8(name.name).Hash(), name.hash) << name.name;
    EXPECT_TRUE(names.insert(name.hash).second)
        << name.name << " shadows another name";
    EXPECT_LT(name.family, table.families.size());
    EXPECT_FALSE(
        BundledFontSet::IsGenericKeyword(FontName::Utf8(name.name), nullptr))
        << name.name;
  }
  for (const uint8_t family : table.generic) {
    EXPECT_LT(family, table.families.size());
  }
  for (const uint8_t family : table.cjk_regions) {
    EXPECT_LT(family, table.families.size());
  }
  EXPECT_LT(table.cjk_default_region, kHanRegionCount);
  for (const ScriptFallback& entry : table.fallback) {
    ASSERT_LE(entry.first + entry.count, table.fallback_families.size());
    for (size_t i = 0; i < entry.count; ++i) {
      const uint8_t family = table.fallback_families[entry.first + i];
      EXPECT_TRUE(family == kFallbackCjk || family < table.families.size());
    }
  }
}

TEST(MoltNetFontSetTest, VisibilityListIsTheSet) {
  const base::span<const FontListEntry> list =
      FontListFor(FontPlatform::kMoltNetBundled);
  EXPECT_EQ(list.data(), GetBundledFontTable().visibility_list.data());
  EXPECT_EQ(list.size(),
            GetBundledFontTable().families.size() +
                GetBundledFontTable().names.size());
  for (const FontListEntry& entry : list) {
    const FontName name = FontName::Utf8(entry.name);
    EXPECT_EQ(FindInFontList(FontPlatform::kMoltNetBundled, name, name.Hash()),
              &entry)
        << entry.name;
    // The same family the set itself resolves the name to.
    const std::optional<std::string_view> resolved = Resolve(entry.name);
    ASSERT_TRUE(resolved.has_value()) << entry.name;
    if (!entry.alias_of.empty()) {
      EXPECT_EQ(*resolved, entry.alias_of) << entry.name;
    }
  }
  // Every family the set's typefaces report is a listed family (not an
  // alias), so IsVisible() accepts every font of the set.
  for (const BundledFontFamily& family : GetBundledFontTable().families) {
    const FontName name = FontName::Utf8(family.name);
    EXPECT_TRUE(IsAllowListedFamily(FontPlatform::kMoltNetBundled, name,
                                    name.Hash()))
        << family.name;
  }

  const FontVisibility visibility =
      FontVisibility::AllowList(FontPlatform::kMoltNetBundled);
  using Kind = FontVisibility::Request::Kind;
  EXPECT_EQ(visibility.ForFamilyRequest(FontName::Utf8("Arimo")).kind,
            Kind::kLookUp);
  const FontVisibility::Request arial =
      visibility.ForFamilyRequest(FontName::Utf8("arial"));
  EXPECT_EQ(arial.kind, Kind::kLookUpInstead);
  EXPECT_EQ(arial.instead, "Arimo");
  // OS fonts are not in the set, whatever the OS.
  for (std::string_view hidden :
       {"Segoe UI", "Helvetica Neue", "DejaVu Sans", "Calibri", "Menlo",
        "Apple Color Emoji", "Segoe UI Emoji", "SimSun", "Hiragino Sans"}) {
    EXPECT_EQ(visibility.ForFamilyRequest(FontName::Utf8(hidden)).kind,
              Kind::kBlocked)
        << hidden;
  }
  EXPECT_TRUE(visibility.IsFamilyVisible(FontName::Utf8("Noto Sans CJK JP")));
  EXPECT_FALSE(visibility.IsFamilyVisible(FontName::Utf8("Liberation Sans")))
      << "an alias is a request name, never a resolved family";
}

TEST(MoltNetFontSetTest, NamesResolveInEveryEncodingAndSpelling) {
  EXPECT_EQ(Resolve("Arimo"), "Arimo");
  EXPECT_EQ(Resolve("  noto SANS cjk jp "), "Noto Sans CJK JP");
  EXPECT_EQ(Resolve("Noto Sans Hebrew Light"), "Noto Sans Hebrew");
  const std::u16string utf16 = u"Noto Color Emoji";
  EXPECT_EQ(FamilyName(Set()
                           .FamilyForName(FontName::Utf16(utf16))
                           .value_or(0)),
            "Noto Color Emoji");
  const std::string latin1 = "TINOS";
  EXPECT_EQ(FamilyName(Set()
                           .FamilyForName(FontName::Latin1(base::as_byte_span(
                               latin1)))
                           .value_or(0)),
            "Tinos");
  EXPECT_EQ(Resolve("Segoe UI"), std::nullopt);
  EXPECT_EQ(Resolve(""), std::nullopt);
  EXPECT_EQ(Resolve("Arimo Bold"), std::nullopt) << "faces are not families";
}

TEST(MoltNetFontSetTest, AliasesAndGenericKeywords) {
  EXPECT_EQ(Resolve("Arial"), "Arimo");
  EXPECT_EQ(Resolve("Helvetica"), "Arimo");
  EXPECT_EQ(Resolve("Times New Roman"), "Tinos");
  EXPECT_EQ(Resolve("Times"), "Tinos");
  EXPECT_EQ(Resolve("Courier New"), "Cousine");
  EXPECT_EQ(Resolve("Courier"), "Cousine");
  EXPECT_EQ(Resolve("sans-serif"), "Arimo");
  EXPECT_EQ(Resolve("serif"), "Tinos");
  EXPECT_EQ(Resolve("-webkit-standard"), "Tinos");
  EXPECT_EQ(Resolve("monospace"), "Cousine");
  EXPECT_EQ(Resolve("math"), "Noto Sans Math");
  EXPECT_EQ(Resolve("system-ui"), "Noto Sans");
  EXPECT_EQ(Resolve("BlinkMacSystemFont"), "Noto Sans");
  // Not generic in Blink 156: plain names the set doesn't have.
  EXPECT_EQ(Resolve("emoji"), std::nullopt);
  EXPECT_EQ(Resolve("ui-monospace"), std::nullopt);
}

// Blink rewrites some names before looking them up, on some OSes only
// (alternate_font_family.h: AdjustFamilyNameToAvoidUnsupportedFonts on
// Windows; AlternateFamilyName's Courier New -> Courier everywhere but
// Windows). Each rewrite must land on the same family as the original name,
// or MoltNet would differ between OSes.
TEST(MoltNetFontSetTest, BlinkPlatformRewritesDontChangeTheResult) {
  const std::pair<std::string_view, std::string_view> kRewrites[] = {
      {"Courier", "Courier New"},  {"MS Sans Serif", "Microsoft Sans Serif"},
      {"MS Serif", "Times New Roman"}, {"Times", "Times New Roman"},
      {"Courier New", "Courier"},  {"Arial", "Helvetica"},
      {"Helvetica", "Arial"},      {"Times New Roman", "Times"},
  };
  for (const auto& [from, to] : kRewrites) {
    EXPECT_EQ(Resolve(from), Resolve(to)) << from << " -> " << to;
  }
}

TEST(MoltNetFontSetTest, GenericFamiliesPerScript) {
  auto generic = [](Generic g, int script, std::string_view locale = "") {
    return FamilyName(Set().FamilyForGeneric(g, script, locale));
  };
  EXPECT_EQ(generic(Generic::kStandard, kLatin), "Tinos");
  EXPECT_EQ(generic(Generic::kSerif, kCyrillic), "Tinos");
  EXPECT_EQ(generic(Generic::kSansSerif, kLatin), "Arimo");
  EXPECT_EQ(generic(Generic::kMonospace, kLatin), "Cousine");
  EXPECT_EQ(generic(Generic::kCursive, kLatin), "Tinos");
  EXPECT_EQ(generic(Generic::kFantasy, kLatin), "Tinos");
  EXPECT_EQ(generic(Generic::kSystemUi, kLatin), "Noto Sans");
  EXPECT_EQ(generic(Generic::kMath, kJapanese), "Noto Sans Math");
  // CJK content gets its region's CJK face for every text generic.
  for (Generic g : {Generic::kStandard, Generic::kSerif, Generic::kSansSerif,
                    Generic::kSystemUi, Generic::kCursive}) {
    EXPECT_EQ(generic(g, kKatakanaOrHiragana), "Noto Sans CJK JP");
    EXPECT_EQ(generic(g, kJapanese), "Noto Sans CJK JP");
    EXPECT_EQ(generic(g, kHangul), "Noto Sans CJK KR");
    EXPECT_EQ(generic(g, kKorean), "Noto Sans CJK KR");
    EXPECT_EQ(generic(g, kSimplifiedHan, "zh-HK"), "Noto Sans CJK SC");
    EXPECT_EQ(generic(g, kTraditionalHan, "zh-TW"), "Noto Sans CJK TC");
    EXPECT_EQ(generic(g, kTraditionalHan, "zh-HK"), "Noto Sans CJK HK");
    EXPECT_EQ(generic(g, kHan, "ja"), "Noto Sans CJK JP");
    EXPECT_EQ(generic(g, kHan, "en-US"), "Noto Sans CJK SC");
  }
  // No CJK monospace face: the Latin one, CJK characters by fallback.
  EXPECT_EQ(generic(Generic::kMonospace, kJapanese), "Cousine");
  // A script the table doesn't know is Latin's.
  EXPECT_EQ(generic(Generic::kSansSerif, 9999), "Arimo");
  EXPECT_EQ(generic(Generic::kSansSerif, -1), "Arimo");
}

TEST(MoltNetFontSetTest, RegionForLocale) {
  const BundledFontSet& set = Set();
  EXPECT_EQ(set.RegionForLocale("ja"), HanRegion::kJP);
  EXPECT_EQ(set.RegionForLocale("ja-JP"), HanRegion::kJP);
  EXPECT_EQ(set.RegionForLocale("KO_kr"), HanRegion::kKR);
  EXPECT_EQ(set.RegionForLocale("zh"), HanRegion::kSC);
  EXPECT_EQ(set.RegionForLocale("zh-CN"), HanRegion::kSC);
  EXPECT_EQ(set.RegionForLocale("zh-SG"), HanRegion::kSC);
  EXPECT_EQ(set.RegionForLocale("zh-Hans"), HanRegion::kSC);
  EXPECT_EQ(set.RegionForLocale("zh-TW"), HanRegion::kTC);
  EXPECT_EQ(set.RegionForLocale("zh-Hant"), HanRegion::kTC);
  EXPECT_EQ(set.RegionForLocale("zh-HK"), HanRegion::kHK);
  EXPECT_EQ(set.RegionForLocale("zh-MO"), HanRegion::kHK);
  EXPECT_EQ(set.RegionForLocale("zh-Hant-HK"), HanRegion::kHK);
  EXPECT_EQ(set.RegionForLocale("zh-Hans-HK"), HanRegion::kSC);
  EXPECT_EQ(set.RegionForLocale("zh-Hant-TW"), HanRegion::kTC);
  EXPECT_EQ(set.RegionForLocale("yue"), HanRegion::kHK);
  EXPECT_EQ(set.RegionForLocale("yue-Hans"), HanRegion::kSC);
  // No region: the default.
  for (std::string_view other :
       {"", "en", "en-US", "-", "--", "zhx", "z", "japanese", "ja2",
        "\xff\xfe", "zh-verylongsubtag-HK"}) {
    const HanRegion region = set.RegionForLocale(other);
    if (other == "zh-verylongsubtag-HK") {
      EXPECT_EQ(region, HanRegion::kHK) << "an overlong subtag is skipped";
    } else {
      EXPECT_EQ(region, HanRegion::kSC) << other;
    }
  }
  EXPECT_EQ(set.RegionForScript(kKatakana, HanRegion::kSC), HanRegion::kJP);
  EXPECT_EQ(set.RegionForScript(kHiragana, HanRegion::kTC), HanRegion::kJP);
  EXPECT_EQ(set.RegionForScript(kHangul, HanRegion::kSC), HanRegion::kKR);
  EXPECT_EQ(set.RegionForScript(kBopomofo, HanRegion::kSC), HanRegion::kTC);
  EXPECT_EQ(set.RegionForScript(kHan, HanRegion::kHK), HanRegion::kHK);
  EXPECT_EQ(set.RegionForScript(kCommon, HanRegion::kKR), HanRegion::kKR);
}

TEST(MoltNetFontSetTest, MatchFaceFollowsCssFontMatching) {
  const BundledFontSet& set = Set();
  auto match = [&](std::string_view family, int weight, bool italic) {
    return set.MatchFace(FamilyIndex(family), weight, italic);
  };
  auto style = [&](std::string_view family, int weight, bool italic) {
    return Face(match(family, weight, italic).face).postscript_name;
  };
  // Four static faces.
  EXPECT_EQ(style("Tinos", 400, false), "Tinos-Regular");
  EXPECT_EQ(style("Tinos", 700, false), "Tinos-Bold");
  EXPECT_EQ(style("Tinos", 400, true), "Tinos-Italic");
  EXPECT_EQ(style("Tinos", 700, true), "Tinos-BoldItalic");
  // 400..500: nothing heavier up to 500, so the lighter one.
  EXPECT_EQ(style("Tinos", 500, false), "Tinos-Regular");
  EXPECT_EQ(style("Tinos", 450, false), "Tinos-Regular");
  // Above 500: heavier first.
  EXPECT_EQ(style("Tinos", 600, false), "Tinos-Bold");
  EXPECT_EQ(style("Tinos", 900, false), "Tinos-Bold");
  // Below 400: lighter first, none, so the nearest heavier.
  EXPECT_EQ(style("Tinos", 100, false), "Tinos-Regular");
  EXPECT_EQ(style("Tinos", 300, true), "Tinos-Italic");
  EXPECT_EQ(match("Tinos", 700, false).weight, 700);
  EXPECT_EQ(match("Tinos", 600, false).weight, 700);

  // Variable faces are instantiated at the weight, clamped to their range.
  EXPECT_EQ(style("Arimo", 550, false), "Arimo-Regular");
  EXPECT_EQ(match("Arimo", 550, false).weight, 550);
  EXPECT_EQ(match("Arimo", 900, false).weight, 700);
  EXPECT_EQ(match("Arimo", 100, true).weight, 400);
  EXPECT_TRUE(Face(match("Arimo", 100, true).face).italic);
  // Noto Sans CJK's file opens at Thin; a request for 400 is 400.
  EXPECT_EQ(match("Noto Sans CJK JP", 400, false).weight, 400);
  EXPECT_EQ(match("Noto Sans CJK JP", 100, false).weight, 100);
  EXPECT_EQ(match("Noto Sans CJK JP", 900, true).weight, 900);

  // One face only: the other slope is used (Blink then slants it).
  EXPECT_FALSE(Face(match("Noto Sans Math", 400, true).face).italic);
  // Out-of-range requests are the normal weight.
  EXPECT_EQ(match("Arimo", 0, false).weight, 400);
  EXPECT_EQ(match("Arimo", 5000, false).weight, 400);
  // A family index past the table is not a crash.
  EXPECT_EQ(set.MatchFace(250, 400, false).face, 0);
}

TEST(MoltNetFontSetTest, FallbackStaysInTheSetAndFollowsTheScript) {
  const size_t family_count = GetBundledFontTable().families.size();
  for (int script : {kCommon, kInherited, kArabic, kLatin, kHan, kKatakana,
                     kHangul, kDevanagari, kThai, kHebrew, kGothic, -7,
                     70000}) {
    for (auto presentation : {BundledFontSet::Presentation::kText,
                              BundledFontSet::Presentation::kEmoji,
                              BundledFontSet::Presentation::kTextEmoji}) {
      const std::vector<std::string_view> order =
          Fallback(script, HanRegion::kSC, presentation);
      // Every family exactly once.
      EXPECT_EQ(order.size(), family_count) << script;
      EXPECT_EQ(std::set<std::string_view>(order.begin(), order.end()).size(),
                family_count)
          << script;
    }
  }
  EXPECT_EQ(Fallback(kArabic, HanRegion::kSC)[0], "Noto Naskh Arabic");
  EXPECT_EQ(Fallback(kDevanagari, HanRegion::kSC)[0], "Noto Sans Devanagari");
  EXPECT_EQ(Fallback(kThai, HanRegion::kSC)[0], "Noto Sans Thai");
  EXPECT_EQ(Fallback(kHebrew, HanRegion::kSC)[0], "Noto Sans Hebrew");
  const std::vector<std::string_view> latin =
      Fallback(kLatin, HanRegion::kSC);
  EXPECT_EQ(latin[0], "Arimo");
  EXPECT_EQ(latin[1], "Noto Sans");
  // Han by the content's region, then the other regions.
  const std::vector<std::string_view> han = Fallback(kHan, HanRegion::kJP);
  EXPECT_EQ(han[0], "Noto Sans CJK JP");
  EXPECT_EQ(han[1], "Noto Sans CJK KR");
  EXPECT_EQ(Fallback(kHan, HanRegion::kTC)[0], "Noto Sans CJK TC");
  EXPECT_EQ(Fallback(kHan, HanRegion::kHK)[0], "Noto Sans CJK HK");
  // Kana and Hangul fix their region.
  EXPECT_EQ(Fallback(kKatakana, HanRegion::kSC)[0], "Noto Sans CJK JP");
  EXPECT_EQ(Fallback(kHangul, HanRegion::kTC)[0], "Noto Sans CJK KR");
  // Punctuation and symbols: the Latin fonts, symbols, math, CJK (for CJK
  // punctuation, by the content's region), emoji.
  const std::vector<std::string_view> common =
      Fallback(kCommon, HanRegion::kKR);
  ASSERT_GE(common.size(), 7u);
  EXPECT_EQ(common[0], "Arimo");
  EXPECT_EQ(common[2], "Noto Sans Symbols");
  EXPECT_EQ(common[3], "Noto Sans Symbols 2");
  EXPECT_EQ(common[4], "Noto Sans Math");
  EXPECT_EQ(common[5], "Noto Sans CJK KR");
  // Emoji: colour first; text-style emoji: symbols first, colour last.
  EXPECT_EQ(
      Fallback(kCommon, HanRegion::kSC, BundledFontSet::Presentation::kEmoji)[0],
      "Noto Color Emoji");
  const std::vector<std::string_view> text_emoji = Fallback(
      kCommon, HanRegion::kSC, BundledFontSet::Presentation::kTextEmoji);
  EXPECT_EQ(text_emoji[0], "Noto Sans Symbols 2");
  // A script without fonts: the common list first.
  EXPECT_EQ(Fallback(kGothic, HanRegion::kSC)[0], "Arimo");
}

TEST(MoltNetFontSetTest, UniqueNames) {
  const BundledFontSet& set = Set();
  auto face = [&](std::string_view name) -> std::optional<std::string_view> {
    const std::optional<uint8_t> index =
        set.FaceForUniqueName(FontName::Utf8(name));
    if (!index) {
      return std::nullopt;
    }
    return Face(*index).postscript_name;
  };
  EXPECT_EQ(face("Tinos-Bold"), "Tinos-Bold");
  EXPECT_EQ(face("tinos bold"), "Tinos-Bold") << "the full name";
  EXPECT_EQ(face("Arimo-Regular"), "Arimo-Regular");
  EXPECT_EQ(face("NotoColorEmoji"), "NotoColorEmoji");
  EXPECT_EQ(face("ArialMT"), std::nullopt);
  EXPECT_EQ(face(""), std::nullopt);
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
