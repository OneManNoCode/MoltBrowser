// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting {
namespace {

constexpr std::array<FontPlatform, 4> kPlatforms = {
    FontPlatform::kWindows, FontPlatform::kMac, FontPlatform::kLinux,
    FontPlatform::kAndroid};

SiteKey KeyFromByte(uint8_t first) {
  SiteKey key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(first + 29 * i);
  }
  return key;
}

FontName Utf8(std::string_view name) {
  return FontName::Utf8(name);
}

bool Listed(FontPlatform platform, std::string_view name) {
  const FontName font_name = Utf8(name);
  return IsAllowListedFamily(platform, font_name, font_name.Hash());
}

std::string_view AliasOf(FontPlatform platform, std::string_view name) {
  const FontName font_name = Utf8(name);
  const FontListEntry* entry =
      FindInFontList(platform, font_name, font_name.Hash());
  return entry ? entry->alias_of : std::string_view();
}

TEST(MoltFontNameTest, IgnoresAsciiCaseAndWhitespace) {
  const FontName segoe = Utf8("Segoe UI");
  for (std::string_view variant :
       {"segoe ui", "SEGOE UI", "SegoeUI", " Segoe\tUI ", "s e g o e u i",
        "Segoe\r\n\fUI"}) {
    EXPECT_TRUE(segoe.Equals(Utf8(variant))) << variant;
    EXPECT_EQ(segoe.Hash(), Utf8(variant).Hash()) << variant;
  }
  EXPECT_FALSE(segoe.Equals(Utf8("Segoe UI Light")));
  EXPECT_FALSE(segoe.Equals(Utf8("Segoe U")));
  EXPECT_FALSE(segoe.Equals(Utf8("Segoe-UI")));
  EXPECT_FALSE(Utf8("").Equals(segoe));
  EXPECT_TRUE(Utf8("").Equals(Utf8(" \t ")));
  EXPECT_TRUE(Utf8(" \t ").IsEmpty());
  EXPECT_FALSE(segoe.IsEmpty());
}

TEST(MoltFontNameTest, NonAsciiIsComparedExactly) {
  // Only ASCII letters are folded: fullwidth Latin and accented letters keep
  // their identity (platform matchers treat them as different names too).
  EXPECT_FALSE(Utf8("\uFF2D\uFF33 Gothic").Equals(Utf8("MS Gothic")));
  EXPECT_FALSE(Utf8("Caf\u00C9").Equals(Utf8("Caf\u00E9")));
  EXPECT_TRUE(Utf8("CAF\u00E9").Equals(Utf8("caf\u00E9")));
  // A no-break space is not ASCII whitespace.
  EXPECT_FALSE(Utf8("Segoe\u00A0UI").Equals(Utf8("Segoe UI")));
}

TEST(MoltFontNameTest, EncodingsAgree) {
  const std::array<uint8_t, 4> latin1 = {'C', 'a', 'f', 0xe9};
  const FontName from_latin1 = FontName::Latin1(latin1);
  const FontName from_utf8 = Utf8("caf\u00E9");
  const std::u16string utf16 = u"CAF\u00E9";
  const FontName from_utf16 = FontName::Utf16(utf16);
  EXPECT_TRUE(from_latin1.Equals(from_utf8));
  EXPECT_TRUE(from_utf8.Equals(from_utf16));
  EXPECT_EQ(from_latin1.Hash(), from_utf8.Hash());
  EXPECT_EQ(from_utf8.Hash(), from_utf16.Hash());

  // Astral code points: a surrogate pair in UTF-16, four bytes in UTF-8.
  const std::u16string astral16 = u"A\U0001F600b";
  EXPECT_TRUE(FontName::Utf16(astral16).Equals(Utf8("a\U0001F600B")));
  EXPECT_EQ(FontName::Utf16(astral16).Hash(), Utf8("a\U0001F600B").Hash());

  // CJK names (as the allow-lists spell them) in UTF-16.
  const std::u16string ms_gothic = u"\uFF2D\uFF33 \u30B4\u30B7\u30C3\u30AF";
  EXPECT_TRUE(
      Listed(FontPlatform::kWindows, "\uFF2D\uFF33\u30B4\u30B7\u30C3\u30AF"));
  const FontName ms_gothic_name = FontName::Utf16(ms_gothic);
  EXPECT_TRUE(IsAllowListedFamily(FontPlatform::kWindows, ms_gothic_name,
                                  ms_gothic_name.Hash()));
}

