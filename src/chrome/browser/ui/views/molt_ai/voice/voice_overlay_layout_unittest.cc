// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_test_trees.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

const OverlayRect kViewport = {0, 0, test::kViewportWidth,
                               test::kViewportHeight};

BadgeStyle Style() {
  BadgeStyle style;
  style.text_height = 26;
  style.digit_width = 14;
  style.padding_x = 6;
  style.padding_y = 2;
  style.gap = 2;
  style.min_scale = 14.0f / 22.0f;
  return style;
}

// What every plan promises: badges apart, inside the viewport, at their
// items; every item that reaches into the viewport got a badge or was
// counted as crowded or over the cap.
void ExpectWellPlaced(const BadgePlan& plan,
                      const std::vector<BadgeRequest>& requests,
                      const OverlayRect& viewport,
                      const BadgeStyle& style) {
  size_t visible = 0;
  for (const BadgeRequest& request : requests) {
    if (request.item.Intersects(viewport)) {
      ++visible;
    }
  }
  EXPECT_EQ(plan.badges.size() + plan.crowded + plan.over_cap, visible);
  EXPECT_LE(plan.badges.size(), kMaxBadges);
  for (size_t i = 0; i < plan.badges.size(); ++i) {
    const BadgePlacement& badge = plan.badges[i];
    SCOPED_TRACE(badge.number);
    EXPECT_TRUE(viewport.Contains(badge.badge));
    EXPECT_TRUE(badge.badge.Outset(style.gap + 1)
                    .Intersects(badge.item.Intersect(viewport)));
    EXPECT_GE(badge.scale, style.min_scale - 1e-4f);
    EXPECT_LE(badge.scale, 1.0f);
    const OverlaySize size = BadgeSize(badge.number, style, badge.scale);
    EXPECT_FLOAT_EQ(badge.badge.width, size.width);
    EXPECT_FLOAT_EQ(badge.badge.height, size.height);
    for (size_t j = i + 1; j < plan.badges.size(); ++j) {
      EXPECT_FALSE(badge.badge.Outset(style.gap - 0.01f)
                       .Intersects(plan.badges[j].badge))
          << badge.number << " and " << plan.badges[j].number;
    }
  }
}

std::vector<BadgeRequest> RequestsFor(const PageTree& tree,
                                      const PageToView& to_view) {
  const PageMap map = BuildPageMap(tree);
  return VisibleItems(OverlayItemsFromMap(map), to_view, kViewport);
}

const BadgePlacement* BadgeFor(const BadgePlan& plan, int number) {
  for (const BadgePlacement& badge : plan.badges) {
    if (badge.number == number) {
      return &badge;
    }
  }
  return nullptr;
}

// A deterministic pseudo-random sequence.
class Lcg {
 public:
  explicit Lcg(uint32_t seed) : state_(seed) {}
  float Next(float max) {
    state_ = state_ * 1664525u + 1013904223u;
    return static_cast<float>(state_ >> 8) / static_cast<float>(1u << 24) *
           max;
  }

 private:
  uint32_t state_;
};

// --- Coordinates ------------------------------------------------------------

TEST(VoiceOverlayLayoutTest, PageRectsBecomeDips) {
  // An element at CSS (100, 50), 200 by 20, on a page zoomed to 200% on a
  // 1.5x screen: Blink reports physical pixels, CSS x zoom x scale.
  const PageRect page = {300, 150, 600, 60};
  const std::optional<OverlayRect> view =
      PageRectToView(page, {/*origin_x=*/10, /*origin_y=*/80, 1.5f});
  ASSERT_TRUE(view);
  EXPECT_FLOAT_EQ(view->x, 10 + 200);
  EXPECT_FLOAT_EQ(view->y, 80 + 100);
  EXPECT_FLOAT_EQ(view->width, 400);
  EXPECT_FLOAT_EQ(view->height, 40);

  // At 100% on a 1x screen, page pixels are DIPs.
  EXPECT_EQ(PageRectToView({5, 6, 7, 8}, {0, 0, 1}),
            (OverlayRect{5, 6, 7, 8}));
  // Scrolled out above the viewport: negative, still converted (the caller
  // clips).
  EXPECT_EQ(PageRectToView({0, -500, 10, 10}, {0, 0, 2}),
            (OverlayRect{0, -250, 5, 5}));
}

