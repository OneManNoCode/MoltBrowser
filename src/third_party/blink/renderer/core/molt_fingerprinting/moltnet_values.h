// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the fixed values MoltNet windows
// report for touches, the screen orientation and form controls, and the few
// rules that derive them. Pure and std-only (tested standalone, like
// farbling.h); the Blink side is moltnet_uniformity.h.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_VALUES_H_

#include <array>
#include <cstdint>

namespace blink::molt_fingerprinting::moltnet {

// --- Screen orientation ------------------------------------------------------

enum class Orientation { kPortraitPrimary, kLandscapePrimary };

// screen.orientation.type of an unrotated display of `width` x `height`, by
// the rule the browser applies to real desktop displays
// (display::DisplayUtil::GetOrientationTypeForDesktop): portrait when the
// height is at least the width, so a square screen is portrait.
Orientation OrientationOf(int width, int height);

// screen.orientation.angle: the uniform screen is never rotated.
inline constexpr uint16_t kOrientationAngle = 0;

// --- Touch -------------------------------------------------------------------

// Touch.radiusX / radiusY / rotationAngle / force: the values the Touch
// Events spec gives when the device reports none.
inline constexpr float kTouchRadius = 0.0f;
inline constexpr float kTouchRotationAngle = 0.0f;
inline constexpr float kTouchForce = 0.0f;

// PointerEvent with pointerType "touch": the contact is 1 x 1 (the default
// for "no contact geometry"), tilt, twist and tangential pressure are 0, and
// altitude / azimuth are those of a pointer without tilt.
inline constexpr double kPointerContactSize = 1.0;
inline constexpr int32_t kPointerTilt = 0;
inline constexpr int32_t kPointerTwist = 0;
inline constexpr float kPointerTangentialPressure = 0.0f;
inline constexpr double kPointerAltitudeAngle = 1.5707963267948966;  // pi / 2
inline constexpr double kPointerAzimuthAngle = 0.0;

// PointerEvent.pressure for hardware without pressure, per the Pointer Events
// spec: 0.5 while a button (here: a finger) is down, 0 otherwise.
float PointerPressure(bool buttons_down);

// --- Form-control locale -----------------------------------------------------
//
// What Blink's ICU-based Locale (platform/text/locale_icu.cc, the Linux
// implementation) computes for en-US at the pinned ICU: the patterns, labels
// and number symbols that date, time, month, week and number inputs and
// validation messages use. UTF-8. Hard-coded, so that MoltNet windows on
// macOS (LocaleMac) and Windows (LocaleWin), whose en-US data differ, and
// future ICU updates all give the same answers.

inline constexpr char kFormLocale[] = "en-US";

// The period separator is U+202F NARROW NO-BREAK SPACE, as in CLDR 42 and
// later (and in locale_icu_test.cc at the pin).
inline constexpr char kDateFormat[] = "M/d/yy";
inline constexpr char kMonthFormat[] = "MMMM yyyy";
inline constexpr char kShortMonthFormat[] = "MMM yyyy";
inline constexpr char kTimeFormat[] = "h:mm:ss\xE2\x80\xAF" "a";
inline constexpr char kShortTimeFormat[] = "h:mm\xE2\x80\xAF" "a";
inline constexpr char kDateTimeFormatWithSeconds[] =
    "M/d/yy, h:mm:ss\xE2\x80\xAF" "a";
inline constexpr char kDateTimeFormatWithoutSeconds[] =
    "M/d/yy, h:mm\xE2\x80\xAF" "a";

// January first. In English the stand-alone forms are the same.
inline constexpr std::array<const char*, 12> kMonthLabels = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
inline constexpr std::array<const char*, 12> kShortMonthLabels = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
// Sunday first (ICU's narrow weekday names, as LocaleIcu uses).
inline constexpr std::array<const char*, 7> kWeekDayShortLabels = {
    "S", "M", "T", "W", "T", "F", "S"};
inline constexpr std::array<const char*, 2> kAmPmLabels = {"AM", "PM"};
// 0 is Sunday.
inline constexpr unsigned kFirstDayOfWeek = 0;

// Digits 0-9 are ASCII.
inline constexpr char kDecimalSeparator[] = ".";
inline constexpr char kGroupSeparator[] = ",";
inline constexpr char kPositivePrefix[] = "";
inline constexpr char kPositiveSuffix[] = "";
inline constexpr char kNegativePrefix[] = "-";
inline constexpr char kNegativeSuffix[] = "";

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_VALUES_H_