TEST(MoltFontNameTest, MalformedInputDecodesToReplacementCharacters) {
  const FontName replacement = Utf8("a\uFFFDb");
  // Lone continuation byte, invalid lead bytes, overlong NUL, a UTF-8
  // encoded surrogate (one U+FFFD per byte), truncated sequences.
  EXPECT_TRUE(Utf8("a\x80"
                   "b")
                  .Equals(replacement));
  EXPECT_TRUE(Utf8("a\xff"
                   "b")
                  .Equals(replacement));
  EXPECT_TRUE(Utf8("a\xc0\x80"
                   "b")
                  .Equals(Utf8("a\uFFFD\uFFFD"
                               "b")));
  EXPECT_TRUE(Utf8("a\xed\xa0\x80"
                   "b")
                  .Equals(Utf8("a\uFFFD\uFFFD\uFFFD"
                               "b")));
  EXPECT_TRUE(Utf8("a\xf4\x90\x80\x80"
                   "b")
                  .Equals(Utf8("a\uFFFD\uFFFD\uFFFD\uFFFD"
                               "b")));
  EXPECT_TRUE(Utf8("ab\xe6\xb8").Equals(Utf8("ab\uFFFD\uFFFD")));
  EXPECT_TRUE(Utf8("\xf0").Equals(Utf8("\uFFFD")));
  // Lone surrogates in UTF-16.
  const std::u16string lone_high = {u'a', char16_t{0xd800}, u'b'};
  const std::u16string lone_low = {u'a', char16_t{0xdc00}, u'b'};
  const std::u16string trailing_high = {u'a', u'b', char16_t{0xdbff}};
  EXPECT_TRUE(FontName::Utf16(lone_high).Equals(replacement));
  EXPECT_TRUE(FontName::Utf16(lone_low).Equals(replacement));
  EXPECT_TRUE(FontName::Utf16(trailing_high).Equals(Utf8("ab\uFFFD")));
  // An embedded NUL is a code point like any other.
  EXPECT_FALSE(Utf8(std::string_view("a\0b", 3)).Equals(Utf8("ab")));
}

TEST(MoltFontListTest, EveryEntryIsFoundAndUnique) {
  for (FontPlatform platform : kPlatforms) {
    const base::span<const FontListEntry> list = FontListFor(platform);
    ASSERT_FALSE(list.empty());
    ASSERT_LE(list.size(), 512u);
    std::set<uint64_t> hashes;
    for (const FontListEntry& entry : list) {
      const FontName name = Utf8(entry.name);
      EXPECT_FALSE(name.IsEmpty());
      EXPECT_EQ(FindInFontList(platform, name, name.Hash()), &entry)
          << entry.name;
      // No two entries normalize to the same name (they would shadow each
      // other).
      EXPECT_TRUE(hashes.insert(name.Hash()).second) << entry.name;
      if (!entry.alias_of.empty()) {
        // Aliases point at listed families, never at other aliases.
        EXPECT_TRUE(Listed(platform, entry.alias_of)) << entry.name;
      }
    }
  }
}

