// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_fonts.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/file_path_conversion.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.h"
#include "third_party/blink/renderer/platform/font_family_names.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"
#include "third_party/blink/renderer/platform/fonts/font_platform_data.h"
#include "third_party/blink/renderer/platform/fonts/simple_font_data.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkTypeface_fontations.h"

namespace blink::molt_fingerprinting {
namespace {

using moltnet::BundledFontFace;
using moltnet::BundledFontFamily;
using moltnet::BundledFontFile;
using moltnet::BundledFontName;
using moltnet::BundledFontTable;
using moltnet::ScriptFallback;

constexpr char kFontFile[] = "roboto-a.ttf";

base::FilePath TestFontPath() {
  return WebStringToFilePath(test::PlatformTestDataPath(kFontFile));
}

base::File OpenTestFont() {
  return base::File(TestFontPath(),
                    base::File::FLAG_OPEN | base::File::FLAG_READ);
}

// A one-family set made of Blink's small Roboto test font: whatever names
// the font has, an alias ("Arial") and every generic family map to it.
class TestSet {
 public:
  TestSet() {
    base::File file = OpenTestFont();
    size_ = static_cast<uint64_t>(file.GetLength());
    std::vector<uint8_t> bytes(size_);
    CHECK(file.Read(0, bytes).has_value());
    sk_sp<SkTypeface> typeface = SkTypeface_Make_Fontations(
        SkData::MakeWithCopy(bytes.data(), bytes.size()), SkFontArguments());
    SkString name;
    typeface->getFamilyName(&name);
    family_ = name.c_str();
    SkString postscript;
    typeface->getPostScriptName(&postscript);
    postscript_ = postscript.c_str();

    files_ = {{BundledFontFile{kFontFile, size_}}};
    faces_ = {{BundledFontFace{0, 0, 0, false, false, 400, 400, 400,
                               postscript_, postscript_, "Regular"}}};
    families_ = {
        {BundledFontFamily{family_, FontName::Utf8(family_).Hash(), 0, 1}}};
    names_ = {
        {BundledFontName{"Arial", FontName::Utf8("Arial").Hash(), 0, true}}};
    fallback_ = {{ScriptFallback{0, 0, 1},
                  ScriptFallback{moltnet::kFallbackTextEmoji, 0, 1},
                  ScriptFallback{moltnet::kFallbackEmoji, 0, 1}}};
    visibility_ = {{FontListEntry{family_}, FontListEntry{"Arial", family_}}};
    table_.version = "test";
    table_.files = files_;
    table_.faces = faces_;
    table_.families = families_;
    table_.names = names_;
    table_.fallback = fallback_;
    table_.fallback_families = fallback_families_;
    table_.visibility_list = visibility_;
  }

  const BundledFontTable& table() const { return table_; }
  const std::string& family() const { return family_; }
  uint64_t size() const { return size_; }

 private:
  uint64_t size_ = 0;
  std::string family_;
  std::string postscript_;
  std::array<BundledFontFile, 1> files_;
  std::array<BundledFontFace, 1> faces_;
  std::array<BundledFontFamily, 1> families_;
  std::array<BundledFontName, 1> names_;
  std::array<ScriptFallback, 3> fallback_;
  std::array<uint8_t, 1> fallback_families_ = {0};
  std::array<FontListEntry, 2> visibility_;
  BundledFontTable table_;
};

class MoltNetFontsTest : public testing::Test {
 protected:
  void SetUp() override {
    UseBundledFontTableForTesting(set_.table());
    FontCache::Get().Invalidate();
    description_.SetComputedSize(16);
    description_.SetSpecifiedSize(16);
  }
  void TearDown() override {
    ResetBundledFontsForTesting();
    FontCache::Get().Invalidate();
  }

  std::vector<base::File> Files() {
    std::vector<base::File> files;
    files.push_back(OpenTestFont());
    return files;
  }

  std::string FamilyOf(const SimpleFontData* font_data) {
    SkString name;
    font_data->PlatformData().Typeface()->getFamilyName(&name);
    return name.c_str();
  }