TEST(VoiceOverlayLayoutTest, BadRectsAndScalesAreRefused) {
  const PageToView ok = {0, 0, 1};
  EXPECT_FALSE(PageRectToView({0, 0, 0, 10}, ok));
  EXPECT_FALSE(PageRectToView({0, 0, 10, -1}, ok));
  EXPECT_FALSE(PageRectToView({kNaN, 0, 10, 10}, ok));
  EXPECT_FALSE(PageRectToView({0, kInf, 10, 10}, ok));
  EXPECT_FALSE(PageRectToView({0, 0, kInf, 10}, ok));
  EXPECT_FALSE(PageRectToView({0, 0, 10, kNaN}, ok));
  EXPECT_FALSE(PageRectToView({3e6f, 0, 10, 10}, ok));
  EXPECT_FALSE(PageRectToView({0, 0, 10, 3e7f}, ok));
  EXPECT_FALSE(PageRectToView({999999, 0, 10, 10}, ok));
  EXPECT_FALSE(PageRectToView({0, 0, 10, 10}, {kNaN, 0, 1}));
  EXPECT_FALSE(PageRectToView({0, 0, 10, 10}, {0, 0, 0}));
  EXPECT_FALSE(PageRectToView({0, 0, 10, 10}, {0, 0, -1}));
  EXPECT_FALSE(PageRectToView({0, 0, 10, 10}, {0, 0, 0.1f}));
  EXPECT_FALSE(PageRectToView({0, 0, 10, 10}, {0, 0, 9}));
  EXPECT_FALSE(PageRectToView({0, 0, 10, 10}, {0, 0, kNaN}));
  EXPECT_TRUE(PageRectToView({0, 0, 10, 10}, {0, 0, 0.25f}));
  EXPECT_TRUE(PageRectToView({0, 0, 10, 10}, {0, 0, 8}));
}

TEST(VoiceOverlayLayoutTest, VisibleItemsAreOnScreenAndInTheViewport) {
  std::vector<OverlayItem> items(4);
  for (int i = 0; i < 4; ++i) {
    items[i].number = i + 1;
    items[i].handle = {0, i + 1};
  }
  items[0].bounds = {20, 20, 100, 20};
  items[1].bounds = {20, 60, 100, 20};
  items[1].on_screen = false;           // scrolled out of its container
  items[2].bounds = {20, 2000, 100, 20};  // below the viewport
  items[3].bounds = {2500, 40, 100, 20};  // right of it
  const std::vector<BadgeRequest> requests =
      VisibleItems(items, {0, 0, 1}, kViewport);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].number, 1);
  EXPECT_EQ(requests[0].item, (OverlayRect{20, 20, 100, 20}));

  // At a scale factor of 2 the third and fourth reach into the viewport.
  EXPECT_EQ(VisibleItems(items, {0, 0, 2}, {0, 0, 1280, 1100}).size(), 3u);
}

// --- Following the page -----------------------------------------------------

std::vector<OverlayItem> Items(int count, float y_offset = 0) {
  std::vector<OverlayItem> items;
  for (int i = 0; i < count; ++i) {
    OverlayItem item;
    item.number = i + 1;
    item.handle = {0, 100 + i};
    item.bounds = {10, y_offset + 40.0f * i, 200, 30};
    items.push_back(item);
  }
  return items;
}

TEST(VoiceOverlayLayoutTest, ScrollingMovesTheNumbers) {
  std::vector<OverlayItem> shown = Items(5);
  std::vector<OverlayItem> fresh = Items(5, -120);
  fresh[0].on_screen = false;
  ASSERT_EQ(FollowPage(shown, fresh), FollowResult::kMoved);
  EXPECT_FLOAT_EQ(shown[2].bounds.y, -120 + 80);
  EXPECT_FALSE(shown[0].on_screen);
  EXPECT_TRUE(shown[4].on_screen);
  // The same again: nothing to draw again.
  EXPECT_EQ(FollowPage(shown, fresh), FollowResult::kSame);
  // A NaN from the page is a change once.
  fresh[1].bounds.x = std::numeric_limits<float>::quiet_NaN();
  EXPECT_EQ(FollowPage(shown, fresh), FollowResult::kMoved);
  EXPECT_EQ(FollowPage(shown, fresh), FollowResult::kSame);
}