TEST(MoltFontListTest, ListsHoldDefaultFontsOnly) {
  using P = FontPlatform;
  // OS defaults.
  for (std::string_view name :
       {"Arial", "Segoe UI", "segoe ui semibold", "Calibri", "Consolas",
        "Times New Roman", "Yu Gothic", "Microsoft YaHei", "Malgun Gothic",
        "Segoe UI Emoji", "Bahnschrift"}) {
    EXPECT_TRUE(Listed(P::kWindows, name)) << name;
  }
  for (std::string_view name :
       {"Helvetica", "Helvetica Neue", "Arial", "Times", "Menlo", "Monaco",
        "Avenir Next", "Hiragino Sans", "PingFang SC", "Apple Color Emoji",
        "Lucida Grande", "Georgia", "Courier"}) {
    EXPECT_TRUE(Listed(P::kMac, name)) << name;
  }
  for (std::string_view name :
       {"DejaVu Sans", "DejaVu Sans Mono", "Liberation Sans",
        "Liberation Serif", "Liberation Mono", "Noto Sans",
        "Noto Color Emoji"}) {
    EXPECT_TRUE(Listed(P::kLinux, name)) << name;
  }
  for (std::string_view name : {"sans-serif", "serif", "monospace", "Arial"}) {
    EXPECT_TRUE(Listed(P::kAndroid, name)) << name;
  }

  // Never listed: fonts users install, Features on Demand, Windows 11-only
  // fonts, Office / app fonts, fonts of other platforms.
  for (P platform : kPlatforms) {
    for (std::string_view name : {"Fira Code",
                                  "JetBrains Mono",
                                  "Inter",
                                  "Source Code Pro",
                                  "Comic Neue",
                                  "Aptos",
                                  "Segoe UI Variable",
                                  "Cascadia Code",
                                  "Meiryo",
                                  "MS Mincho",
                                  "SimHei",
                                  "Gulim",
                                  "Arial Nova",
                                  "SF Pro",
                                  "New York",
                                  "Ubuntu",
                                  "Cantarell",
                                  "Roboto",
                                  "Font Awesome 6 Free",
                                  "Wingdings 4",
                                  ""}) {
      EXPECT_FALSE(Listed(platform, name)) << name;
    }
  }
  EXPECT_FALSE(Listed(P::kMac, "Calibri"));
  EXPECT_FALSE(Listed(P::kMac, "Segoe UI"));
  EXPECT_FALSE(Listed(P::kWindows, "Helvetica Neue"));
  EXPECT_FALSE(Listed(P::kLinux, "Verdana"));
}

TEST(MoltFontListTest, AliasesAreRequestNamesOnly) {
  // Linux resolves these through fontconfig's metric aliases, or to the real
  // fonts on machines that have them: protected contexts always ask for the
  // Liberation family.
  EXPECT_EQ(AliasOf(FontPlatform::kLinux, "Arial"), "Liberation Sans");
  EXPECT_EQ(AliasOf(FontPlatform::kLinux, "helvetica"), "Liberation Sans");
  EXPECT_EQ(AliasOf(FontPlatform::kLinux, "Times New Roman"),
            "Liberation Serif");
  EXPECT_EQ(AliasOf(FontPlatform::kLinux, "Courier"), "Liberation Mono");
  EXPECT_FALSE(Listed(FontPlatform::kLinux, "Arial"));
  // Windows: Blink's own alternates.
  EXPECT_EQ(AliasOf(FontPlatform::kWindows, "Helvetica"), "Arial");
  EXPECT_EQ(AliasOf(FontPlatform::kWindows, "Times"), "Times New Roman");
  EXPECT_FALSE(Listed(FontPlatform::kWindows, "Helvetica"));
  // macOS installs all of them.
  EXPECT_EQ(AliasOf(FontPlatform::kMac, "Helvetica"), "");
}

TEST(MoltFontVisibilityTest, AllSeesEverything) {
  const FontVisibility all = FontVisibility::All();
  EXPECT_FALSE(all.restricts());
  EXPECT_EQ(all.ForFamilyRequest(Utf8("Fira Code")).kind,
            FontVisibility::Request::Kind::kUnrestricted);
  EXPECT_TRUE(all.IsFamilyVisible(Utf8("Fira Code")));
  EXPECT_TRUE(all.IsFamilyVisible(Utf8("")));
  EXPECT_TRUE(all.IsFamilyVisible(Utf8("x").Hash(), false));
  EXPECT_FALSE(all.IsSampled(Utf8("x").Hash()));
}

