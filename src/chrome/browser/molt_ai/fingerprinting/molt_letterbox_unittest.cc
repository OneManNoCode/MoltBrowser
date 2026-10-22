// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"

namespace molt_ai::fingerprinting {
namespace {

// The browser rounds the page's size with Blink's steps and caps.
static_assert(kLetterboxStepWidth ==
              blink::molt_fingerprinting::kLetterboxStepWidth);
static_assert(kLetterboxStepHeight ==
              blink::molt_fingerprinting::kLetterboxStepHeight);
static_assert(kLetterboxMaxWidth ==
              blink::molt_fingerprinting::kLetterboxMaxWidth);
static_assert(kLetterboxMaxHeight ==
              blink::molt_fingerprinting::kLetterboxMaxHeight);
static_assert(kLetterboxMaxWidth % kLetterboxStepWidth == 0);
static_assert(kLetterboxMaxHeight % kLetterboxStepHeight == 0);

// A few cases at compile time.
static_assert(LetterboxMarginsFor(1400, 900) == LetterboxMargins{});
static_assert(LetterboxMarginsFor(1399, 899) == LetterboxMargins{.left = 99,
                                                                 .top = 49,
                                                                 .right = 100,
                                                                 .bottom = 50});
static_assert(
    LetterboxMarginsFor(2560, 1300) ==
    LetterboxMargins{.left = 580, .top = 200, .right = 580, .bottom = 200});
static_assert(LetterboxLength(150, 200, 1400) == 150);
static_assert(LetterboxLength(-5, 200, 1400) == 0);

// Zoomed pages are letterboxed in CSS pixels. At 125% on a 1x screen a
// 1500 x 1000 DIP area is 1200 x 800 CSS pixels: no margins.
static_assert(PageCssLength(1500, {1.0f, 1.25f}) == 1200);
static_assert(PageCssLength(1000, {1.0f, 1.25f}) == 800);
static_assert(LetterboxMarginsFor(1500, 1000, {1.0f, 1.25f}) ==
              LetterboxMargins{});
// At 50% on a 2x screen the page is capped at 1400 x 900 CSS pixels, which
// are 700 x 450 DIPs.
static_assert(LetterboxPageLength(1500,
                                  kLetterboxStepWidth,
                                  kLetterboxMaxWidth,
                                  {2.0f, 0.5f}) == 700);
static_assert(LetterboxPageLength(1000,
                                  kLetterboxStepHeight,
                                  kLetterboxMaxHeight,
                                  {2.0f, 0.5f}) == 450);
// The renderer's float rounding: 1400 DIPs at a scale factor of 1.2 are
// 1681 device pixels and 1401 CSS pixels, so a 1.2 screen can't have a
// 1400-pixel-wide page; it gets 1200.
static_assert(PageDevicePixels(1400, 1.2f) == 1681);
static_assert(PageCssPixels(1681, 1.2f) == 1401);
static_assert(LetterboxPageLength(1400,
                                  kLetterboxStepWidth,
                                  kLetterboxMaxWidth,
                                  {1.2f, 1.0f}) == 1200);
// A scale the renderer can't have counts as 100% on a 1x screen.
static_assert(LetterboxPageLength(1399,
                                  kLetterboxStepWidth,
                                  kLetterboxMaxWidth,
                                  {0.0f, 1.0f}) == 1200);
static_assert(LetterboxPageLength(1399,
                                  kLetterboxStepWidth,
                                  kLetterboxMaxWidth,
                                  {1.0f, -1.0f}) == 1200);

// What the page sees, by Blink's rounding: screen.width/height for a
// viewport of `length` CSS pixels (LetterboxBucket rounds width and height
// alike).
int BlinkScreenWidth(int length) {
  return blink::molt_fingerprinting::LetterboxBucket(length, 100).width;
}
int BlinkScreenHeight(int length) {
  return blink::molt_fingerprinting::LetterboxBucket(200, length).height;
}

// Screens the tests try: 1x, 2x and the fractional scales of Windows,
// GNOME, KDE and Wayland (which counts in 120ths, e.g. 160/120).
constexpr float kScaleFactors[] = {
    1.0f, 1.1f, 1.15f, 1.2f, 1.25f, 1.3f, static_cast<float>(160 / 120.0),
    1.4f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f, 3.0f};

// The zoom factors a page gets from the zoom menu, the keyboard and the
// mouse wheel, as its renderer has them: each preset made a zoom level
// (zoom::PageZoom), sent, made a factor again and cast to float
// (WebFrameWidgetImpl::SetZoomInternal).
std::vector<float> PresetZoomFactors() {
  std::vector<float> factors;
  for (double preset : blink::kPresetBrowserZoomFactors) {
    factors.push_back(static_cast<float>(
        blink::ZoomLevelToZoomFactor(blink::ZoomFactorToZoomLevel(preset))));
  }
  return factors;
}

// The point of letterboxing: once the browser has rounded the content area,
// the page's size is what Blink reports as the screen size, and it is the
// size Blink would have reported for the whole area anyway.
TEST(MoltLetterboxTest, PageSizeEqualsTheUniformScreen) {
  for (int width = kLetterboxStepWidth; width <= 4000; ++width) {
    const int page =
        LetterboxLength(width, kLetterboxStepWidth, kLetterboxMaxWidth);
    EXPECT_EQ(BlinkScreenWidth(page), page) << width;
    EXPECT_EQ(BlinkScreenWidth(width), page) << width;
  }
  for (int height = kLetterboxStepHeight; height <= 3000; ++height) {
    const int page =
        LetterboxLength(height, kLetterboxStepHeight, kLetterboxMaxHeight);
    EXPECT_EQ(BlinkScreenHeight(page), page) << height;
    EXPECT_EQ(BlinkScreenHeight(height), page) << height;
  }
}

TEST(MoltLetterboxTest, MarginsCentreThePage) {
  for (int width = 0; width <= 3000; width += 7) {
    for (int height = 0; height <= 2000; height += 11) {
      const LetterboxMargins m = LetterboxMarginsFor(width, height);
      const int page_width = width - m.left - m.right;
      const int page_height = height - m.top - m.bottom;
      EXPECT_EQ(page_width, LetterboxLength(width, kLetterboxStepWidth,
                                            kLetterboxMaxWidth));
      EXPECT_EQ(page_height, LetterboxLength(height, kLetterboxStepHeight,
                                             kLetterboxMaxHeight));
      EXPECT_GE(m.left, 0);
      EXPECT_GE(m.top, 0);
      // Centred: the two sides differ by at most the odd DIP, which goes
      // right / bottom.
      EXPECT_TRUE(m.right == m.left || m.right == m.left + 1);
      EXPECT_TRUE(m.bottom == m.top || m.bottom == m.top + 1);
    }
  }
}

TEST(MoltLetterboxTest, SizesAreStepsUpToTheCap) {
  EXPECT_EQ(LetterboxLength(200, 200, 1400), 200);
  EXPECT_EQ(LetterboxLength(399, 200, 1400), 200);
  EXPECT_EQ(LetterboxLength(400, 200, 1400), 400);
  EXPECT_EQ(LetterboxLength(1399, 200, 1400), 1200);
  EXPECT_EQ(LetterboxLength(1400, 200, 1400), 1400);
  EXPECT_EQ(LetterboxLength(5120, 200, 1400), 1400);
  EXPECT_EQ(LetterboxLength(899, 100, 900), 800);
  EXPECT_EQ(LetterboxLength(2000, 100, 900), 900);
}

// Below one step there is nothing to round down to: the page keeps the
// area (and Blink reports one step, the documented mismatch).
TEST(MoltLetterboxTest, TinyAreasAreNotLetterboxed) {
  EXPECT_EQ(LetterboxMarginsFor(150, 80), LetterboxMargins{});
  EXPECT_EQ(LetterboxMarginsFor(0, 0), LetterboxMargins{});
  EXPECT_EQ(LetterboxMarginsFor(-10, -10), LetterboxMargins{});
  const LetterboxMargins narrow = LetterboxMarginsFor(150, 950);
  EXPECT_EQ(narrow.left, 0);
  EXPECT_EQ(narrow.right, 0);
  EXPECT_EQ(narrow.top + narrow.bottom, 50);
  // At 200% a 300-DIP area is 150 CSS pixels: less than one step too.
  const PageScale zoomed{1.0f, 2.0f};
  EXPECT_EQ(
      LetterboxPageLength(300, kLetterboxStepWidth, kLetterboxMaxWidth, zoomed),
      300);
  EXPECT_EQ(LetterboxMarginsFor(300, 150, zoomed), LetterboxMargins{});
}

// The browser's copy of the renderer's arithmetic is farbling.h's, which
// Blink's letterbox_test.cc checks against Blink and gfx themselves.
TEST(MoltLetterboxTest, RendererArithmeticIsBlinks) {
  std::vector<float> zooms;
  for (float scale : kScaleFactors) {
    for (int dips = 0; dips <= 5000; ++dips) {
      ASSERT_EQ(PageDevicePixels(dips, scale),
                blink::molt_fingerprinting::PageDevicePixels(dips, scale))
          << scale << " " << dips;
    }
    for (float zoom : PresetZoomFactors()) {
      zooms.push_back(PageLayoutZoom({scale, zoom}));
    }
  }
  for (float zoom : zooms) {
    for (int pixels = 0; pixels <= 16000; ++pixels) {
      ASSERT_EQ(PageCssPixels(pixels, zoom),
                blink::molt_fingerprinting::PageCssPixels(pixels, zoom))
          << zoom << " " << pixels;
      ASSERT_EQ(PageMediaQueryPixels(pixels, zoom),
                blink::molt_fingerprinting::PageMediaQueryPixels(pixels, zoom))
          << zoom << " " << pixels;
    }
  }
}

// One dimension at `scale`, every area from 1 to `limit` DIPs, against an
// oracle that doesn't search: every DIP length's CSS length, in one pass.
// Returns how many areas were letterboxed, how many of those have a
// media-query length of exactly the step multiple and how many within the
// media queries' tolerance.
struct Counts {
  int areas = 0;
  int letterboxed = 0;
  int exact = 0;
  int within_tolerance = 0;
};
Counts CheckEveryArea(int step,
                      int max,
                      int limit,
                      const PageScale& scale,
                      int (*blink_screen)(int)) {
  Counts counts;
  const int max_css = PageCssLength(limit, scale);
  // first[css] / last[css]: the shortest / longest DIP length whose CSS
  // length is `css` (0: none).
  std::vector<int> first(static_cast<size_t>(max_css) + 1, 0);
  std::vector<int> last(static_cast<size_t>(max_css) + 1, 0);
  for (int dips = 1; dips <= limit; ++dips) {
    const int css = PageCssLength(dips, scale);
    EXPECT_GE(css, 0);
    if (css < 0 || css > max_css) {
      continue;
    }
    if (!first[static_cast<size_t>(css)]) {
      first[static_cast<size_t>(css)] = dips;
    }
    last[static_cast<size_t>(css)] = dips;
  }
  const float zoom = PageLayoutZoom(scale);
  auto media_error = [&](int dips, int css) {
    const float media = PageMediaQueryPixels(
        PageDevicePixels(dips, scale.device_scale_factor), zoom);
    const float target = static_cast<float>(css);
    return media > target ? media - target : target - media;
  };
  // The smallest media-query error of the DIP lengths up to `area` whose CSS
  // length is `css`, or -1 if there are none.
  auto best_error = [&](int css, int area) {
    const size_t i = static_cast<size_t>(css);
    if (!first[i] || first[i] > area) {
      return -1.0f;
    }
    float best = media_error(first[i], css);
    for (int dips = first[i]; dips <= std::min(last[i], area); ++dips) {
      best = std::min(best, media_error(dips, css));
    }
    return best;
  };
  const float tolerance = 1.0f / 64;
  for (int area = 1; area <= limit; ++area) {
    ++counts.areas;
    const int page = LetterboxPageLength(area, step, max, scale);
    EXPECT_GE(page, 1);
    EXPECT_LE(page, area);
    // The largest step multiple, at most `max`, that a page in this area can
    // be exactly with its media-query length within the tolerance; failing
    // that, the largest it can be exactly at all.
    int expected = 0;
    int fallback = 0;
    for (int css = std::min(max, PageCssLength(area, scale)) / step * step;
         css >= step; css -= step) {
      const float error = best_error(css, area);
      if (error < 0) {
        continue;
      }
      if (error <= tolerance) {
        expected = css;
        break;
      }
      if (!fallback) {
        fallback = css;
      }
    }
    if (!expected) {
      expected = fallback;
    }
    if (!expected) {
      // Too short for one step (or no step reachable): not letterboxed.
      EXPECT_EQ(page, area) << area;
      continue;
    }
    ++counts.letterboxed;
    const int css = PageCssLength(page, scale);
    EXPECT_EQ(css, expected) << area;
    // innerWidth/innerHeight is what Blink reports as the screen.
    EXPECT_EQ(blink_screen(css), css) << area;
    // Of the DIP lengths that give `css`, the one whose media-query length
    // is closest to it, and that is within one device pixel.
    EXPECT_EQ(media_error(page, css), best_error(css, area)) << area;
    EXPECT_LE(media_error(page, css), 1.0f / zoom + 0.001f) << area;
    counts.exact += media_error(page, css) == 0.0f;
    counts.within_tolerance += media_error(page, css) <= tolerance;
    // A letterboxed page is letterboxed already.
    EXPECT_EQ(LetterboxPageLength(page, step, max, scale), page) << area;
  }
  return counts;
}

// Every zoom level the zoom menu has, on every screen, for every content
// area: the page's CSS size is a letterbox size, the largest that can be
// hit, and the screen Blink reports.
TEST(MoltLetterboxTest, ZoomedPagesAreLetterboxedInCssPixels) {
  Counts widths;
  Counts heights;
  for (float scale_factor : kScaleFactors) {
    for (float zoom : PresetZoomFactors()) {
      SCOPED_TRACE(testing::Message()
                   << "scale " << scale_factor << " zoom " << zoom);
      const PageScale scale{scale_factor, zoom};
      const Counts w = CheckEveryArea(kLetterboxStepWidth, kLetterboxMaxWidth,
                                      3000, scale, BlinkScreenWidth);
      const Counts h = CheckEveryArea(kLetterboxStepHeight, kLetterboxMaxHeight,
                                      2000, scale, BlinkScreenHeight);
      for (auto [total, one] :
           {std::pair(&widths, w), std::pair(&heights, h)}) {
        total->areas += one.areas;
        total->letterboxed += one.letterboxed;
        total->exact += one.exact;
        total->within_tolerance += one.within_tolerance;
      }
      if (::testing::Test::HasFatalFailure()) {
        return;
      }
    }
  }
  // Most areas are letterboxed (the others are shorter than one step at
  // high zoom), most media-query lengths are exact and nearly all are
  // within the media queries' tolerance.
  for (const Counts& counts : {widths, heights}) {
    EXPECT_GT(counts.letterboxed, counts.areas * 3 / 4);
    EXPECT_GT(counts.exact, counts.letterboxed / 2);
    EXPECT_GT(counts.within_tolerance, counts.letterboxed / 10 * 9);
  }
}

// What a letterboxed page then sees in MoltNet, by the renderer's arithmetic
// (farbling.h): its media-query length and its viewport-unit and
// visualViewport length are exactly its innerWidth/innerHeight whenever the
// browser could letterbox it exactly; and with a classic scrollbar,
// innerWidth - documentElement.clientWidth and the visualViewport length
// are the CSS thickness exactly wherever that thickness is a whole number of
// device pixels. Counts the pages and how many of them see whole lengths.
struct ViewportCounts {
  int letterboxed = 0;
  int whole_lengths = 0;         // media queries, vw/vh, visualViewport
  int scrollbar_whole_zoom = 0;  // pages at a zoom where 16 CSS px are whole
  int scrollbar_exact = 0;       // clientWidth and visualViewport both exact
};
ViewportCounts CheckWhatThePageSees(int step,
                                    int max,
                                    int limit,
                                    const PageScale& scale) {
  namespace mf = blink::molt_fingerprinting;
  ViewportCounts counts;
  const float zoom = PageLayoutZoom(scale);
  const int thickness =
      mf::UniformScrollbarDevicePixels(mf::kUniformScrollbarThickness, zoom);
  const double exact_thickness =
      mf::kUniformScrollbarThickness * static_cast<double>(zoom);
  const bool whole_thickness = std::abs(thickness - exact_thickness) < 1e-3;
  for (int area = 1; area <= limit; ++area) {
    const int page = LetterboxPageLength(area, step, max, scale);
    const int css = PageCssLength(page, scale);
    if (PageCssLength(area, scale) < step || css < step || css % step) {
      continue;
    }
    ++counts.letterboxed;
    const int device_pixels = PageDevicePixels(page, scale.device_scale_factor);
    const float media = PageMediaQueryPixels(device_pixels, zoom);
    const double snapped_media = mf::SnapViewportLength(media);
    const double snapped_units =
        mf::SnapViewportLength(mf::PageViewportPixels(device_pixels, zoom));
    const double target = css;
    if (std::abs(media - static_cast<float>(css)) <= 1.0f / 64) {
      // The browser letterboxed it exactly: every length is the step.
      EXPECT_EQ(snapped_media, target) << area;
      EXPECT_EQ(snapped_units, target) << area;
      ++counts.whole_lengths;
    } else {
      // The browser's fallback: left as it is, and the same for both.
      EXPECT_NE(snapped_media, target) << area;
      EXPECT_LE(std::abs(snapped_media - snapped_units), 0.001) << area;
    }
    // A classic scrollbar across this dimension.
    const int client = PageCssPixels(device_pixels - thickness, zoom);
    const double visual = mf::SnapViewportLength(
        mf::PageViewportPixels(device_pixels - thickness, zoom));
    const bool exact = css - client == mf::kUniformScrollbarThickness &&
                       visual == css - mf::kUniformScrollbarThickness;
    if (whole_thickness) {
      ++counts.scrollbar_whole_zoom;
      if (std::abs(media - static_cast<float>(css)) <= 1.0f / 64) {
        EXPECT_TRUE(exact) << area << ": clientWidth " << client
                           << ", visualViewport " << visual;
      }
    }
    counts.scrollbar_exact += exact;
  }
  return counts;
}

TEST(MoltLetterboxTest, PagesSeeWholeViewportLengthsAndScrollbars) {
  ViewportCounts total;
  int whole_pairs = 0;
  int pairs = 0;
  for (float scale_factor : kScaleFactors) {
    for (float zoom : PresetZoomFactors()) {
      SCOPED_TRACE(testing::Message()
                   << "scale " << scale_factor << " zoom " << zoom);
      const PageScale scale{scale_factor, zoom};
      for (const ViewportCounts& one :
           {CheckWhatThePageSees(kLetterboxStepWidth, kLetterboxMaxWidth, 3000,
                                 scale),
            CheckWhatThePageSees(kLetterboxStepHeight, kLetterboxMaxHeight,
                                 2000, scale)}) {
        total.letterboxed += one.letterboxed;
        total.whole_lengths += one.whole_lengths;
        total.scrollbar_whole_zoom += one.scrollbar_whole_zoom;
        total.scrollbar_exact += one.scrollbar_exact;
      }
      ++pairs;
      const float layout_zoom = PageLayoutZoom(scale);
      whole_pairs +=
          std::abs(blink::molt_fingerprinting::UniformScrollbarDevicePixels(
                       16, layout_zoom) -
                   16 * static_cast<double>(layout_zoom)) < 1e-3;
      if (::testing::Test::HasFailure()) {
        return;
      }
    }
  }
  // As measured when this was written: 96.5% of the letterboxed pages see
  // whole lengths (the rest are the browser's fallback, mostly pages a few
  // hundred CSS pixels across at high zoom and 4/3 screens); 16 CSS pixels
  // are whole device pixels at 126 of the 238 zoom x screen pairs, and the
  // scrollbar shows exactly 16 CSS pixels on about half of all pages.
  EXPECT_GT(total.whole_lengths, total.letterboxed / 100 * 96);
  EXPECT_EQ(pairs, 14 * 17);
  EXPECT_GE(whole_pairs, 126);
  EXPECT_GT(total.scrollbar_exact, total.letterboxed / 2);
  EXPECT_GT(total.scrollbar_exact, total.scrollbar_whole_zoom * 9 / 10);
}

// At 100% on the usual screens the page is letterboxed in DIPs, as before
// zoom was taken into account.
TEST(MoltLetterboxTest, FullZoomOnUsualScreensIsTheDipLetterbox) {
  for (float scale_factor : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f}) {
    const PageScale scale{scale_factor, 1.0f};
    for (int area = 0; area <= 4000; ++area) {
      EXPECT_EQ(LetterboxPageLength(area, kLetterboxStepWidth,
                                    kLetterboxMaxWidth, scale),
                LetterboxLength(area, kLetterboxStepWidth, kLetterboxMaxWidth))
          << scale_factor << " " << area;
      EXPECT_EQ(
          LetterboxPageLength(area, kLetterboxStepHeight, kLetterboxMaxHeight,
                              scale),
          LetterboxLength(area, kLetterboxStepHeight, kLetterboxMaxHeight))
          << scale_factor << " " << area;
    }
  }
}

// What a user sees, with the zoom factors the renderer really gets.
TEST(MoltLetterboxTest, ZoomExamples) {
  auto zoom = [](double preset) {
    return static_cast<float>(
        blink::ZoomLevelToZoomFactor(blink::ZoomFactorToZoomLevel(preset)));
  };
  struct Example {
    float scale_factor;
    double zoom;
    int area_width;
    int area_height;
    int page_width;   // DIPs
    int page_height;  // DIPs
    int css_width;
    int css_height;
  };
  const Example examples[] = {
      // 125%: the page keeps 1500 x 1000 DIPs, 1200 x 800 CSS pixels.
      {1.0f, 1.25, 1500, 1000, 1500, 1000, 1200, 800},
      // 90%: 1400 x 900 CSS pixels are 1260 x 810 DIPs.
      {1.0f, 0.9, 1400, 900, 1260, 810, 1400, 900},
      // 200% on a 2x screen: 600 x 400 CSS pixels in 1440 x 900 DIPs.
      {2.0f, 2.0, 1440, 900, 1200, 800, 600, 400},
      // 50% on a 2x screen: capped at 1400 x 900 CSS pixels.
      {2.0f, 0.5, 1920, 1080, 700, 450, 1400, 900},
      // 33%: one DIP is three CSS pixels, so 1400 can't be hit; 1200 can.
      {1.0f, 1 / 3.0, 1920, 1080, 400, 300, 1200, 900},
      // A 1.2 screen at 100%: 1400 can't be hit either.
      {1.2f, 1.0, 1400, 900, 1200, 900, 1200, 900},
      // 175% on a 1.5 screen: 500 CSS pixels high would be 1312.5 device
      // pixels; the page could get 1313, but media queries would see 500.19
      // there, so it gets 400 (1050 device pixels), which is exact.
      {1.5f, 1.75, 1700, 1000, 1400, 700, 800, 400},
  };
  for (const Example& e : examples) {
    const PageScale scale{e.scale_factor, zoom(e.zoom)};
    SCOPED_TRACE(testing::Message()
                 << "scale " << e.scale_factor << " zoom " << e.zoom);
    const int width = LetterboxPageLength(e.area_width, kLetterboxStepWidth,
                                          kLetterboxMaxWidth, scale);
    const int height = LetterboxPageLength(e.area_height, kLetterboxStepHeight,
                                           kLetterboxMaxHeight, scale);
    EXPECT_EQ(width, e.page_width);
    EXPECT_EQ(height, e.page_height);
    EXPECT_EQ(PageCssLength(width, scale), e.css_width);
    EXPECT_EQ(PageCssLength(height, scale), e.css_height);
    const LetterboxMargins margins =
        LetterboxMarginsFor(e.area_width, e.area_height, scale);
    EXPECT_EQ(margins.left + margins.right, e.area_width - e.page_width);
    EXPECT_EQ(margins.top + margins.bottom, e.area_height - e.page_height);
  }
}

// New MoltNet windows: a 1024 x 700 window whose page area is 1008 x 610
// (16 DIPs of frame, 90 of frame, tab strip and toolbar).
static_assert(WindowLetterboxSize(1024, 700, 1008, 610, 1920, 1040) ==
              LetterboxSize{1400, 900});
static_assert(WindowLetterboxSize(1024, 700, 1008, 610, 1366, 728) ==
              LetterboxSize{1200, 600});
static_assert(WindowLetterboxSize(1024, 700, 1008, 610, 1416, 990) ==
              LetterboxSize{1400, 900});
static_assert(WindowLetterboxSize(1024, 700, 1008, 610, 1415, 989) ==
              LetterboxSize{1200, 800});
static_assert(WindowLetterboxSize(1024, 700, 1008, 610, 200, 600).IsEmpty());
static_assert(
    WindowLetterboxSize(1024, 700, 1008, 610, 1920, 1040).width ==
    WindowLetterboxSize(1024, 700, 1008, 610, 1920, 1040, {2.0f, 1.0f}).width);
// On a 1.2 screen a 1400-pixel-wide page can't be hit: 1200.
static_assert(
    WindowLetterboxSize(1024, 700, 1008, 610, 1920, 1040, {1.2f, 1.0f}) ==
    LetterboxSize{1200, 900});
static_assert(WindowLetterboxSize(1024, 700, 0, 610, 1920, 1040).IsEmpty());
static_assert(WindowLetterboxSize(1000, 600, 1008, 610, 1920, 1040).IsEmpty());

// The window that results: its page area is the letterbox size (so the
// page gets no margins), and the window fits on the screen; and no larger
// letterbox size would have fit.
TEST(MoltLetterboxTest, NewWindowsOpenAtTheLargestLetterboxThatFits) {
  for (int chrome_width : {0, 2, 16, 320}) {
    for (int chrome_height : {0, 31, 90, 141}) {
      for (int work_width = 0; work_width <= 3000; work_width += 23) {
        for (int work_height = 0; work_height <= 2000; work_height += 19) {
          const LetterboxSize page =
              WindowLetterboxSize(800 + chrome_width, 600 + chrome_height, 800,
                                  600, work_width, work_height);
          const int room_width = work_width - chrome_width;
          const int room_height = work_height - chrome_height;
          if (room_width < kLetterboxStepWidth ||
              room_height < kLetterboxStepHeight) {
            EXPECT_TRUE(page.IsEmpty());
            continue;
          }
          ASSERT_FALSE(page.IsEmpty());
          // A letterbox size: the page gets no margins in that window.
          EXPECT_EQ(LetterboxMarginsFor(page.width, page.height),
                    LetterboxMargins{});
          EXPECT_EQ(page.width % kLetterboxStepWidth, 0);
          EXPECT_EQ(page.height % kLetterboxStepHeight, 0);
          EXPECT_LE(page.width, kLetterboxMaxWidth);
          EXPECT_LE(page.height, kLetterboxMaxHeight);
          // The window fits.
          EXPECT_LE(page.width + chrome_width, work_width);
          EXPECT_LE(page.height + chrome_height, work_height);
          // The largest that fits.
          EXPECT_TRUE(page.width == kLetterboxMaxWidth ||
                      page.width + kLetterboxStepWidth + chrome_width >
                          work_width);
          EXPECT_TRUE(page.height == kLetterboxMaxHeight ||
                      page.height + kLetterboxStepHeight + chrome_height >
                          work_height);
        }
      }
    }
  }
}

// On every screen a new window's page starts without margins too, and is a
// letterbox size in CSS pixels.
TEST(MoltLetterboxTest, NewWindowsOpenWithoutMarginsOnEveryScreen) {
  for (float scale_factor : kScaleFactors) {
    const PageScale scale{scale_factor, 1.0f};
    for (int work_width = 0; work_width <= 3000; work_width += 37) {
      for (int work_height = 0; work_height <= 2000; work_height += 29) {
        const LetterboxSize page = WindowLetterboxSize(
            816, 690, 800, 600, work_width, work_height, scale);
        if (page.IsEmpty()) {
          continue;
        }
        EXPECT_EQ(LetterboxMarginsFor(page.width, page.height, scale),
                  LetterboxMargins{})
            << scale_factor;
        EXPECT_EQ(PageCssLength(page.width, scale) % kLetterboxStepWidth, 0);
        EXPECT_EQ(PageCssLength(page.height, scale) % kLetterboxStepHeight, 0);
        EXPECT_LE(page.width + 16, work_width);
        EXPECT_LE(page.height + 90, work_height);
      }
    }
  }
}

// Whatever size the window had before (the last window's, the saved one),
// the result depends only on the browser UI's size and the screen.
TEST(MoltLetterboxTest, NewWindowSizeDoesNotDependOnThePreviousSize) {
  const LetterboxSize expected =
      WindowLetterboxSize(1000, 700, 984, 610, 1680, 1010);
  EXPECT_EQ(expected, (LetterboxSize{1400, 900}));
  for (int page_width = 1; page_width <= 2500; page_width += 37) {
    for (int page_height = 1; page_height <= 1500; page_height += 29) {
      EXPECT_EQ(WindowLetterboxSize(page_width + 16, page_height + 90,
                                    page_width, page_height, 1680, 1010),
                expected);
    }
  }
}

}  // namespace
}  // namespace molt_ai::fingerprinting
