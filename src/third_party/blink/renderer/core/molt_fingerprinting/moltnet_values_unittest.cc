// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_values.h"

#include <cstring>
#include <string>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

TEST(MoltNetValuesTest, OrientationFollowsTheDesktopRule) {
  EXPECT_EQ(OrientationOf(1400, 900), Orientation::kLandscapePrimary);
  EXPECT_EQ(OrientationOf(201, 200), Orientation::kLandscapePrimary);
  // Square and taller screens are portrait, as for a real desktop display.
  EXPECT_EQ(OrientationOf(600, 600), Orientation::kPortraitPrimary);
  EXPECT_EQ(OrientationOf(200, 900), Orientation::kPortraitPrimary);
  // Degenerate sizes don't crash.
  EXPECT_EQ(OrientationOf(0, 0), Orientation::kPortraitPrimary);
  EXPECT_EQ(OrientationOf(-1, -5), Orientation::kLandscapePrimary);
  EXPECT_EQ(kOrientationAngle, 0u);
}

// The uniform screen is the viewport rounded down to the letterbox steps:
// landscape for every window whose rounded size is wider than tall, which
// is any window at least 200 DIPs wider than tall.
TEST(MoltNetValuesTest, LetterboxedScreensAreLandscapeUnlessNarrow) {
  for (int width = 1; width <= 3000; width += 7) {
    for (int height = 1; height <= 2000; height += 13) {
      const LetterboxSize screen = LetterboxBucket(width, height);
      const Orientation orientation =
          OrientationOf(screen.width, screen.height);
      if (width >= height + kLetterboxStepWidth) {
        EXPECT_EQ(orientation, Orientation::kLandscapePrimary)
            << width << "x" << height;
      }
      EXPECT_EQ(orientation == Orientation::kPortraitPrimary,
                screen.height >= screen.width)
          << width << "x" << height;
    }
  }
  // Every full-size window is landscape.
  const LetterboxSize full = LetterboxBucket(1920, 1080);
  EXPECT_EQ(OrientationOf(full.width, full.height),
            Orientation::kLandscapePrimary);
  // A window snapped to half of a 1366x768 laptop screen is not.
  const LetterboxSize snapped = LetterboxBucket(683, 650);
  EXPECT_EQ(OrientationOf(snapped.width, snapped.height),
            Orientation::kPortraitPrimary);
}

TEST(MoltNetValuesTest, TouchValuesAreTheSpecsUnknowns) {
  EXPECT_EQ(kTouchRadius, 0.0f);
  EXPECT_EQ(kTouchRotationAngle, 0.0f);
  EXPECT_EQ(kTouchForce, 0.0f);
  EXPECT_EQ(kPointerContactSize, 1.0);
  EXPECT_EQ(kPointerTilt, 0);
  EXPECT_EQ(kPointerTwist, 0);
  EXPECT_EQ(kPointerTangentialPressure, 0.0f);
  EXPECT_DOUBLE_EQ(kPointerAltitudeAngle, 3.14159265358979323846 / 2);
  EXPECT_EQ(kPointerAzimuthAngle, 0.0);
  EXPECT_EQ(PointerPressure(true), 0.5f);
  EXPECT_EQ(PointerPressure(false), 0.0f);
}

TEST(MoltNetValuesTest, FormLocaleIsEnUs) {
  EXPECT_STREQ(kFormLocale, "en-US");
  EXPECT_STREQ(kDateFormat, "M/d/yy");
  EXPECT_STREQ(kMonthFormat, "MMMM yyyy");
  EXPECT_STREQ(kShortMonthFormat, "MMM yyyy");
  EXPECT_STREQ(kTimeFormat, "h:mm:ss a");
  EXPECT_STREQ(kShortTimeFormat, "h:mm a");
  EXPECT_EQ(kFirstDayOfWeek, 0u);  // Sunday
  EXPECT_STREQ(kMonthLabels[0], "January");
  EXPECT_STREQ(kMonthLabels[11], "December");
  EXPECT_STREQ(kAmPmLabels[0], "AM");
  EXPECT_STREQ(kAmPmLabels[1], "PM");
  EXPECT_STREQ(kDecimalSeparator, ".");
  EXPECT_STREQ(kGroupSeparator, ",");
}