TEST(MoltFontVisibilityTest, AllowListOnly) {
  using Kind = FontVisibility::Request::Kind;
  for (FontPlatform platform : kPlatforms) {
    const FontVisibility uniform = FontVisibility::AllowList(platform);
    EXPECT_TRUE(uniform.restricts());
    EXPECT_EQ(uniform.platform(), platform);
    for (const FontListEntry& entry : FontListFor(platform)) {
      const FontVisibility::Request request =
          uniform.ForFamilyRequest(Utf8(entry.name));
      if (entry.alias_of.empty()) {
        EXPECT_EQ(request.kind, Kind::kLookUp) << entry.name;
        EXPECT_TRUE(uniform.IsFamilyVisible(Utf8(entry.name))) << entry.name;
      } else {
        EXPECT_EQ(request.kind, Kind::kLookUpInstead) << entry.name;
        EXPECT_EQ(request.instead, entry.alias_of);
        // The alias is not a family: a machine whose lookup reaches a real
        // font of that name doesn't get it.
        EXPECT_FALSE(uniform.IsFamilyVisible(Utf8(entry.name))) << entry.name;
      }
    }
    // Nothing outside the list, whatever the name looks like.
    for (std::string_view name :
         {"Fira Code", "fira code", "FiraCode-Regular", "Fira Code Bold",
          "Arial Narrow", "Segoe UI Variable Display", "x"}) {
      if (platform == FontPlatform::kMac && name == "Arial Narrow") {
        continue;  // Installed on every Mac.
      }
      EXPECT_EQ(uniform.ForFamilyRequest(Utf8(name)).kind, Kind::kBlocked)
          << name;
      EXPECT_FALSE(uniform.IsFamilyVisible(Utf8(name))) << name;
      EXPECT_FALSE(uniform.IsSampled(Utf8(name).Hash()));
    }
    // The platform UI font, however Blink spells it.
    EXPECT_EQ(uniform.ForFamilyRequest(Utf8("system-ui")).kind,
              Kind::kUnrestricted);
    EXPECT_EQ(uniform.ForFamilyRequest(Utf8("System-UI")).kind,
              Kind::kUnrestricted);
    EXPECT_FALSE(uniform.IsFamilyVisible(Utf8("")));
    EXPECT_FALSE(uniform.IsFamilyVisible(Utf8("x").Hash(), false));
    EXPECT_TRUE(uniform.IsFamilyVisible(Utf8("x").Hash(), true));
  }
}

std::vector<std::string> SyntheticNames(size_t count) {
  std::vector<std::string> names;
  names.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    names.push_back("Installed Font " + std::to_string(i));
  }
  return names;
}

TEST(MoltFontVisibilityTest, SampleRateStabilityAndUnlinkability) {
  const std::vector<std::string> names = SyntheticNames(40000);
  const Farbler site_a(KeyFromByte(1));
  const Farbler site_a_again(KeyFromByte(1));
  const Farbler site_b(KeyFromByte(2));
  const FontVisibility a =
      FontVisibility::AllowListAndSample(FontPlatform::kWindows, site_a);
  const FontVisibility a_again =
      FontVisibility::AllowListAndSample(FontPlatform::kWindows, site_a_again);
  const FontVisibility b =
      FontVisibility::AllowListAndSample(FontPlatform::kWindows, site_b);
  size_t in_a = 0;
  size_t in_b = 0;
  size_t in_both = 0;
  for (const std::string& name : names) {
    const FontName font_name = Utf8(name);
    const bool visible_a = a.IsFamilyVisible(font_name);
    // Stable for the same key, whatever the spelling.
    EXPECT_EQ(visible_a, a_again.IsFamilyVisible(font_name));
    EXPECT_EQ(visible_a, a.IsSampled(font_name.Hash()));
    EXPECT_EQ(visible_a, a.IsFamilyVisible(font_name.Hash(), false));
    const bool visible_b = b.IsFamilyVisible(font_name);
    in_a += visible_a;
    in_b += visible_b;
    in_both += visible_a && visible_b;
  }
  // 1 in 16 = 2500 of 40000; the bounds are about 6 standard deviations.
  const double n = static_cast<double>(names.size());
  EXPECT_NEAR(static_cast<double>(in_a) / n, 1.0 / kExtraFontOneIn, 0.008);
  EXPECT_NEAR(static_cast<double>(in_b) / n, 1.0 / kExtraFontOneIn, 0.008);
  // Two sites' samples are independent: overlap ~ 1/256.
  EXPECT_NEAR(static_cast<double>(in_both) / n,
              1.0 / (kExtraFontOneIn * kExtraFontOneIn), 0.002);

  // Case and whitespace don't change the decision.
  for (size_t i = 0; i < 200; ++i) {
    std::string upper = names[i];
    for (char& c : upper) {
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
    }
    EXPECT_EQ(a.IsFamilyVisible(Utf8(names[i])),
              a.IsFamilyVisible(Utf8(" " + upper + "\t")));
  }
}

