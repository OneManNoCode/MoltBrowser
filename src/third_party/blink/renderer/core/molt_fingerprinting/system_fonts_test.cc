// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/system_fonts.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/font_family_names.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"
#include "third_party/blink/renderer/platform/fonts/font_family.h"
#include "third_party/blink/renderer/platform/fonts/simple_font_data.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"

namespace blink::molt_fingerprinting {
namespace {

// Like molt_fingerprinting_test.cc, these tests never configure kUniform
// (it pins the process-wide time zone and locale); fonts_unittest.cc covers
// the allow-list-only visibility MoltNet uses.
class MoltSystemFontsTest : public testing::Test {
 protected:
  void SetUp() override { ResetForTesting(); }
  void TearDown() override { ResetForTesting(); }

  LocalDOMWindow& Window(const char* url) {
    holders_.push_back(DummyPageHolder::CreateAndCommitNavigation(KURL(url)));
    LocalDOMWindow& window = *holders_.back()->GetFrame().DomWindow();
    window.SetStorageKey(
        BlinkStorageKey::CreateFirstParty(SecurityOrigin::Create(KURL(url))));
    return window;
  }

  void Farble() {
    Configure(Mode::kFarble);
    std::array<uint8_t, kSessionSeedSize> seed;
    for (size_t i = 0; i < seed.size(); ++i) {
      seed[i] = static_cast<uint8_t>(7 + 13 * i);
    }
    SetSessionSeed(seed);
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

std::vector<std::string> Names(size_t count) {
  std::vector<std::string> names;
  for (size_t i = 0; i < count; ++i) {
    names.push_back("Probe Font " + std::to_string(i));
  }
  return names;
}

// A family name that is in neither the allow-list nor `visibility`'s sample.
AtomicString HiddenName(const FontVisibility& visibility) {
  for (const std::string& name : Names(1000)) {
    if (!visibility.IsFamilyVisible(FontName::Utf8(name))) {
      return AtomicString::FromUtf8(name);
    }
  }
  return AtomicString();
}

TEST_F(MoltSystemFontsTest, OffChangesNothing) {
  LocalDOMWindow& window = Window("https://a.example/");
  EXPECT_FALSE(SystemFontVisibility(&window).restricts());
  EXPECT_FALSE(SystemFontVisibility(nullptr).restricts());
  EXPECT_TRUE(AllowEnumeratedFont(&window, "Probe Font 1"));

  const FontDescription description;
  const FontFamily family(AtomicString("Probe Font 1"),
                          FontFamily::Type::kFamilyName);
  EXPECT_EQ(GetSystemFontData(&window, description, family),
            FontCache::Get().GetFontData(description, family.FamilyName()));
  EXPECT_FALSE(
      IsSystemFontFamilyAvailable(&window, description, family).has_value());
  EXPECT_TRUE(
      AllowLocalFontFace(&window, description, AtomicString("Probe Font 1")));
}

TEST_F(MoltSystemFontsTest, WebSitesSeeTheAllowListAndTheirSample) {
  Farble();
  LocalDOMWindow& a = Window("https://www.a.example/");
  const FontVisibility visibility = SystemFontVisibility(&a);
  EXPECT_EQ(visibility.access(), FontVisibility::Access::kAllowListAndSample);

  // The sample is the one the site's key gives.
  const std::optional<Farbler> farbler = FarblerFor(&a);
  ASSERT_TRUE(farbler.has_value());
  const FontVisibility expected =
      FontVisibility::AllowListAndSample(CurrentFontPlatform(), *farbler);
  size_t visible = 0;
  for (const std::string& name : Names(2000)) {
    const bool allowed = AllowEnumeratedFont(&a, name);
    EXPECT_EQ(allowed, expected.IsFamilyVisible(FontName::Utf8(name)));
    visible += allowed;
  }
  EXPECT_GT(visible, 2000u / kExtraFontOneIn / 2);
  EXPECT_LT(visible, 2000u / kExtraFontOneIn * 2);

  // Every listed family is visible.
  for (const FontListEntry& entry : FontListFor(CurrentFontPlatform())) {
    if (entry.alias_of.empty()) {
      EXPECT_TRUE(AllowEnumeratedFont(&a, entry.name)) << entry.name;
    }
  }

  // Same site: same sample. Another site: another sample.
  LocalDOMWindow& a2 = Window("https://a.example/other");
  LocalDOMWindow& b = Window("https://b.example/");
  size_t differ = 0;
  for (const std::string& name : Names(2000)) {
    EXPECT_EQ(AllowEnumeratedFont(&a, name), AllowEnumeratedFont(&a2, name));
    differ += AllowEnumeratedFont(&a, name) != AllowEnumeratedFont(&b, name);
  }
  EXPECT_GT(differ, 0u);
}

TEST_F(MoltSystemFontsTest, ShieldsOffSitesSeeEveryFont) {
  Farble();
  SetUnfarbledSites({"https://a.example"});
  LocalDOMWindow& a = Window("https://a.example/");
  EXPECT_FALSE(SystemFontVisibility(&a).restricts());
  EXPECT_TRUE(SystemFontVisibility(&Window("https://b.example/")).restricts());
}

TEST_F(MoltSystemFontsTest, RandomKeysGetNoSample) {
  Farble();
  // An opaque top-level site (e.g. a sandboxed top-level document or an SVG
  // image's document) has a random key per context; summing the samples of
  // many such contexts would reveal every installed font.
  LocalDOMWindow& opaque = Window("https://a.example/");
  opaque.SetStorageKey(BlinkStorageKey());
  EXPECT_EQ(SystemFontVisibility(&opaque).access(),
            FontVisibility::Access::kAllowList);
  for (const std::string& name : Names(500)) {
    EXPECT_FALSE(AllowEnumeratedFont(&opaque, name));
  }
}

TEST_F(MoltSystemFontsTest, NullContextsFailClosed) {
  Farble();
  EXPECT_EQ(SystemFontVisibility(nullptr).access(),
            FontVisibility::Access::kAllowList);
}

TEST_F(MoltSystemFontsTest, HiddenFamiliesAreNotLookedUp) {
  Farble();
  LocalDOMWindow& a = Window("https://a.example/");
  const AtomicString hidden = HiddenName(SystemFontVisibility(&a));
  ASSERT_FALSE(hidden.IsNull());
  const FontDescription description;
  const FontFamily family(hidden, FontFamily::Type::kFamilyName);
  EXPECT_EQ(GetSystemFontData(&a, description, family), nullptr);
  EXPECT_EQ(IsSystemFontFamilyAvailable(&a, description, family),
            std::optional<bool>(false));
  EXPECT_FALSE(AllowLocalFontFace(&a, description, hidden));
}

TEST_F(MoltSystemFontsTest, GenericFamiliesAreNotRestricted) {
  Farble();
  LocalDOMWindow& a = Window("https://a.example/");
  const FontDescription description;
  for (const AtomicString& name :
       {font_family_names::kSansSerif, font_family_names::kSerif,
        font_family_names::kMonospace, font_family_names::kSystemUi}) {
    const FontFamily generic(name, FontFamily::Type::kGenericFamily);
    EXPECT_EQ(GetSystemFontData(&a, description, generic),
              FontCache::Get().GetFontData(description, name));
    EXPECT_FALSE(
        IsSystemFontFamilyAvailable(&a, description, generic).has_value());
  }
}

TEST_F(MoltSystemFontsTest, FoundFontsMustBeVisible) {
  Farble();
  LocalDOMWindow& a = Window("https://a.example/");
  const FontVisibility visibility = SystemFontVisibility(&a);
  const FontDescription description;
  // Whatever the test machine has installed: a listed family that resolves
  // is returned, and anything returned has a visible family.
  for (const FontListEntry& entry : FontListFor(CurrentFontPlatform())) {
    const FontFamily family(AtomicString::FromUtf8(entry.name),
                            FontFamily::Type::kFamilyName);
    const SimpleFontData* font_data =
        GetSystemFontData(&a, description, family);
    if (font_data) {
      EXPECT_TRUE(AllowInstalledFont(&a, *font_data)) << entry.name;
    }
    EXPECT_EQ(IsSystemFontFamilyAvailable(&a, description, family),
              std::optional<bool>(font_data != nullptr));
  }
}

}  // namespace
}  // namespace blink::molt_fingerprinting