TEST(VoiceOverlayLayoutTest, ItemsAddedAfterTheLastKeepTheNumbers) {
  std::vector<OverlayItem> shown = Items(3);
  ASSERT_EQ(FollowPage(shown, Items(8, 5)), FollowResult::kMoved);
  EXPECT_EQ(shown.size(), 3u);  // the new ones wait for a new map
  EXPECT_FLOAT_EQ(shown[0].bounds.y, 5);
}

TEST(VoiceOverlayLayoutTest, NumbersThatMovedAreStale) {
  const std::vector<OverlayItem> before = Items(4);

  // An item before the last went away.
  std::vector<OverlayItem> shown = before;
  std::vector<OverlayItem> fresh = Items(3, 50);
  EXPECT_EQ(FollowPage(shown, fresh), FollowResult::kStale);
  EXPECT_FLOAT_EQ(shown[1].bounds.y, before[1].bounds.y);  // untouched

  // One was inserted: every number after it names another node.
  fresh = Items(5);
  for (size_t i = 2; i < fresh.size(); ++i) {
    fresh[i].handle.node += 1000;
  }
  shown = before;
  EXPECT_EQ(FollowPage(shown, fresh), FollowResult::kStale);

  // Same count, but a node of another document (its frame navigated).
  fresh = before;
  fresh[3].handle.tree = 7;
  shown = before;
  EXPECT_EQ(FollowPage(shown, fresh), FollowResult::kStale);

  // Nothing to follow.
  shown = before;
  EXPECT_EQ(FollowPage(shown, {}), FollowResult::kStale);
  std::vector<OverlayItem> none;
  EXPECT_EQ(FollowPage(none, before), FollowResult::kSame);
}

TEST(VoiceOverlayLayoutTest, TheOverlayCopiesTheMapsItems) {
  const PageMap map = BuildPageMap(test::GoogleLikeResults());
  const std::vector<OverlayItem> items = OverlayItemsFromMap(map);
  ASSERT_EQ(items.size(), map.items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    EXPECT_EQ(items[i].number, map.items[i].number);
    EXPECT_EQ(items[i].handle, map.items[i].handle);
    EXPECT_EQ(items[i].on_screen, map.items[i].on_screen);
  }

  PageMap big;
  big.items.resize(kMaxOverlayItems + 10);
  for (size_t i = 0; i < big.items.size(); ++i) {
    big.items[i].number = static_cast<int>(i + 1);
  }
  EXPECT_EQ(OverlayItemsFromMap(big).size(), kMaxOverlayItems);
}

// --- Sizes ------------------------------------------------------------------

TEST(VoiceOverlayLayoutTest, SizesFollowTheUsersTextSettings) {
  // The usual interface fonts and the default page font: the defaults.
  for (float ui : {12.0f, 13.0f}) {
    const OverlaySizes sizes = ComputeOverlaySizes(ui, 16);
    EXPECT_FLOAT_EQ(sizes.scale, 1);
    EXPECT_FLOAT_EQ(sizes.badge_font, 22);
    EXPECT_FLOAT_EQ(sizes.badge_min_font, 14);
    EXPECT_FLOAT_EQ(sizes.caption_font, 26);
    EXPECT_FLOAT_EQ(sizes.ring_width, 6);
  }
  // Text made larger in the OS doubles everything...
  OverlaySizes sizes = ComputeOverlaySizes(26, 16);
  EXPECT_FLOAT_EQ(sizes.scale, 2);
  EXPECT_FLOAT_EQ(sizes.badge_font, 44);
  EXPECT_FLOAT_EQ(sizes.badge_min_font, 28);
  EXPECT_FLOAT_EQ(sizes.caption_font, 52);
  EXPECT_FLOAT_EQ(sizes.ring_width, 8);
  // ... as does a "Very large" (24 px) page font, by half.
  EXPECT_FLOAT_EQ(ComputeOverlaySizes(13, 24).scale, 1.5f);
  // Never smaller than the defaults, never over three times.
  EXPECT_FLOAT_EQ(ComputeOverlaySizes(9, 9).scale, 1);
  EXPECT_FLOAT_EQ(ComputeOverlaySizes(100, 16).scale, 3);
  EXPECT_FLOAT_EQ(ComputeOverlaySizes(kNaN, kInf).scale, 1);
  EXPECT_FLOAT_EQ(ComputeOverlaySizes(-5, 0).scale, 1);
}

