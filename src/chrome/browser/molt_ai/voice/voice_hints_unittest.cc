// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site hints (voice_hints.h): the order a site's parts are found in (a
// built-in skill, a learned hint, the generic map), what a hint keeps, and
// hints learned, dropped, capped and aged.

#include "chrome/browser/molt_ai/voice/voice_hints.h"

#include <string>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_test_trees.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using test::TreeBuilder;

constexpr uint32_t kField = kStateFocusable | kStateEditable;
constexpr int64_t kNow = 1'800'000'000;

// A shop: a site-wide search box first, the catalogue's own below it, two
// lists of products and two players.
PageTree ShopPage() {
  TreeBuilder b("Shop");
  const uint32_t root =
      b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea, "Shop", 0);
  const uint32_t banner = b.Add(root, PageRole::kBanner, "", 0);
  b.Add(banner, PageRole::kSearchBox, "Search the site", kField);
  const uint32_t main = b.Add(root, PageRole::kMain, "", 0);
  b.Add(main, PageRole::kSearchBox, "Search products", kField);
  for (const char* label : {"Deals", "Products"}) {
    const uint32_t list = b.Add(main, PageRole::kList, label, 0);
    for (int i = 0; i < 4; ++i) {
      const uint32_t item = b.Add(list, PageRole::kListItem, "", 0);
      b.Link(item, std::string(label) + " item " + std::to_string(i + 1),
             "/item" + std::to_string(i));
    }
  }
  b.Add(main, PageRole::kVideo, "Advert", kStateFocusable);
  b.Add(main, PageRole::kVideo, "Product video", kStateFocusable);
  return b.Take();
}

const std::string& Label(const LocatedPart& found) {
  static const std::string none;
  if (found.field) {
    return found.field->label;
  }
  if (found.list) {
    return found.list->label;
  }
  if (found.media) {
    return found.media->label;
  }
  return none;
}

TEST(VoiceHintsTest, SkillThenHintThenGeneric) {
  const PageMap map = BuildPageMap(ShopPage());
  // The generic map: the first search field on screen, the most prominent
  // list, the first player.
  LocatedPart found = LocateSitePart(SitePart::kSearchField, map, nullptr,
                                     nullptr);
  EXPECT_EQ(found.source, PartSource::kGeneric);
  EXPECT_EQ(Label(found), "Search the site");
  const LocatedPart generic_list =
      LocateSitePart(SitePart::kResultList, map, nullptr, nullptr);
  ASSERT_TRUE(generic_list.list);
  found = LocateSitePart(SitePart::kPlayer, map, nullptr, nullptr);
  EXPECT_EQ(Label(found), "Advert");

  // A hint: what worked here before, by its name.
  SiteHint hint;
  hint.host = "shop.example";
  hint.search_field = "Search products";
  hint.result_list = generic_list.list->label == "Deals" ? "Products" : "Deals";
  hint.player = "product video";  // names compare folded
  found = LocateSitePart(SitePart::kSearchField, map, nullptr, &hint);
  EXPECT_EQ(found.source, PartSource::kHint);
  EXPECT_EQ(Label(found), "Search products");
  found = LocateSitePart(SitePart::kResultList, map, nullptr, &hint);
  EXPECT_EQ(found.source, PartSource::kHint);
  EXPECT_EQ(Label(found), hint.result_list);
  found = LocateSitePart(SitePart::kPlayer, map, nullptr, &hint);
  EXPECT_EQ(Label(found), "Product video");

  // A built-in skill comes first.
  LocatedPart skill;
  skill.field = &map.items[0];
  found = LocateSitePart(SitePart::kSearchField, map, &skill, &hint);
  EXPECT_EQ(found.source, PartSource::kSkill);
  EXPECT_EQ(found.field, &map.items[0]);
  // A skill that found another kind of part doesn't count for this one.
  found = LocateSitePart(SitePart::kPlayer, map, &skill, &hint);
  EXPECT_EQ(found.source, PartSource::kHint);

  // A hint naming what the page doesn't have: the generic map.
  hint.search_field = "Search the archive";
  found = LocateSitePart(SitePart::kSearchField, map, nullptr, &hint);
  EXPECT_EQ(found.source, PartSource::kGeneric);
  EXPECT_EQ(Label(found), "Search the site");

  // Nothing at all.
  const PageMap empty;
  EXPECT_FALSE(
      LocateSitePart(SitePart::kResultList, empty, nullptr, &hint).found());
  EXPECT_EQ(LocateSitePart(SitePart::kPlayer, empty, nullptr, nullptr).source,
            PartSource::kNone);
}