TEST(MoltNetValuesTest, FormLocaleIsConsistent) {
  // The date-time formats are the date format and a time format joined the
  // way ICU joins en-US short dates and times.
  EXPECT_EQ(std::string(kDateTimeFormatWithSeconds),
            std::string(kDateFormat) + ", " + kTimeFormat);
  EXPECT_EQ(std::string(kDateTimeFormatWithoutSeconds),
            std::string(kDateFormat) + ", " + kShortTimeFormat);

  // Each pattern has the fields Blink's date/time editors need.
  const std::string_view date = kDateFormat;
  EXPECT_NE(date.find('M'), std::string_view::npos);
  EXPECT_NE(date.find('d'), std::string_view::npos);
  EXPECT_NE(date.find('y'), std::string_view::npos);
  for (std::string_view month : {std::string_view(kMonthFormat),
                                 std::string_view(kShortMonthFormat)}) {
    EXPECT_NE(month.find("MMM"), std::string_view::npos) << month;
    EXPECT_NE(month.find('y'), std::string_view::npos) << month;
    EXPECT_EQ(month.find('d'), std::string_view::npos) << month;
  }
  const std::string_view time = kTimeFormat;
  const std::string_view short_time = kShortTimeFormat;
  EXPECT_NE(time.find('h'), std::string_view::npos);
  EXPECT_NE(time.find("mm"), std::string_view::npos);
  EXPECT_NE(time.find("ss"), std::string_view::npos);
  EXPECT_NE(time.find('a'), std::string_view::npos);
  EXPECT_EQ(short_time.find('s'), std::string_view::npos);
  EXPECT_EQ(time.substr(0, 4), short_time.substr(0, 4));

  // Labels: complete, and the short month names are the long ones cut to
  // three letters, as in English.
  for (size_t i = 0; i < kMonthLabels.size(); ++i) {
    ASSERT_NE(kMonthLabels[i], nullptr);
    ASSERT_NE(kShortMonthLabels[i], nullptr);
    EXPECT_EQ(std::strlen(kShortMonthLabels[i]), 3u);
    EXPECT_EQ(std::string_view(kMonthLabels[i]).substr(0, 3),
              std::string_view(kShortMonthLabels[i]));
  }
  const char* const kWeekDays[] = {"Sunday",   "Monday", "Tuesday",
                                   "Wednesday", "Thursday", "Friday",
                                   "Saturday"};
  ASSERT_EQ(kWeekDayShortLabels.size(), std::size(kWeekDays));
  for (size_t i = 0; i < kWeekDayShortLabels.size(); ++i) {
    ASSERT_EQ(std::strlen(kWeekDayShortLabels[i]), 1u);
    EXPECT_EQ(kWeekDayShortLabels[i][0], kWeekDays[i][0]) << i;
  }
  EXPECT_LT(kFirstDayOfWeek, 7u);

  // Number symbols allow Blink's single-character filtering: one-character
  // separators and sign, no suffixes.
  EXPECT_EQ(std::strlen(kDecimalSeparator), 1u);
  EXPECT_EQ(std::strlen(kGroupSeparator), 1u);
  EXPECT_STRNE(kDecimalSeparator, kGroupSeparator);
  EXPECT_EQ(std::strlen(kNegativePrefix), 1u);
  EXPECT_EQ(std::strlen(kPositivePrefix), 0u);
  EXPECT_EQ(std::strlen(kPositiveSuffix), 0u);
  EXPECT_EQ(std::strlen(kNegativeSuffix), 0u);
}

// The display the media queries describe agrees with screen.colorDepth and
// is the plain one: sRGB, standard dynamic range, colour (not monochrome).
TEST(MoltNetValuesTest, TheDisplayIsAPlainSrgbScreen) {
  EXPECT_EQ(kDisplayGamut, DisplayGamut::kSRGB);
  EXPECT_FALSE(kDisplayHighDynamicRange);
  EXPECT_EQ(3 * kDisplayColorBitsPerComponent,
            static_cast<int>(kUniformColorDepth));
  // (color) and (monochrome) can't both match: a colour screen reports no
  // monochrome bits.
  EXPECT_GT(kDisplayColorBitsPerComponent, 0);
  EXPECT_EQ(kDisplayMonochromeBitsPerComponent, 0);
}

// Each platform's default scrollbar style: overlay where Chromium turns
// overlay scrollbars on by default, classic elsewhere.
TEST(MoltNetValuesTest, ScrollbarStyleIsThePlatformDefault) {
  EXPECT_TRUE(UsesOverlayScrollbars(FontPlatform::kMac));
  EXPECT_TRUE(UsesOverlayScrollbars(FontPlatform::kAndroid));
  EXPECT_FALSE(UsesOverlayScrollbars(FontPlatform::kWindows));
  EXPECT_FALSE(UsesOverlayScrollbars(FontPlatform::kLinux));
  // An out-of-range value (a future platform) gets classic scrollbars.
  EXPECT_FALSE(UsesOverlayScrollbars(static_cast<FontPlatform>(0x7f)));
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