TEST(VoiceOverlayLayoutTest, BadgesFitTheirNumber) {
  const BadgeStyle style = Style();
  const OverlaySize one = BadgeSize(7, style, 1);
  EXPECT_FLOAT_EQ(one.height, 30);
  EXPECT_FLOAT_EQ(one.width, 30);  // at least as wide as high
  const OverlaySize three = BadgeSize(123, style, 1);
  EXPECT_FLOAT_EQ(three.width, 3 * 14 + 12);
  EXPECT_FLOAT_EQ(three.height, 30);
  const OverlaySize half = BadgeSize(123, style, 0.5f);
  EXPECT_FLOAT_EQ(half.width, three.width / 2);
  EXPECT_FLOAT_EQ(half.height, 15);
  EXPECT_GT(BadgeSize(2000, style, 1).width, three.width);

  const BadgeScales scales = BadgeScalesFor(style);
  ASSERT_EQ(scales.count, 3u);
  EXPECT_FLOAT_EQ(scales.scale[0], 1);
  EXPECT_FLOAT_EQ(scales.scale[1], (1 + style.min_scale) / 2);
  EXPECT_FLOAT_EQ(scales.scale[2], style.min_scale);
  BadgeStyle no_shrinking = style;
  no_shrinking.min_scale = 1;
  EXPECT_EQ(BadgeScalesFor(no_shrinking).count, 1u);
  no_shrinking.min_scale = kNaN;
  EXPECT_EQ(BadgeScalesFor(no_shrinking).count, 1u);
  BadgeStyle tiny = style;
  tiny.min_scale = 0.01f;
  EXPECT_FLOAT_EQ(BadgeScalesFor(tiny).scale[2], 0.3f);
}

// --- Placing badges -----------------------------------------------------------

TEST(VoiceOverlayLayoutTest, ABadgeSitsOnItsItemsCorner) {
  const BadgeStyle style = Style();
  const BadgePlan plan =
      PlaceBadges({{1, {100, 200, 300, 40}}}, 0, kViewport, style, 6);
  ASSERT_EQ(plan.badges.size(), 1u);
  EXPECT_EQ(plan.badges[0].anchor, BadgeAnchor::kInsideTopLeft);
  EXPECT_EQ(plan.badges[0].badge, (OverlayRect{100, 200, 30, 30}));
  EXPECT_FLOAT_EQ(plan.badges[0].scale, 1);
  EXPECT_FALSE(plan.ring);
  EXPECT_EQ(plan.crowded, 0u);
}

TEST(VoiceOverlayLayoutTest, BadgesStayOnScreenAtItsEdges) {
  const BadgeStyle style = Style();
  // Partly above and left of the viewport; at its right and bottom edges.
  const std::vector<BadgeRequest> requests = {
      {1, {-50, -20, 120, 40}},
      {2, {1270, 300, 40, 40}},
      {3, {600, 790, 100, 40}},
      {4, {-500, 100, 100, 40}},  // wholly outside: no badge
  };
  const BadgePlan plan = PlaceBadges(requests, 0, kViewport, style, 6);
  ExpectWellPlaced(plan, requests, kViewport, style);
  EXPECT_EQ(plan.badges.size(), 3u);
  EXPECT_FALSE(BadgeFor(plan, 4));
  const BadgePlacement* first = BadgeFor(plan, 1);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->badge.x, 0);
  EXPECT_EQ(first->badge.y, 0);
}

TEST(VoiceOverlayLayoutTest, BadgesOfACloseColumnMoveAside) {
  const BadgeStyle style = Style();
  // A column of 20 links 18 DIPs apart, as in a dense sidebar: a 30 DIP
  // badge can't sit on each one's corner.
  std::vector<BadgeRequest> requests;
  for (int i = 0; i < 20; ++i) {
    requests.push_back({i + 1, {200, 100 + 18.0f * i, 120, 16}});
  }
  const BadgePlan plan = PlaceBadges(requests, 0, kViewport, style, 6);
  ExpectWellPlaced(plan, requests, kViewport, style);
  // Every link gets a full-size badge, some left of the link.
  EXPECT_EQ(plan.badges.size(), 20u);
  EXPECT_EQ(plan.shrunk, 0u);
  bool outside = false;
  for (const BadgePlacement& badge : plan.badges) {
    outside |= badge.anchor == BadgeAnchor::kOutsideLeft;
  }
  EXPECT_TRUE(outside);
}