TEST(VoiceHintsTest, HintsAreNamesOnly) {
  const PageMap map = BuildPageMap(ShopPage());
  LocatedPart found =
      LocateSitePart(SitePart::kSearchField, map, nullptr, nullptr);
  EXPECT_EQ(HintName(SitePart::kSearchField, found), "Search the site");
  // Page text is cleaned and capped as it is kept.
  PageItem item;
  item.label = "Search\xE2\x80\xAE " + std::string(200, 'x');
  found.field = &item;
  const std::string name = HintName(SitePart::kSearchField, found);
  EXPECT_LE(name.size(), kMaxHintNameChars + 4);
  EXPECT_EQ(name.find("\xE2\x80\xAE"), std::string::npos);
  EXPECT_EQ(HintName(SitePart::kPlayer, LocatedPart()), "");
}

TEST(VoiceHintsTest, LearnedDroppedCappedAndAged) {
  SiteHints hints;
  hints.Worked("WWW.YouTube.com", SitePart::kSearchField, "Search", kNow);
  hints.Worked("www.youtube.com", SitePart::kPlayer, "Video player", kNow + 1);
  const SiteHint* hint = hints.Find("www.youtube.com");
  ASSERT_TRUE(hint);
  EXPECT_EQ(hint->search_field, "Search");
  EXPECT_EQ(hint->player, "Video player");
  EXPECT_EQ(hint->last_used, kNow + 1);
  // A newer name replaces the old one.
  hints.Worked("www.youtube.com", SitePart::kSearchField, "Search videos",
               kNow + 2);
  EXPECT_EQ(hints.Find("www.youtube.com")->search_field, "Search videos");
  // Not a host, or no name: nothing.
  hints.Worked("evil host/", SitePart::kPlayer, "x", kNow);
  hints.Worked("a..b", SitePart::kPlayer, "x", kNow);
  hints.Worked("shop.example", SitePart::kPlayer, "", kNow);
  EXPECT_EQ(hints.hints().size(), 1u);
  // A hint that failed is dropped; the site goes with its last one.
  hints.Failed("www.youtube.com", SitePart::kSearchField);
  hint = hints.Find("www.youtube.com");
  ASSERT_TRUE(hint);
  EXPECT_TRUE(hint->search_field.empty());
  hints.Failed("www.youtube.com", SitePart::kPlayer);
  EXPECT_FALSE(hints.Find("www.youtube.com"));
  // Capped: the least recently used site goes.
  for (size_t i = 0; i < kMaxHintedSites + 5; ++i) {
    hints.Worked("site" + std::to_string(i) + ".example",
                 SitePart::kResultList, "Results",
                 kNow + static_cast<int64_t>(i));
  }
  EXPECT_EQ(hints.hints().size(), kMaxHintedSites);
  EXPECT_FALSE(hints.Find("site0.example"));
  EXPECT_TRUE(hints.Find("site5.example"));
  EXPECT_TRUE(hints.Forget("site5.example"));
  EXPECT_FALSE(hints.Forget("site5.example"));
  // Aged by last use.
  hints.Age(kNow + 100 + kHintLifetimeSeconds);
  for (const SiteHint& h : hints.hints()) {
    EXPECT_GE(h.last_used, kNow + 100);
  }
  hints.Clear();
  EXPECT_TRUE(hints.empty());
}

TEST(VoiceHintsTest, RestoresOnlyValidHints) {
  SiteHints hints;
  SiteHint hint;
  hint.host = "www.youtube.com";
  hint.search_field = "Search";
  hint.last_used = kNow;
  EXPECT_TRUE(hints.Restore(hint));
  EXPECT_FALSE(hints.Restore(hint));  // twice
  SiteHint bad = hint;
  bad.host = "WWW.EXAMPLE.COM";  // not as kept
  EXPECT_FALSE(hints.Restore(bad));
  bad.host = "example.com";
  bad.search_field = " Search ";
  EXPECT_FALSE(hints.Restore(bad));
  bad.search_field = std::string(100, 'x');
  EXPECT_FALSE(hints.Restore(bad));
  bad.search_field.clear();  // nothing hinted
  EXPECT_FALSE(hints.Restore(bad));
  bad.player = "Player";
  bad.last_used = -5;
  EXPECT_FALSE(hints.Restore(bad));
  bad.last_used = kNow;
  EXPECT_TRUE(hints.Restore(bad));
  EXPECT_EQ(HintHost("Sub.Example.COM"), "sub.example.com");
  EXPECT_EQ(HintHost("exa_mple.com"), "");
  EXPECT_EQ(HintHost(".example.com"), "");
  EXPECT_EQ(HintHost(std::string(300, 'a')), "");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