  test::TaskEnvironment task_environment_;
  TestSet set_;
  FontDescription description_;
};

TEST_F(MoltNetFontsTest, InactiveUntilExpected) {
  EXPECT_FALSE(BundledFontsActive());
  EXPECT_EQ(FontPlatformData::GetUniformFontSet(), nullptr);
  EXPECT_FALSE(SetBundledFontFiles(Files()));
  ExpectBundledFonts();
  EXPECT_TRUE(BundledFontsActive());
  EXPECT_NE(FontPlatformData::GetUniformFontSet(), nullptr);
}

TEST_F(MoltNetFontsTest, NothingFromTheSystemBeforeTheFiles) {
  ExpectBundledFonts();
  EXPECT_EQ(FontCache::Get().GetFontData(description_,
                                         AtomicString(set_.family().c_str())),
            nullptr);
  // Whatever the machine has installed, nothing is found.
  for (const char* name : {"Arial", "DejaVu Sans", "Ahem", "sans-serif"}) {
    EXPECT_EQ(FontCache::Get().GetFontData(description_, AtomicString(name)),
              nullptr)
        << name;
  }
  // The last resort is a font without glyphs.
  const SimpleFontData* last_resort =
      FontCache::Get().GetLastResortFallbackFont(description_);
  ASSERT_TRUE(last_resort);
  EXPECT_EQ(last_resort->PlatformData().Typeface()->countGlyphs(), 0);
  // Then the files come, and the set's fonts are found.
  ASSERT_TRUE(SetBundledFontFiles(Files()));
  const SimpleFontData* font_data = FontCache::Get().GetFontData(
      description_, AtomicString(set_.family().c_str()));
  ASSERT_TRUE(font_data);
  EXPECT_EQ(FamilyOf(font_data), set_.family());
}

TEST_F(MoltNetFontsTest, RefusesFilesThatDontMatchTheTable) {
  ExpectBundledFonts();
  EXPECT_FALSE(SetBundledFontFiles({}));
  std::vector<base::File> two = Files();
  two.push_back(OpenTestFont());
  EXPECT_FALSE(SetBundledFontFiles(std::move(two)));
  std::vector<base::File> invalid;
  invalid.emplace_back();
  EXPECT_FALSE(SetBundledFontFiles(std::move(invalid)));
  // Another font, of another size.
  std::vector<base::File> other;
  other.emplace_back(
      WebStringToFilePath(test::PlatformTestDataPath("colrv1_test.ttf")),
      base::File::FLAG_OPEN | base::File::FLAG_READ);
  EXPECT_FALSE(SetBundledFontFiles(std::move(other)));
  EXPECT_TRUE(SetBundledFontFiles(Files()));
  // Only once.
  EXPECT_FALSE(SetBundledFontFiles(Files()));
}

TEST_F(MoltNetFontsTest, EveryLookupEndsInTheSet) {
  ExpectBundledFonts();
  ASSERT_TRUE(SetBundledFontFiles(Files()));
  FontCache& cache = FontCache::Get();
  // An alias and the generic keywords.
  for (const char* name : {"Arial", "sans-serif", "serif", "monospace",
                           "system-ui", "BlinkMacSystemFont"}) {
    const SimpleFontData* font_data =
        cache.GetFontData(description_, AtomicString(name));
    ASSERT_TRUE(font_data) << name;
    EXPECT_EQ(FamilyOf(font_data), set_.family()) << name;
  }
  // Installed system fonts are not.
  for (const char* name : {"DejaVu Sans", "Ahem", "Times New Roman"}) {
    EXPECT_EQ(cache.GetFontData(description_, AtomicString(name)), nullptr)
        << name;
  }
  // Generic families map to the set (what FamilyNameFromSettings returns
  // instead of the settings' family), system-ui is looked up by name.
  const FontPlatformData::UniformFontSet* set =
      FontPlatformData::GetUniformFontSet();
  ASSERT_TRUE(set);
  for (const AtomicString& generic :
       {font_family_names::kSansSerif, font_family_names::kSerif,
        font_family_names::kMonospace, font_family_names::kMath}) {
    EXPECT_EQ(set->GenericFamily(description_, generic),
              AtomicString(set_.family().c_str()));
  }
  EXPECT_EQ(set->GenericFamily(description_, font_family_names::kSystemUi),
            g_empty_atom);
  // Fallback stays in the set: the test font has 'a' only.
  const SimpleFontData* fallback =
      cache.FallbackFontForCharacter(description_, 'a', nullptr);
  ASSERT_TRUE(fallback);
  EXPECT_EQ(FamilyOf(fallback), set_.family());
  EXPECT_EQ(cache.FallbackFontForCharacter(description_, 0x4E00, nullptr),
            nullptr);
  // The last resort too.
  const SimpleFontData* last_resort =
      cache.GetLastResortFallbackFont(description_);
  ASSERT_TRUE(last_resort);
  EXPECT_EQ(FamilyOf(last_resort), set_.family());
}

TEST_F(MoltNetFontsTest, RendersAlikeOnEveryOs) {
  ExpectBundledFonts();
  ASSERT_TRUE(SetBundledFontFiles(Files()));
  const SimpleFontData* font_data = FontCache::Get().GetFontData(
      description_, AtomicString(set_.family().c_str()));
  ASSERT_TRUE(font_data);
  const SkFont font = font_data->PlatformData().CreateSkFont(&description_);
  EXPECT_EQ(font.getEdging(), SkFont::Edging::kAntiAlias);
  EXPECT_EQ(font.getHinting(), SkFontHinting::kNone);
  EXPECT_TRUE(font.isSubpixel());
  EXPECT_TRUE(font.isLinearMetrics());
  EXPECT_FALSE(font.isEmbeddedBitmaps());
}

TEST_F(MoltNetFontsTest, LocalFontAccessListsTheSet) {
  const std::vector<BundledFontFaceInfo> faces = BundledFontFaces();
  ASSERT_EQ(faces.size(), 1u);
  EXPECT_EQ(faces[0].family, set_.family());
  ResetBundledFontsForTesting();
  EXPECT_EQ(BundledFontFaces().size(),
            moltnet::GetBundledFontTable().faces.size());
}

}  // namespace
}  // namespace blink::molt_fingerprinting