TEST(VoiceOverlayLayoutTest, CrowdedBadgesShrinkButNeverOverlap) {
  const BadgeStyle style = Style();
  // A grid of small buttons 22 DIPs apart (an emoji picker, a calendar):
  // full-size badges don't fit.
  std::vector<BadgeRequest> requests;
  for (int i = 0; i < 100; ++i) {
    requests.push_back(
        {i + 1, {300 + 22.0f * (i % 10), 200 + 22.0f * (i / 10), 20, 14}});
  }
  const BadgePlan plan = PlaceBadges(requests, 0, kViewport, style, 6);
  ExpectWellPlaced(plan, requests, kViewport, style);
  EXPECT_GT(plan.shrunk, 0u);
  EXPECT_GT(plan.crowded, 0u);  // some can't have a readable badge
  EXPECT_GT(plan.badges.size(), 30u);
  // Lower numbers come first.
  EXPECT_TRUE(BadgeFor(plan, 1));

  // Away from the grid, a lone button keeps a full-size badge.
  requests.push_back({101, {900, 600, 80, 30}});
  const BadgePlan with_lone = PlaceBadges(requests, 0, kViewport, style, 6);
  ExpectWellPlaced(with_lone, requests, kViewport, style);
  ASSERT_TRUE(BadgeFor(with_lone, 101));
  EXPECT_FLOAT_EQ(BadgeFor(with_lone, 101)->scale, 1);
  EXPECT_GE(with_lone.badges.size(), plan.badges.size());
}

TEST(VoiceOverlayLayoutTest, TheCurrentItemComesFirstWithARing) {
  const BadgeStyle style = Style();
  // Two items on top of each other: the current one (2) gets the corner.
  const std::vector<BadgeRequest> requests = {{1, {100, 100, 200, 30}},
                                              {2, {100, 100, 200, 30}}};
  const BadgePlan plan = PlaceBadges(requests, 2, kViewport, style, 6);
  ExpectWellPlaced(plan, requests, kViewport, style);
  ASSERT_FALSE(plan.badges.empty());
  EXPECT_EQ(plan.badges[0].number, 2);
  EXPECT_TRUE(plan.badges[0].current);
  EXPECT_EQ(plan.badges[0].anchor, BadgeAnchor::kInsideTopLeft);
  const BadgePlacement* other = BadgeFor(plan, 1);
  ASSERT_TRUE(other);
  EXPECT_FALSE(other->current);
  ASSERT_TRUE(plan.ring);
  EXPECT_TRUE(plan.ring->Contains({100, 100, 200, 30}));
  EXPECT_FLOAT_EQ(plan.ring->x, 100 - 4);

  // At the viewport's edge the ring stays wholly visible.
  const BadgePlan edge =
      PlaceBadges({{1, {0, 0, 200, 30}}}, 1, kViewport, style, 6);
  ASSERT_TRUE(edge.ring);
  EXPECT_TRUE(kViewport.Outset(-3).Contains(*edge.ring));

  // No ring for a current item off screen, or no current item.
  EXPECT_FALSE(
      PlaceBadges({{1, {0, 900, 200, 30}}}, 1, kViewport, style, 6).ring);
  EXPECT_FALSE(PlaceBadges(requests, 0, kViewport, style, 6).ring);
}

TEST(VoiceOverlayLayoutTest, RandomPagesAreWellPlaced) {
  const BadgeStyle style = Style();
  for (uint32_t seed = 1; seed <= 40; ++seed) {
    SCOPED_TRACE(seed);
    Lcg random(seed);
    std::vector<BadgeRequest> requests;
    const int count = 10 + static_cast<int>(random.Next(400));
    for (int i = 0; i < count; ++i) {
      requests.push_back({i + 1,
                          {random.Next(1400) - 60, random.Next(900) - 50,
                           4 + random.Next(300), 4 + random.Next(60)}});
    }
    const int current = static_cast<int>(random.Next(30));
    const BadgePlan plan = PlaceBadges(requests, current, kViewport, style, 6);
    ExpectWellPlaced(plan, requests, kViewport, style);
  }
}

