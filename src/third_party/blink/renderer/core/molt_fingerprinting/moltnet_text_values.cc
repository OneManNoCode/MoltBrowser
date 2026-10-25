// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_text_values.h"

#include <cmath>

namespace blink::molt_fingerprinting::moltnet {

namespace {

// Beyond 2^42 grid steps nothing a page lays out is left to round, and
// settling (x 1024) must stay below 2^52, where doubles stop counting whole
// numbers exactly.
constexpr double kLargestStep = 4398046511104.0;  // 2^42.

bool ValidGrid(double grid) {
  return grid > 0 && std::isfinite(grid);
}

// `value` in grid steps, settled on the 1/kTextGridSettle lattice, or false
// when it can't or needn't be rounded.
bool Steps(double value, double grid, double& steps) {
  if (!ValidGrid(grid) || !std::isfinite(value)) {
    return false;
  }
  steps = value / grid;
  if (!(std::fabs(steps) < kLargestStep)) {
    return false;
  }
  steps = std::round(steps * kTextGridSettle) / kTextGridSettle;
  return true;
}

double AlignFraction(TextAlignPoint align) {
  switch (align) {
    case TextAlignPoint::kLeft:
      return 0;
    case TextAlignPoint::kCenter:
      return 0.5;
    case TextAlignPoint::kRight:
      return 1;
  }
  return 0;
}

}  // namespace

// (The "+ 0.0" below turns -0 into +0: a page can tell them apart with
// Object.is() or 1 / x, and a length just below or above 0 must read the
// same.)

double TextMetricsGrid(double font_size) {
  if (!(font_size > 0) || !std::isfinite(font_size)) {
    return kTextGridMin;
  }
  const double limit = font_size / kTextGridEmDivisor;
  double grid = kTextGridMin;
  while (grid * 2 <= limit && grid < kTextGridMax) {
    grid *= 2;
  }
  return grid;
}

double RoundToGrid(double value, double grid) {
  double steps = 0;
  if (!Steps(value, grid, steps)) {
    return value;
  }
  return std::round(steps) * grid + 0.0;
}

double FloorToGrid(double value, double grid) {
  double steps = 0;
  if (!Steps(value, grid, steps)) {
    return value;
  }
  return std::floor(steps) * grid + 0.0;
}

double CeilToGrid(double value, double grid) {
  double steps = 0;
  if (!Steps(value, grid, steps)) {
    return value;
  }
  return std::ceil(steps) * grid + 0.0;
}

void RoundTextMetrics(double grid,
                      TextMetricsValues& values,
                      TextMetricsFrame& frame) {
  if (!ValidGrid(grid)) {
    return;
  }
  TextMetricsValues& v = values;
  const double dx = frame.align_dx;
  const double baseline = frame.baseline;

  // The lengths Update() derived the attributes from. x from the text's
  // left end; y up from the alphabetic baseline (Update() works with the
  // glyph bounds y down, and with the font's descent as a positive
  // distance below the baseline).
  const double ink_left = dx - v.actual_bounding_box_left;
  const double ink_right = v.actual_bounding_box_right + dx;
  const double ink_top = v.actual_bounding_box_ascent + baseline;
  const double ink_bottom = baseline - v.actual_bounding_box_descent;
  const double font_top = v.font_bounding_box_ascent + baseline;
  const double font_bottom = baseline - v.font_bounding_box_descent;
  const double em_top = v.em_height_ascent + baseline;
  const double em_bottom = baseline - v.em_height_descent;
  const double alphabetic = v.alphabetic_baseline + baseline;
  const double hanging = v.hanging_baseline + baseline;
  const double ideographic = v.ideographic_baseline + baseline;

  // Rounded. The ink box outwards, so it still holds every glyph.
  const double width = RoundToGrid(v.width, grid);
  const double align_dx = AlignFraction(frame.align) * width;
  const double baseline_q = RoundToGrid(baseline, grid);
  const double ink_left_q = FloorToGrid(ink_left, grid);
  const double ink_right_q = CeilToGrid(ink_right, grid);
  const double ink_top_q = CeilToGrid(ink_top, grid);
  const double ink_bottom_q = FloorToGrid(ink_bottom, grid);

  // Derived as Update() derives them.
  v.width = width;
  v.actual_bounding_box_left = align_dx - ink_left_q;
  v.actual_bounding_box_right = ink_right_q - align_dx;
  v.actual_bounding_box_ascent = ink_top_q - baseline_q;
  v.actual_bounding_box_descent = baseline_q - ink_bottom_q;
  v.font_bounding_box_ascent = RoundToGrid(font_top, grid) - baseline_q;
  v.font_bounding_box_descent = baseline_q - RoundToGrid(font_bottom, grid);
  v.em_height_ascent = RoundToGrid(em_top, grid) - baseline_q;
  v.em_height_descent = baseline_q - RoundToGrid(em_bottom, grid);
  v.alphabetic_baseline = RoundToGrid(alphabetic, grid) - baseline_q;
  v.hanging_baseline = RoundToGrid(hanging, grid) - baseline_q;
  v.ideographic_baseline = RoundToGrid(ideographic, grid) - baseline_q;

  frame.align_dx = align_dx;
  frame.baseline = baseline_q;
}

void RoundSpan(double grid, double& start, double& end) {
  start = RoundToGrid(start, grid);
  end = RoundToGrid(end, grid);
}

InkBox RoundInkBox(double grid, const InkBox& box) {
  InkBox result;
  result.left = FloorToGrid(box.left, grid);
  result.top = FloorToGrid(box.top, grid);
  result.right = CeilToGrid(box.right, grid);
  result.bottom = CeilToGrid(box.bottom, grid);
  return result;
}

const char* UniformEmojiFamily(FontPlatform platform) {
  switch (platform) {
    case FontPlatform::kLinux:
    // The bundled set's emoji font has the same family name.
    case FontPlatform::kMoltNetBundled:
      return "Noto Color Emoji";
    case FontPlatform::kWindows:
    case FontPlatform::kMac:
    case FontPlatform::kAndroid:
      return nullptr;
  }
  return nullptr;
}

}  // namespace blink::molt_fingerprinting::moltnet
