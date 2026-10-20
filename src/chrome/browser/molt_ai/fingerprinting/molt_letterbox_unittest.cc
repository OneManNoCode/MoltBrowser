// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox.h"

#include "testing/gtest/include/gtest/gtest.h"
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

// What the page sees, by Blink's rounding: screen.width/height for a
// viewport of `length` DIPs (LetterboxBucket rounds width and height alike).
int BlinkScreenWidth(int length) {
  return blink::molt_fingerprinting::LetterboxBucket(length, 100).width;
}
int BlinkScreenHeight(int length) {
  return blink::molt_fingerprinting::LetterboxBucket(200, length).height;
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
}

}  // namespace
}  // namespace molt_ai::fingerprinting