// Bounds come from the page: whatever they are, nothing lands outside the
// viewport and nothing breaks.
TEST(VoiceOverlayLayoutTest, HostileBoundsAreSafe) {
  const BadgeStyle style = Style();
  const float values[] = {kNaN, kInf, -kInf, 0, -1, 1e30f, -1e30f, 3e38f,
                          1e-30f, 640, 12};
  std::vector<OverlayItem> items;
  int number = 0;
  for (float x : values) {
    for (float y : values) {
      for (float size : values) {
        OverlayItem item;
        item.number = ++number;
        item.handle = {0, number};
        item.bounds = {x, y, size, size};
        items.push_back(item);
      }
    }
  }
  for (float scale : {1.0f, 2.0f, kNaN, 0.0f}) {
    const std::vector<BadgeRequest> requests =
        VisibleItems(items, {0, 0, scale}, kViewport);
    for (const BadgeRequest& request : requests) {
      EXPECT_TRUE(std::isfinite(request.item.x));
      EXPECT_TRUE(std::isfinite(request.item.y));
      EXPECT_TRUE(std::isfinite(request.item.right()));
      EXPECT_TRUE(std::isfinite(request.item.bottom()));
      EXPECT_TRUE(request.item.Intersects(kViewport));
    }
    const BadgePlan plan = PlaceBadges(requests, 7, kViewport, style, 6);
    ExpectWellPlaced(plan, requests, kViewport, style);
  }
  // Requests made by hand with what VisibleItems() refuses.
  const std::vector<BadgeRequest> raw = {{1, {kNaN, 0, 10, 10}},
                                         {2, {0, kInf, 10, 10}},
                                         {3, {0, 0, -kInf, 10}},
                                         {4, {-1e30f, -1e30f, 2e30f, 2e30f}}};
  const BadgePlan plan = PlaceBadges(raw, 4, kViewport, style, 6);
  for (const BadgePlacement& badge : plan.badges) {
    EXPECT_TRUE(kViewport.Contains(badge.badge));
  }
  if (plan.ring) {
    EXPECT_TRUE(kViewport.Contains(*plan.ring));
  }
}

TEST(VoiceOverlayLayoutTest, BadgesAreCapped) {
  const BadgeStyle style = Style();
  // 900 well-spread items, each with room for its badge.
  std::vector<BadgeRequest> requests;
  const OverlayRect big = {0, 0, 4000, 4000};
  for (int i = 0; i < 900; ++i) {
    requests.push_back(
        {i + 1, {(i % 30) * 130.0f, (i / 30) * 130.0f, 100, 100}});
  }
  const BadgePlan plan = PlaceBadges(requests, 0, big, style, 6);
  EXPECT_EQ(plan.badges.size(), kMaxBadges);
  EXPECT_EQ(plan.over_cap, 900 - kMaxBadges);
  EXPECT_EQ(plan.crowded, 0u);
}

TEST(VoiceOverlayLayoutTest, NothingIsPlacedInAnEmptyOrBadViewport) {
  const BadgeStyle style = Style();
  const std::vector<BadgeRequest> requests = {{1, {0, 0, 10, 10}}};
  EXPECT_TRUE(PlaceBadges(requests, 1, {}, style, 6).badges.empty());
  EXPECT_TRUE(
      PlaceBadges(requests, 1, {kNaN, 0, 10, 10}, style, 6).badges.empty());
  // A viewport smaller than the smallest badge.
  const BadgePlan tiny = PlaceBadges(requests, 0, {0, 0, 8, 8}, style, 6);
  EXPECT_TRUE(tiny.badges.empty());
  EXPECT_EQ(tiny.crowded, 1u);
}

// --- The round 36 fixture pages ---------------------------------------------

TEST(VoiceOverlayLayoutTest, YouTubeResultsGetNumbersOnScreenOnly) {
  const PageMap map = BuildPageMap(test::YouTubeLikeResults());
  const std::vector<BadgeRequest> requests =
      VisibleItems(OverlayItemsFromMap(map), {0, 0, 1}, kViewport);
  ASSERT_FALSE(requests.empty());
  const BadgeStyle style = Style();
  const BadgePlan plan = PlaceBadges(requests, 0, kViewport, style, 6);
  ExpectWellPlaced(plan, requests, kViewport, style);
  EXPECT_LE(plan.crowded, requests.size() / 4);

  // The results' title links: the first three are on screen and numbered,
  // the rest are off screen and get none.
  ASSERT_FALSE(map.lists.empty());
  const PageList& results = map.lists[0];
  ASSERT_GE(results.entries.size(), 5u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_TRUE(BadgeFor(plan, results.entries[i].number)) << i;
  }
  EXPECT_FALSE(BadgeFor(plan, results.entries[4].number));
}

