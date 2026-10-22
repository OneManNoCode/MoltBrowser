// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_locale_prefs.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/tor/molt_net_locale_prefs_resource_ids.h"
#include "chrome/browser/molt_ai/tor/molt_net_locale_prefs_table.h"
#include "chrome/common/pref_names.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"
#include "ui/base/resource/data_pack.h"
#include "ui/base/resource/resource_handle.h"
#include "ui/base/resource/resource_scale_factor.h"

namespace molt_ai::tor {
namespace {

namespace table = locale_prefs_table;
using blink::molt_fingerprinting::FindInFontList;
using blink::molt_fingerprinting::FontListEntry;
using blink::molt_fingerprinting::FontName;
using blink::molt_fingerprinting::FontPlatform;
using blink::molt_fingerprinting::IsAllowListedFamily;

struct PlatformTable {
  LocalePrefsPlatform platform;
  FontPlatform font_platform;
  std::vector<MoltNetFontDefault> fonts;
  int minimum_logical_font_size;
};

std::vector<PlatformTable> Tables() {
  return {
      {LocalePrefsPlatform::kWindows, FontPlatform::kWindows,
       {table::kWindowsFontDefaults.begin(), table::kWindowsFontDefaults.end()},
       table::kWindowsMinimumLogicalFontSize},
      {LocalePrefsPlatform::kMac, FontPlatform::kMac,
       {table::kMacFontDefaults.begin(), table::kMacFontDefaults.end()},
       table::kMacMinimumLogicalFontSize},
      {LocalePrefsPlatform::kLinux, FontPlatform::kLinux,
       {table::kLinuxFontDefaults.begin(), table::kLinuxFontDefaults.end()},
       table::kLinuxMinimumLogicalFontSize},
  };
}

std::string StringValue(std::string_view pref, LocalePrefsPlatform platform) {
  const std::optional<LocaleIndependentDefault> value =
      MoltNetLocaleIndependentDefault(pref, platform);
  CHECK(value);
  CHECK(value->type == LocaleIndependentDefault::Type::kString);
  return std::string(value->string_value);
}

// The family a default install of `platform` gives a default of `en_us`
// (Chromium's ",A,B,C" list form or one family): the first family on the
// allow-list, an alias replaced by its family; "" if none is listed.
std::string ResolveOnDefaultInstall(std::string_view en_us,
                                    FontPlatform platform) {
  std::vector<std::string_view> families;
  if (en_us.starts_with(',')) {
    std::string_view rest = en_us.substr(1);
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      families.push_back(rest.substr(0, comma));
      rest = comma == std::string_view::npos ? std::string_view()
                                             : rest.substr(comma + 1);
    }
  } else {
    families.push_back(en_us);
  }
  for (std::string_view family : families) {
    const FontName name = FontName::Utf8(family);
    if (const FontListEntry* entry =
            FindInFontList(platform, name, name.Hash())) {
      return std::string(entry->alias_of.empty() ? family : entry->alias_of);
    }
  }
  return std::string();
}

// The pref names in the table and in the code are Chromium's.
TEST(MoltNetLocalePrefsTest, PrefNamesMatchChromiumsConstants) {
  EXPECT_EQ(StringValue(prefs::kDefaultCharset, LocalePrefsPlatform::kLinux),
            "windows-1252");
  EXPECT_TRUE(MoltNetLocaleIndependentDefault(
      prefs::kWebKitMinimumLogicalFontSize, LocalePrefsPlatform::kMac));
  std::set<std::string_view> names;
  for (const PlatformTable& t : Tables()) {
    for (const MoltNetFontDefault& row : t.fonts) {
      names.insert(row.pref_name);
    }
  }
  for (const char* name : {
           prefs::kWebKitStandardFontFamily,
           prefs::kWebKitFixedFontFamily,
           prefs::kWebKitSerifFontFamily,
           prefs::kWebKitSansSerifFontFamily,
           prefs::kWebKitCursiveFontFamily,
           prefs::kWebKitFantasyFontFamily,
           prefs::kWebKitMathFontFamily,
           prefs::kWebKitStandardFontFamilyJapanese,
           prefs::kWebKitFixedFontFamilyJapanese,
           prefs::kWebKitStandardFontFamilyKorean,
           prefs::kWebKitSansSerifFontFamilySimplifiedHan,
           prefs::kWebKitSerifFontFamilyTraditionalHan,
           prefs::kWebKitStandardFontFamilyDevanagari,
           prefs::kWebKitSansSerifFontFamilyArabic,
       }) {
    EXPECT_TRUE(names.contains(name)) << name;
  }
}

TEST(MoltNetLocalePrefsTest, FixedValues) {
  for (const PlatformTable& t : Tables()) {
    // The encoding is the same everywhere.
    EXPECT_EQ(StringValue("intl.charset_default", t.platform),
              "windows-1252");
    const std::optional<LocaleIndependentDefault> minimum =
        MoltNetLocaleIndependentDefault(
            "webkit.webprefs.minimum_logical_font_size", t.platform);
    ASSERT_TRUE(minimum);
    EXPECT_EQ(minimum->type, LocaleIndependentDefault::Type::kInteger);
    EXPECT_EQ(minimum->integer_value, t.minimum_logical_font_size);
    for (const MoltNetFontDefault& row : t.fonts) {
      EXPECT_EQ(StringValue(row.pref_name, t.platform), row.family);
    }
    // A font pref without a default (most scripts) gets none either.
    EXPECT_EQ(StringValue("webkit.webprefs.fonts.standard.Thai", t.platform),
              "");
    EXPECT_EQ(StringValue("webkit.webprefs.fonts.math.Hebr", t.platform),
              "");
  }
  EXPECT_EQ(
      StringValue("webkit.webprefs.fonts.standard.Zyyy",
                  LocalePrefsPlatform::kWindows),
      "Times New Roman");
  EXPECT_EQ(StringValue("webkit.webprefs.fonts.fixed.Zyyy",
                        LocalePrefsPlatform::kWindows),
            "Consolas");
  EXPECT_EQ(StringValue("webkit.webprefs.fonts.sansserif.Jpan",
                        LocalePrefsPlatform::kMac),
            "Hiragino Kaku Gothic ProN");
  EXPECT_EQ(StringValue("webkit.webprefs.fonts.sansserif.Zyyy",
                        LocalePrefsPlatform::kLinux),
            "Liberation Sans");
  EXPECT_EQ(StringValue("webkit.webprefs.fonts.fixed.Zyyy",
                        LocalePrefsPlatform::kLinux),
            "DejaVu Sans Mono");
}

TEST(MoltNetLocalePrefsTest, OtherPrefsAndPlatformsKeepTheirDefaults) {
  for (std::string_view name : {
           "webkit.webprefs.default_font_size",
           "webkit.webprefs.minimum_font_size",
           "webkit.webprefs.javascript_enabled",
           "intl.accept_languages",
           // Not font family prefs.
           "webkit.webprefs.fonts.standard",
           "webkit.webprefs.fonts.standard.",
           "webkit.webprefs.fonts.standard.Zyy",
           "webkit.webprefs.fonts.standard.Zyyyy",
           "webkit.webprefs.fonts.standard.Zy1y",
           "webkit.webprefs.fonts.monospace.Zyyy",
           "webkit.webprefs.fonts..Zyyy",
           "webkit.webprefs.fonts.standard.Zyyy.x",
           "",
       }) {
    EXPECT_FALSE(
        MoltNetLocaleIndependentDefault(name, LocalePrefsPlatform::kWindows))
        << name;
  }
  // MoltNet doesn't run where there is no table; only the encoding is fixed.
  EXPECT_FALSE(MoltNetLocaleIndependentDefault(
      "webkit.webprefs.fonts.standard.Zyyy", LocalePrefsPlatform::kOther));
  EXPECT_FALSE(MoltNetLocaleIndependentDefault(
      "webkit.webprefs.minimum_logical_font_size",
      LocalePrefsPlatform::kOther));
  EXPECT_EQ(StringValue("intl.charset_default", LocalePrefsPlatform::kOther),
            "windows-1252");
}

TEST(MoltNetLocalePrefsTest, CurrentPlatform) {
#if BUILDFLAG(IS_WIN)
  EXPECT_EQ(CurrentLocalePrefsPlatform(), LocalePrefsPlatform::kWindows);
#elif BUILDFLAG(IS_MAC)
  EXPECT_EQ(CurrentLocalePrefsPlatform(), LocalePrefsPlatform::kMac);
#elif BUILDFLAG(IS_LINUX)
  EXPECT_EQ(CurrentLocalePrefsPlatform(), LocalePrefsPlatform::kLinux);
#else
  EXPECT_EQ(CurrentLocalePrefsPlatform(), LocalePrefsPlatform::kOther);
#endif
}

// Every family MoltNet names is one every supported version of the OS
// installs (so every MoltNet user on it renders the generic families
// alike), and a single family, never a list resolved per machine.
TEST(MoltNetLocalePrefsTest, FamiliesAreInstalledByDefault) {
  for (const PlatformTable& t : Tables()) {
    std::set<std::string_view> names;
    for (const MoltNetFontDefault& row : t.fonts) {
      EXPECT_TRUE(names.insert(row.pref_name).second) << row.pref_name;
      EXPECT_EQ(row.family.find(','), std::string_view::npos)
          << row.pref_name;
      if (row.family.empty()) {
        continue;
      }
      const FontName family = FontName::Utf8(row.family);
      EXPECT_TRUE(IsAllowListedFamily(t.font_platform, family, family.Hash()))
          << row.pref_name << ": " << row.family;
    }
    // The script-independent generics that pages use most have a family;
    // monospace text must stay monospace.
    for (std::string_view generic : {"standard", "serif", "sansserif",
                                     "fixed"}) {
      EXPECT_FALSE(StringValue("webkit.webprefs.fonts." + std::string(generic) +
                                   ".Zyyy",
                               t.platform)
                       .empty())
          << generic;
    }
  }
}

// The table is Chromium's en-US default resolved on a default install,
// except for the two documented choices.
TEST(MoltNetLocalePrefsTest, FollowsTheRule) {
  for (const PlatformTable& t : Tables()) {
    for (const MoltNetFontDefault& row : t.fonts) {
      if (row.pref_name == "webkit.webprefs.fonts.fixed.Zyyy" &&
          (t.platform == LocalePrefsPlatform::kWindows ||
           t.platform == LocalePrefsPlatform::kLinux)) {
        continue;  // Consolas, DejaVu Sans Mono: see the header.
      }
      EXPECT_EQ(row.family,
                ResolveOnDefaultInstall(row.en_us_default, t.font_platform))
          << row.pref_name << " (" << row.en_us_default << ")";
    }
  }
}

// The table's en-US column is what Chromium registers with an English UI on
// this platform: the paks grit builds from the .grd files. Fails after a
// rebase that changed a default: rerun
// tools/moltnet_locale/generate_tables.py --write.
TEST(MoltNetLocalePrefsTest, MatchesChromiumsEnUsResources) {
  base::FilePath gen;
  ASSERT_TRUE(base::PathService::Get(base::DIR_GEN_TEST_DATA_ROOT, &gen));
  const auto pak_string = [](const ui::DataPack& pack,
                             int id) -> std::optional<std::string> {
    if (pack.GetTextEncodingType() != ui::ResourceHandle::UTF8) {
      return std::nullopt;
    }
    const std::optional<std::string_view> data =
        pack.GetStringView(static_cast<uint16_t>(id));
    return data ? std::optional<std::string>(*data) : std::nullopt;
  };

  ui::DataPack components(ui::kScaleFactorNone);
  ASSERT_TRUE(components.LoadFromPath(
      gen.AppendASCII("components")
          .AppendASCII("strings")
          .AppendASCII("components_locale_settings_en-US.pak")));
  EXPECT_EQ(pak_string(components, table::kDefaultEncodingResource),
            std::string(table::kDefaultEncoding));

#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
  ui::DataPack platform(ui::kScaleFactorNone);
  ASSERT_TRUE(platform.LoadFromPath(
      gen.AppendASCII("chrome").AppendASCII(
          "platform_locale_settings_en-US.pak")));
  const LocalePrefsPlatform current = CurrentLocalePrefsPlatform();
  const PlatformTable* t = nullptr;
  const std::vector<PlatformTable> tables = Tables();
  for (const PlatformTable& candidate : tables) {
    if (candidate.platform == current) {
      t = &candidate;
    }
  }
  ASSERT_TRUE(t);

  const std::optional<std::string> minimum =
      pak_string(platform, table::kMinimumLogicalFontSizeResource);
  int minimum_value = 0;
  ASSERT_TRUE(minimum && base::StringToInt(*minimum, &minimum_value));
  EXPECT_EQ(minimum_value, t->minimum_logical_font_size);

  ASSERT_EQ(table::kFontResources.size(), t->fonts.size());
  for (const table::FontResource& resource : table::kFontResources) {
    const MoltNetFontDefault* row = nullptr;
    for (const MoltNetFontDefault& candidate : t->fonts) {
      if (candidate.pref_name == resource.pref_name) {
        row = &candidate;
      }
    }
    ASSERT_TRUE(row) << resource.pref_name;
    EXPECT_EQ(pak_string(platform, resource.resource_id),
              std::string(row->en_us_default))
        << resource.pref_name;
  }
#endif
}

}  // namespace
}  // namespace molt_ai::tor