TEST(MoltFontVisibilityTest, SampledRequestsAndAliases) {
  using Kind = FontVisibility::Request::Kind;
  const std::vector<std::string> names = SyntheticNames(2000);
  for (uint8_t key_byte = 1; key_byte < 9; ++key_byte) {
    const FontVisibility farble = FontVisibility::AllowListAndSample(
        FontPlatform::kLinux, Farbler(KeyFromByte(key_byte)));
    // Listed families are always visible.
    for (const FontListEntry& entry : FontListFor(FontPlatform::kLinux)) {
      if (entry.alias_of.empty()) {
        EXPECT_EQ(farble.ForFamilyRequest(Utf8(entry.name)).kind,
                  Kind::kLookUp);
        EXPECT_TRUE(farble.IsFamilyVisible(Utf8(entry.name)));
      }
    }
    // An unlisted name is looked up iff it is in the sample, and then its
    // resolved family (here: the same name) is visible too.
    for (const std::string& name : names) {
      const FontName font_name = Utf8(name);
      const bool sampled = farble.IsSampled(font_name.Hash());
      EXPECT_EQ(farble.ForFamilyRequest(font_name).kind,
                sampled ? Kind::kLookUp : Kind::kBlocked);
      EXPECT_EQ(farble.IsFamilyVisible(font_name), sampled);
    }
    // An alias reaches the real font only when its name is sampled;
    // otherwise the listed family is looked up instead.
    const FontName arial = Utf8("Arial");
    const FontVisibility::Request request = farble.ForFamilyRequest(arial);
    if (farble.IsSampled(arial.Hash())) {
      EXPECT_EQ(request.kind, Kind::kLookUp);
      EXPECT_TRUE(farble.IsFamilyVisible(arial));
    } else {
      EXPECT_EQ(request.kind, Kind::kLookUpInstead);
      EXPECT_EQ(request.instead, "Liberation Sans");
      EXPECT_FALSE(farble.IsFamilyVisible(arial));
    }
  }
}

TEST(MoltFontVisibilityTest, UniformIgnoresTheKey) {
  // kAllowList has no key: two MoltNet users (or New Identities) see the
  // same thing for every name.
  const FontVisibility one = FontVisibility::AllowList(FontPlatform::kMac);
  const FontVisibility two = FontVisibility::AllowList(FontPlatform::kMac);
  for (const std::string& name : SyntheticNames(500)) {
    EXPECT_EQ(one.ForFamilyRequest(Utf8(name)).kind,
              two.ForFamilyRequest(Utf8(name)).kind);
    EXPECT_FALSE(one.IsFamilyVisible(Utf8(name)));
  }
}

TEST(MoltFontVisibilityTest, CurrentPlatform) {
  const FontPlatform platform = CurrentFontPlatform();
  EXPECT_EQ(FontVisibility::All().platform(), platform);
  EXPECT_FALSE(FontListFor(platform).empty());
}

}  // namespace
}  // namespace blink::molt_fingerprinting