TEST(VoiceOverlayLayoutTest, FixturePagesAreWellPlaced) {
  const BadgeStyle style = Style();
  for (const PageTree& tree :
       {test::YouTubeLikeResults(), test::GoogleLikeResults(),
        test::WikipediaArticle(), test::PasswordForm(),
        test::CrossOriginFrames()}) {
    SCOPED_TRACE(tree.title);
    const std::vector<BadgeRequest> requests = RequestsFor(tree, {0, 0, 1});
    const BadgePlan plan = PlaceBadges(requests, 1, kViewport, style, 6);
    ExpectWellPlaced(plan, requests, kViewport, style);
  }
}

// The same page on a screen at twice the scale factor: Blink's pixels
// double, and the badges land in the same places.
TEST(VoiceOverlayLayoutTest, TheScaleFactorDoesNotMoveTheBadges) {
  PageTree tree = test::GoogleLikeResults();
  PageTree doubled = tree;
  for (PageNode& node : doubled.nodes) {
    node.bounds = {node.bounds.x * 2, node.bounds.y * 2, node.bounds.width * 2,
                   node.bounds.height * 2};
  }
  const BadgeStyle style = Style();
  const BadgePlan at1 =
      PlaceBadges(RequestsFor(tree, {0, 0, 1}), 3, kViewport, style, 6);
  const BadgePlan at2 =
      PlaceBadges(RequestsFor(doubled, {0, 0, 2}), 3, kViewport, style, 6);
  ASSERT_EQ(at1.badges.size(), at2.badges.size());
  for (size_t i = 0; i < at1.badges.size(); ++i) {
    EXPECT_EQ(at1.badges[i].number, at2.badges[i].number);
    EXPECT_EQ(at1.badges[i].badge, at2.badges[i].badge);
  }
  EXPECT_EQ(at1.ring, at2.ring);
}

// --- The caption ------------------------------------------------------------

TEST(VoiceOverlayLayoutTest, TheCaptionSaysWhatWasHeardAndWhatIsHappening) {
  const CaptionText caption =
      MakeCaptionText(VoiceState::kListening, "play the live one by Queen",
                      "Playing: Bohemian Rhapsody, Live Aid");
  EXPECT_EQ(caption.state, VoiceState::kListening);
  EXPECT_EQ(caption.state_label, "Listening");
  EXPECT_EQ(caption.heard, "Heard: play the live one by Queen");
  EXPECT_EQ(caption.status, "Playing: Bohemian Rhapsody, Live Aid");

  const CaptionText idle = MakeCaptionText(VoiceState::kWaiting, "  ", "");
  EXPECT_EQ(idle.state_label, "Waiting");
  EXPECT_TRUE(idle.heard.empty());
  EXPECT_TRUE(idle.status.empty());

  EXPECT_EQ(VoiceStateLabel(VoiceState::kOff), "Off");
  EXPECT_EQ(VoiceStateLabel(VoiceState::kThinking), "Thinking");
  EXPECT_EQ(VoiceStateLabel(VoiceState::kSpeaking), "Speaking");
}

TEST(VoiceOverlayLayoutTest, CaptionTextFromPagesIsCleanedAndCapped) {
  // Bidi overrides, zero-width and control characters, line breaks.
  const CaptionText caption = MakeCaptionText(
      VoiceState::kSpeaking, "open\xE2\x80\xAE moc.evil\x07",
      "Playing: Evil\xE2\x80\x8B Title\r\nwith \x1B[2Jbreaks");
  EXPECT_EQ(caption.heard, "Heard: open moc.evil");
  EXPECT_EQ(caption.status, "Playing: Evil Title with [2Jbreaks");
  EXPECT_EQ(SpeechSafeText(caption.status, kMaxCaptionStatusChars),
            caption.status);

  // Invalid UTF-8 reads as spaces.
  EXPECT_EQ(MakeCaptionText(VoiceState::kOff, "a\xFF\xFE" "b", "").heard,
            "Heard: a b");

  // Long text is cut.
  const std::string long_text(5000, 'x');
  const CaptionText capped =
      MakeCaptionText(VoiceState::kThinking, long_text, long_text);
  EXPECT_EQ(CountCodePoints(capped.heard),
            kHeardPrefix.size() + kMaxCaptionHeardChars);
  EXPECT_EQ(CountCodePoints(capped.status), kMaxCaptionStatusChars);
}

