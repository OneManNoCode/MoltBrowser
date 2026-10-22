// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/renderer/molt_ai/fingerprinting/moltnet_locale_strings.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/strings/grit/blink_strings.h"
#include "ui/base/resource/data_pack.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/resource/resource_handle.h"
#include "ui/base/resource/resource_scale_factor.h"

namespace molt_ai::fingerprinting {
namespace {

// The paks grit builds from blink_strings.grd and
// permission_element_strings.grd (//third_party/blink/public/strings).
base::FilePath StringsPak(std::string_view file) {
  base::FilePath gen;
  CHECK(base::PathService::Get(base::DIR_GEN_TEST_DATA_ROOT, &gen));
  return gen.AppendASCII("third_party")
      .AppendASCII("blink")
      .AppendASCII("public")
      .AppendASCII("strings")
      .AppendASCII(file);
}

// The string `id` of a UTF-8 locale pak (what grit writes), or nullopt.
std::optional<std::u16string> PakString(const ui::DataPack& pack, int id) {
  if (pack.GetTextEncodingType() != ui::ResourceHandle::UTF8) {
    return std::nullopt;
  }
  const std::optional<std::string_view> data =
      pack.GetStringView(static_cast<uint16_t>(id));
  if (!data) {
    return std::nullopt;
  }
  return base::UTF8ToUTF16(*data);
}

TEST(MoltNetLocaleStringsTest, TableIsWellFormed) {
  std::set<int> ids;
  for (const MoltNetLocaleString& string : MoltNetLocaleStrings()) {
    EXPECT_TRUE(ids.insert(string.id).second) << string.name;
    EXPECT_TRUE(string.name.starts_with("IDS_")) << string.name;
    EXPECT_FALSE(string.en_us.empty()) << string.name;
  }
  // validationMessage, the file input and <details> are in it.
  EXPECT_TRUE(ids.contains(IDS_FORM_VALIDATION_VALUE_MISSING));
  EXPECT_TRUE(ids.contains(IDS_FORM_FILE_NO_FILE_LABEL));
  EXPECT_TRUE(ids.contains(IDS_DETAILS_WITHOUT_SUMMARY_LABEL));
  // Media controls (closed shadow trees) keep the user's language.
  EXPECT_FALSE(ids.contains(IDS_MEDIA_TRACKS_OFF));
}

// The generated table is what the en-US paks say, string for string. Fails
// after a Chromium rebase that changed one of them: rerun
// tools/moltnet_locale/generate_tables.py --write.
TEST(MoltNetLocaleStringsTest, MatchesTheEnUsPaks) {
  ui::DataPack blink(ui::kScaleFactorNone);
  ASSERT_TRUE(blink.LoadFromPath(StringsPak("blink_strings_en-US.pak")));
  ui::DataPack permission(ui::kScaleFactorNone);
  ASSERT_TRUE(permission.LoadFromPath(
      StringsPak("permission_element_strings_en-US.pak")));
  for (const MoltNetLocaleString& string : MoltNetLocaleStrings()) {
    std::optional<std::u16string> text = PakString(blink, string.id);
    if (!text) {
      text = PakString(permission, string.id);
    }
    ASSERT_TRUE(text) << string.name << " is in neither pak";
    EXPECT_EQ(*text, string.en_us) << string.name;
  }
}

// In a renderer whose UI language is French, the pinned strings come out in
// English and the others stay French.
TEST(MoltNetLocaleStringsTest, OverridesTheResourceBundle) {
  ASSERT_FALSE(ui::ResourceBundle::HasSharedInstance());
  ui::ResourceBundle::InitSharedInstanceWithPakPath(
      StringsPak("blink_strings_fr.pak"));
  ui::ResourceBundle& bundle = ui::ResourceBundle::GetSharedInstance();
  const std::u16string french_missing =
      bundle.GetLocalizedString(IDS_FORM_VALIDATION_VALUE_MISSING);
  const std::u16string french_tracks_off =
      bundle.GetLocalizedString(IDS_MEDIA_TRACKS_OFF);
  ASSERT_NE(french_missing, u"Please fill out this field.");

  EXPECT_EQ(PinMoltNetLocaleStrings(), MoltNetLocaleStrings().size());

  for (const MoltNetLocaleString& string : MoltNetLocaleStrings()) {
    EXPECT_EQ(bundle.GetLocalizedString(string.id), string.en_us)
        << string.name;
  }
  EXPECT_EQ(bundle.GetLocalizedString(IDS_FORM_VALIDATION_VALUE_MISSING),
            u"Please fill out this field.");
  EXPECT_EQ(bundle.GetLocalizedString(IDS_MEDIA_TRACKS_OFF),
            french_tracks_off);
  ui::ResourceBundle::CleanupSharedInstance();
}

TEST(MoltNetLocaleStringsTest, NoResourceBundleNoCrash) {
  ASSERT_FALSE(ui::ResourceBundle::HasSharedInstance());
  EXPECT_EQ(PinMoltNetLocaleStrings(), 0u);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