TEST(VoiceOverlayLayoutTest, TheCaptionBarsHeightDoesNotDependOnItsText) {
  const CaptionMetrics metrics = {26, 32, 10, 3};
  EXPECT_FLOAT_EQ(CaptionBarHeight(metrics), 3 + 25 + 64);

  // Fits a third of the page area, shrinking its text down to the floor.
  EXPECT_EQ(FitCaption(metrics, 16, 900).font, 26);
  const CaptionMetrics fitted = FitCaption(metrics, 16, 240);
  EXPECT_LT(fitted.font, 26);
  EXPECT_LE(CaptionBarHeight(fitted), 240.0f / 3 + 0.01f);
  EXPECT_FLOAT_EQ(fitted.line_height / fitted.font, 32.0f / 26);
  const CaptionMetrics floor = FitCaption(metrics, 16, 60);
  EXPECT_FLOAT_EQ(floor.font, 16);
  EXPECT_EQ(FitCaption(metrics, 16, kNaN).font, 26);
  EXPECT_EQ(FitCaption(metrics, 16, 0).font, 26);
}

// --- Colours ----------------------------------------------------------------

TEST(VoiceOverlayLayoutTest, DefaultColoursHaveHighContrast) {
  EXPECT_DOUBLE_EQ(ContrastRatio(0xFF000000, 0xFFFFFFFF), 21);
  EXPECT_DOUBLE_EQ(ContrastRatio(0xFF777777, 0xFF777777), 1);

  const OverlayPalette palette = ChoosePalette(false, SystemColors());
  // Text: WCAG AAA (7:1).
  EXPECT_GE(ContrastRatio(palette.badge_text, palette.badge_fill), 7);
  EXPECT_GE(ContrastRatio(palette.current_text, palette.current_fill), 7);
  EXPECT_GE(ContrastRatio(palette.caption_text, palette.caption_fill), 7);
  for (VoiceState state :
       {VoiceState::kOff, VoiceState::kWaiting, VoiceState::kListening,
        VoiceState::kThinking, VoiceState::kSpeaking}) {
    EXPECT_GE(ContrastRatio(palette.state_text, StateFill(palette, state)), 7)
        << VoiceStateLabel(state);
  }
  // The ring's band against its edges, and the badges against black and
  // white pages: well over 3:1 for graphics.
  EXPECT_GE(ContrastRatio(palette.ring_band, palette.ring_edge), 7);
  EXPECT_GE(ContrastRatio(palette.badge_border, palette.badge_fill), 7);
  // Each state has its own colour.
  for (int a = 0; a < 5; ++a) {
    for (int b = a + 1; b < 5; ++b) {
      EXPECT_NE(palette.state_fill[a], palette.state_fill[b]);
    }
  }
}

TEST(VoiceOverlayLayoutTest, ForcedColoursUseOnlyTheSystemsColours) {
  SystemColors system;
  system.window = 0xFF000000;
  system.window_text = 0xFFFFFFFF;
  system.highlight = 0xFF1AEBFF;
  system.highlight_text = 0xFF000000;
  const OverlayPalette palette = ChoosePalette(true, system);
  const OverlayColor all[] = {
      palette.badge_fill,    palette.badge_text,     palette.badge_border,
      palette.current_fill,  palette.current_text,   palette.current_border,
      palette.ring_band,     palette.ring_edge,      palette.caption_fill,
      palette.caption_text,  palette.caption_border, palette.state_text,
      palette.state_fill[0], palette.state_fill[4]};
  for (OverlayColor color : all) {
    EXPECT_TRUE(color == system.window || color == system.window_text ||
                color == system.highlight || color == system.highlight_text)
        << std::hex << color;
  }
  EXPECT_EQ(palette.badge_fill, system.highlight);
  EXPECT_EQ(palette.badge_text, system.highlight_text);
  EXPECT_EQ(palette.caption_fill, system.window);
  EXPECT_EQ(palette.caption_text, system.window_text);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
