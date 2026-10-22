// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_text_values.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// --- A model of TextMetrics::Update() ---------------------------------------
//
// What a font and a text give Update() (text_metrics.cc at 152): the
// advance, the glyphs' ink box (x from the text's left end, y down from the
// alphabetic baseline), the font's rounded ascent and descent, its
// normalized sTypo ascent and descent, and the baseline-table entries it may
// have.
struct Measured {
  double advance = 0;
  double ink_x = 0;
  double ink_y = 0;  // Negative: above the baseline.
  double ink_right = 0;
  double ink_bottom = 0;
  double ascent = 0;
  double descent = 0;
  double typo_ascent = 0;
  double typo_descent = 0;
  std::optional<double> table_alphabetic;
  std::optional<double> table_hanging;
  std::optional<double> table_ideographic;
};

enum class Align { kLeft, kCenter, kRight };
enum class Baseline {
  kTop,
  kHanging,
  kMiddle,
  kAlphabetic,
  kIdeographic,
  kBottom
};
constexpr std::array<Align, 3> kAligns = {Align::kLeft, Align::kCenter,
                                          Align::kRight};
constexpr std::array<Baseline, 6> kBaselines = {
    Baseline::kTop,        Baseline::kHanging,     Baseline::kMiddle,
    Baseline::kAlphabetic, Baseline::kIdeographic, Baseline::kBottom};

TextAlignPoint PointOf(Align align) {
  switch (align) {
    case Align::kLeft:
      return TextAlignPoint::kLeft;
    case Align::kCenter:
      return TextAlignPoint::kCenter;
    case Align::kRight:
      return TextAlignPoint::kRight;
  }
  return TextAlignPoint::kLeft;
}

double AlignShare(Align align) {
  return align == Align::kCenter ? 0.5 : align == Align::kRight ? 1.0 : 0.0;
}

// TextMetrics::GetFontBaseline().
double FontBaseline(const Measured& m, Baseline baseline) {
  switch (baseline) {
    case Baseline::kTop:
      return m.typo_ascent;
    case Baseline::kHanging:
      return m.table_hanging ? *m.table_hanging : m.ascent * 80 / 100.0;
    case Baseline::kIdeographic:
      return m.table_ideographic ? *m.table_ideographic : -m.descent;
    case Baseline::kBottom:
      return -m.typo_descent;
    case Baseline::kMiddle:
      return (m.typo_ascent - m.typo_descent) / 2;
    case Baseline::kAlphabetic:
      return m.table_alphabetic ? *m.table_alphabetic : 0;
  }
  return 0;
}

// TextMetrics::Update(): the attributes and the frame.
struct Measurement {
  TextMetricsValues values;
  TextMetricsFrame frame;
};
Measurement Update(const Measured& m, Align align, Baseline baseline) {
  Measurement out;
  TextMetricsValues& v = out.values;
  const double dx = AlignShare(align) * m.advance;
  const double b = FontBaseline(m, baseline);
  v.width = m.advance;
  v.actual_bounding_box_left = -m.ink_x + dx;
  v.actual_bounding_box_right = m.ink_right - dx;
  v.font_bounding_box_ascent = m.ascent - b;
  v.font_bounding_box_descent = m.descent + b;
  v.actual_bounding_box_ascent = -m.ink_y - b;
  v.actual_bounding_box_descent = m.ink_bottom + b;
  v.em_height_ascent = m.typo_ascent - b;
  v.em_height_descent = m.typo_descent + b;
  v.alphabetic_baseline =
      (m.table_alphabetic ? *m.table_alphabetic : 0) - b;
  v.hanging_baseline =
      (m.table_hanging ? *m.table_hanging : m.ascent * 80 / 100.0) - b;
  v.ideographic_baseline =
      (m.table_ideographic ? *m.table_ideographic : -m.descent) - b;
  out.frame.align = PointOf(align);
  out.frame.align_dx = dx;
  out.frame.baseline = b;
  return out;
}

// What MoltNet must report: Update() run on the rounded lengths, i.e. the
// advance, the font's lengths, the baselines and the chosen baseline's
// offset to the nearest grid point, the ink box outwards.
Measurement Expected(const Measured& m,
                     Align align,
                     Baseline baseline,
                     double g) {
  Measurement out;
  TextMetricsValues& v = out.values;
  const double width = RoundToGrid(m.advance, g);
  const double dx = AlignShare(align) * width;
  const double b = RoundToGrid(FontBaseline(m, baseline), g);
  const double hanging =
      RoundToGrid(m.table_hanging ? *m.table_hanging : m.ascent * 80 / 100.0,
                  g);
  const double ideographic =
      RoundToGrid(m.table_ideographic ? *m.table_ideographic : -m.descent, g);
  const double alphabetic =
      RoundToGrid(m.table_alphabetic ? *m.table_alphabetic : 0, g);
  v.width = width;
  v.actual_bounding_box_left = dx - FloorToGrid(m.ink_x, g);
  v.actual_bounding_box_right = CeilToGrid(m.ink_right, g) - dx;
  v.font_bounding_box_ascent = RoundToGrid(m.ascent, g) - b;
  v.font_bounding_box_descent = RoundToGrid(m.descent, g) + b;
  v.actual_bounding_box_ascent = -FloorToGrid(m.ink_y, g) - b;
  v.actual_bounding_box_descent = CeilToGrid(m.ink_bottom, g) + b;
  v.em_height_ascent = RoundToGrid(m.typo_ascent, g) - b;
  v.em_height_descent = RoundToGrid(m.typo_descent, g) + b;
  v.alphabetic_baseline = alphabetic - b;
  v.hanging_baseline = hanging - b;
  v.ideographic_baseline = ideographic - b;
  out.frame.align = PointOf(align);
  out.frame.align_dx = dx;
  out.frame.baseline = b;
  return out;
}

std::array<double, 12> Fields(const TextMetricsValues& v) {
  return {v.width,
          v.actual_bounding_box_left,
          v.actual_bounding_box_right,
          v.font_bounding_box_ascent,
          v.font_bounding_box_descent,
          v.actual_bounding_box_ascent,
          v.actual_bounding_box_descent,
          v.em_height_ascent,
          v.em_height_descent,
          v.alphabetic_baseline,
          v.hanging_baseline,
          v.ideographic_baseline};
}

constexpr std::array<std::string_view, 12> kFieldNames = {
    "width",
    "actualBoundingBoxLeft",
    "actualBoundingBoxRight",
    "fontBoundingBoxAscent",
    "fontBoundingBoxDescent",
    "actualBoundingBoxAscent",
    "actualBoundingBoxDescent",
    "emHeightAscent",
    "emHeightDescent",
    "alphabeticBaseline",
    "hangingBaseline",
    "ideographicBaseline"};

// Deterministic xorshift for test data.
class TestRng {
 public:
  explicit TestRng(uint64_t seed) : state_(seed ? seed : 1) {}
  uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  // A length in [lo, hi) on a 1/2^20 lattice, so the model's sums are exact
  // in double and the recovered lengths are the measured ones.
  double Length(double lo, double hi) {
    const double unit = 1.0 / 1048576;
    const auto steps = static_cast<uint64_t>((hi - lo) / unit);
    return lo + static_cast<double>(Next() % steps) * unit;
  }

 private:
  uint64_t state_;
};

// A plausible font and text at `size` CSS pixels.
Measured RandomMeasured(TestRng& rng, double size) {
  Measured m;
  m.advance = rng.Length(0, 40 * size);
  m.ink_x = rng.Length(-0.2 * size, 0.2 * size);
  m.ink_right = m.advance + rng.Length(-0.3 * size, 0.3 * size);
  m.ink_y = -rng.Length(0, 1.1 * size);
  m.ink_bottom = rng.Length(-0.1 * size, 0.4 * size);
  // Blink rounds ascent and descent to whole pixels (FontMetrics), except
  // for tiny fonts; the typo metrics are LayoutUnits (1/64).
  m.ascent = std::round(rng.Length(0.7 * size, 1.1 * size));
  m.descent = std::round(rng.Length(0.1 * size, 0.4 * size));
  m.typo_ascent = std::round(rng.Length(0.6 * size, size) * 64) / 64;
  m.typo_descent = std::round(rng.Length(0.1 * size, 0.4 * size) * 64) / 64;
  if (rng.Next() % 3 == 0) {
    m.table_alphabetic = rng.Length(-0.05 * size, 0.05 * size);
    m.table_hanging = rng.Length(0.5 * size, 0.9 * size);
    m.table_ideographic = -rng.Length(0.05 * size, 0.3 * size);
  }
  return m;
}

// --- The grid ---------------------------------------------------------------

TEST(MoltNetTextValuesTest, GridIsWholePixelsForTextUpTo127Px) {
  for (double size : {0.5, 1.0, 9.0, 12.0, 16.0, 32.0, 63.9, 64.0, 100.0,
                      127.99}) {
    EXPECT_EQ(TextMetricsGrid(size), 1.0) << size;
  }
  EXPECT_EQ(TextMetricsGrid(128), 2.0);
  EXPECT_EQ(TextMetricsGrid(255.9), 2.0);
  EXPECT_EQ(TextMetricsGrid(256), 4.0);
  EXPECT_EQ(TextMetricsGrid(1000), 8.0);
  EXPECT_EQ(TextMetricsGrid(1024), 16.0);
  // Not a size: the smallest grid.
  for (double size : {0.0, -16.0, kNaN, kInf, -kInf}) {
    EXPECT_EQ(TextMetricsGrid(size), kTextGridMin) << size;
  }
  EXPECT_EQ(TextMetricsGrid(1e300), kTextGridMax);
}

// Above 127px the grid is a 64th to a 128th of the font size, a power of
// two, and never shrinks as the size grows: drawing larger doesn't buy
// precision.
TEST(MoltNetTextValuesTest, GridFollowsTheFontSize) {
  double previous = 0;
  for (double size = 1; size < 70000; size *= 1.013) {
    const double grid = TextMetricsGrid(size);
    EXPECT_GE(grid, previous) << size;
    previous = grid;
    int exponent = 0;
    EXPECT_EQ(std::frexp(grid, &exponent), 0.5) << size;  // A power of two.
    if (size >= 128) {
      EXPECT_LE(grid, size / 64) << size;
      EXPECT_GT(grid, size / 128) << size;
    }
  }
}

TEST(MoltNetTextValuesTest, Rounding) {
  EXPECT_EQ(RoundToGrid(2.4, 1), 2);
  EXPECT_EQ(RoundToGrid(2.5, 1), 3);
  EXPECT_EQ(RoundToGrid(-2.5, 1), -3);  // Symmetric.
  EXPECT_EQ(RoundToGrid(-2.4, 1), -2);
  // No -0, which a page could tell from +0.
  EXPECT_FALSE(std::signbit(RoundToGrid(-0.3, 1)));
  EXPECT_FALSE(std::signbit(CeilToGrid(-0.3, 1)));
  EXPECT_FALSE(std::signbit(FloorToGrid(-0.0, 1)));
  EXPECT_EQ(RoundToGrid(5.9, 4), 4);
  EXPECT_EQ(RoundToGrid(6.0, 4), 8);
  EXPECT_EQ(FloorToGrid(11.99, 1), 11);
  EXPECT_EQ(FloorToGrid(-0.3, 1), -1);
  EXPECT_EQ(CeilToGrid(12.01, 1), 13);
  EXPECT_EQ(CeilToGrid(-0.3, 1), 0);
  // Float noise at a grid point doesn't move an edge a whole step.
  EXPECT_EQ(CeilToGrid(12.0000005, 1), 12);
  EXPECT_EQ(FloorToGrid(11.9999995, 1), 12);
  EXPECT_EQ(CeilToGrid(16.0 + 1.0 / 2048, 16), 16);
  EXPECT_EQ(CeilToGrid(16.0 + 1.0 / 16, 16), 32);
  // Grid points stay.
  for (double v : {-3.0, 0.0, 7.0, 1024.0}) {
    EXPECT_EQ(RoundToGrid(v, 1), v);
    EXPECT_EQ(FloorToGrid(v, 1), v);
    EXPECT_EQ(CeilToGrid(v, 1), v);
  }
  // Nothing to round: invalid grids, values that aren't finite, values far
  // beyond any grid step.
  for (double grid : {0.0, -1.0, kNaN, kInf}) {
    EXPECT_EQ(RoundToGrid(2.4, grid), 2.4);
    EXPECT_EQ(FloorToGrid(2.4, grid), 2.4);
    EXPECT_EQ(CeilToGrid(2.4, grid), 2.4);
  }
  EXPECT_TRUE(std::isnan(RoundToGrid(kNaN, 1)));
  EXPECT_EQ(RoundToGrid(kInf, 1), kInf);
  EXPECT_EQ(FloorToGrid(-kInf, 1), -kInf);
  EXPECT_EQ(CeilToGrid(1e300, 1), 1e300);
}

// --- measureText() ----------------------------------------------------------

// The heart of it: rounding what Update() computed gives exactly what
// Update() computes from the rounded lengths, for every alignment and
// baseline, so every attribute is consistent with every other.
TEST(MoltNetTextValuesTest, RoundedMetricsAreUpdateOfRoundedLengths) {
  TestRng rng(0x7e47);
  for (double size : {7.0, 16.0, 33.0, 100.0, 300.0, 2048.0}) {
    const double grid = TextMetricsGrid(size);
    for (int i = 0; i < 400; ++i) {
      const Measured m = RandomMeasured(rng, size);
      for (Align align : kAligns) {
        for (Baseline baseline : kBaselines) {
          Measurement got = Update(m, align, baseline);
          RoundTextMetrics(grid, got.values, got.frame);
          const Measurement want = Expected(m, align, baseline, grid);
          const auto got_fields = Fields(got.values);
          const auto want_fields = Fields(want.values);
          for (size_t f = 0; f < got_fields.size(); ++f) {
            ASSERT_EQ(got_fields[f], want_fields[f])
                << kFieldNames[f] << " size " << size << " case " << i
                << " align " << static_cast<int>(align) << " baseline "
                << static_cast<int>(baseline);
          }
          ASSERT_EQ(got.frame.align_dx, want.frame.align_dx);
          ASSERT_EQ(got.frame.baseline, want.frame.baseline);
          ASSERT_EQ(got.frame.align, want.frame.align);
        }
      }
    }
  }
}

// Every attribute is on the grid, or on its half where the alignment point
// is the centre (half the rounded width).
TEST(MoltNetTextValuesTest, AttributesAreOnTheGrid) {
  TestRng rng(0xa11);
  for (double size : {12.0, 16.0, 200.0}) {
    const double grid = TextMetricsGrid(size);
    for (int i = 0; i < 300; ++i) {
      const Measured m = RandomMeasured(rng, size);
      for (Align align : kAligns) {
        for (Baseline baseline : kBaselines) {
          Measurement got = Update(m, align, baseline);
          RoundTextMetrics(grid, got.values, got.frame);
          const double step =
              (align == Align::kCenter) ? grid / 2 : grid;
          for (double value : Fields(got.values)) {
            ASSERT_EQ(std::fmod(value, step), 0.0)
                << value << " step " << step;
            ASSERT_FALSE(value == 0 && std::signbit(value));
          }
          if (align == Align::kLeft) {
            for (double value : Fields(got.values)) {
              ASSERT_EQ(std::fmod(value, grid), 0.0) << value;
            }
          }
        }
      }
    }
  }
}

// What a page can read never depends on more than the rounded lengths:
// measurements that round to the same lengths read the same under every
// alignment and baseline (rounding the attributes instead would let a page
// combine alignments to recover sub-grid digits).
TEST(MoltNetTextValuesTest, MachinesThatDifferBelowTheGridReadTheSame) {
  TestRng rng(0x5ead);
  int same = 0;
  for (int i = 0; i < 500; ++i) {
    Measured a = RandomMeasured(rng, 16);
    // Keep every length well inside its grid cell, then perturb by less
    // than the room left: hinting and rasteriser jitter on another machine.
    auto inside = [](double v) { return std::floor(v) + 0.3; };
    a.advance = inside(a.advance);
    a.ink_x = inside(a.ink_x);
    a.ink_y = inside(a.ink_y);
    a.ink_right = inside(a.ink_right);
    a.ink_bottom = inside(a.ink_bottom);
    a.typo_ascent = inside(a.typo_ascent);
    a.typo_descent = inside(a.typo_descent);
    a.table_alphabetic.reset();
    a.table_hanging.reset();
    a.table_ideographic.reset();
    Measured b = a;
    b.advance += 0.15;
    b.ink_x -= 0.2;
    b.ink_y += 0.12;
    b.ink_right += 0.18;
    b.ink_bottom -= 0.25;
    b.typo_ascent += 0.05;
    for (Align align : kAligns) {
      for (Baseline baseline : kBaselines) {
        if (baseline == Baseline::kMiddle || baseline == Baseline::kHanging) {
          // Their offsets are halves and 0.8ths of other lengths, which
          // land anywhere in their cells.
          continue;
        }
        Measurement ma = Update(a, align, baseline);
        Measurement mb = Update(b, align, baseline);
        ASSERT_NE(Fields(ma.values), Fields(mb.values));
        RoundTextMetrics(1, ma.values, ma.frame);
        RoundTextMetrics(1, mb.values, mb.frame);
        ASSERT_EQ(Fields(ma.values), Fields(mb.values)) << i;
        ++same;
      }
    }
  }
  EXPECT_EQ(same, 500 * 3 * 4);
}

// Measuring at a larger size doesn't recover what the grid hides: a
// difference of a 256th of an em between two fonts (a font update) is
// hidden at 16px (sub-pixel) and stays hidden at 1000px (the grid grows).
TEST(MoltNetTextValuesTest, LargerTextDoesNotRevealMore) {
  int hidden_small = 0;
  int hidden_large = 0;
  const int kCases = 1000;
  for (int i = 0; i < kCases; ++i) {
    const double em_advance = 7.0 + i * 0.0137;  // Text of ~7 ems.
    const double em_delta = 1.0 / 256;
    for (double size : {16.0, 1000.0}) {
      const double grid = TextMetricsGrid(size);
      const bool same = RoundToGrid(em_advance * size, grid) ==
                        RoundToGrid((em_advance + em_delta) * size, grid);
      (size == 16.0 ? hidden_small : hidden_large) += same ? 1 : 0;
    }
  }
  // 1/256 em is 1/16 px at 16px and about half of an 8px grid at 1000px: the
  // grid hides it most of the time in both.
  EXPECT_GT(hidden_small, kCases * 9 / 10);
  EXPECT_GT(hidden_large, kCases * 4 / 10);
  // Without a growing grid, 1000px text would show it every time.
  int shown_with_fixed_grid = 0;
  for (int i = 0; i < kCases; ++i) {
    const double em_advance = 7.0 + i * 0.0137;
    shown_with_fixed_grid +=
        RoundToGrid(em_advance * 1000, 1) !=
                RoundToGrid((em_advance + 1.0 / 256) * 1000, 1)
            ? 1
            : 0;
  }
  EXPECT_EQ(shown_with_fixed_grid, kCases);
}

// The ink box never shrinks: pages that size a canvas to fit the text from
// actualBoundingBox* don't clip glyphs.
TEST(MoltNetTextValuesTest, InkBoxStillHoldsTheGlyphs) {
  TestRng rng(0xb0c5);
  for (double size : {10.0, 16.0, 500.0}) {
    const double grid = TextMetricsGrid(size);
    for (int i = 0; i < 500; ++i) {
      const Measured m = RandomMeasured(rng, size);
      for (Align align : kAligns) {
        for (Baseline baseline : kBaselines) {
          const Measurement exact = Update(m, align, baseline);
          Measurement got = exact;
          RoundTextMetrics(grid, got.values, got.frame);
          // The box around the alignment point and the alphabetic baseline,
          // as a page computes it: x from -left to right, y from -ascent to
          // descent relative to the baseline it measured with.
          const double ex_left = exact.frame.align_dx -
                                 exact.values.actual_bounding_box_left;
          const double got_left =
              got.frame.align_dx - got.values.actual_bounding_box_left;
          // (Settling lets an edge in by at most half a lattice step.)
          const double slack = grid / kTextGridSettle / 2;
          EXPECT_LE(got_left, ex_left + slack);
          EXPECT_GE(
              got.frame.align_dx + got.values.actual_bounding_box_right + slack,
              exact.frame.align_dx + exact.values.actual_bounding_box_right);
          EXPECT_GE(
              got.values.actual_bounding_box_ascent + got.frame.baseline +
                  slack,
              exact.values.actual_bounding_box_ascent + exact.frame.baseline);
          EXPECT_GE(
              got.values.actual_bounding_box_descent - got.frame.baseline +
                  slack,
              exact.values.actual_bounding_box_descent - exact.frame.baseline);
          // And by less than a grid step on each side.
          EXPECT_LT(ex_left - got_left, grid + 1e-9);
        }
      }
    }
  }
}

// Relations pages rely on hold exactly.
TEST(MoltNetTextValuesTest, ConsistentAcrossAlignmentsAndBaselines) {
  TestRng rng(0xc0de);
  for (int i = 0; i < 300; ++i) {
    const Measured m = RandomMeasured(rng, 16);
    std::array<std::array<Measurement, 6>, 3> all;
    for (size_t a = 0; a < kAligns.size(); ++a) {
      for (size_t b = 0; b < kBaselines.size(); ++b) {
        all[a][b] = Update(m, kAligns[a], kBaselines[b]);
        RoundTextMetrics(1, all[a][b].values, all[a][b].frame);
      }
    }
    for (size_t b = 0; b < kBaselines.size(); ++b) {
      const TextMetricsValues& left = all[0][b].values;
      const TextMetricsValues& center = all[1][b].values;
      const TextMetricsValues& right = all[2][b].values;
      // The ink width doesn't depend on the alignment ...
      EXPECT_EQ(left.actual_bounding_box_left + left.actual_bounding_box_right,
                center.actual_bounding_box_left +
                    center.actual_bounding_box_right);
      EXPECT_EQ(left.actual_bounding_box_left + left.actual_bounding_box_right,
                right.actual_bounding_box_left +
                    right.actual_bounding_box_right);
      // ... and the alignment moves the box by half or all of the width.
      EXPECT_EQ(center.actual_bounding_box_left - left.actual_bounding_box_left,
                left.width / 2);
      EXPECT_EQ(right.actual_bounding_box_left - left.actual_bounding_box_left,
                left.width);
    }
    for (size_t b = 0; b < kBaselines.size(); ++b) {
      const TextMetricsValues& v = all[0][b].values;
      const TextMetricsValues& alphabetic = all[0][3].values;
      // Heights don't depend on the baseline ...
      EXPECT_EQ(v.font_bounding_box_ascent + v.font_bounding_box_descent,
                alphabetic.font_bounding_box_ascent +
                    alphabetic.font_bounding_box_descent);
      EXPECT_EQ(v.actual_bounding_box_ascent + v.actual_bounding_box_descent,
                alphabetic.actual_bounding_box_ascent +
                    alphabetic.actual_bounding_box_descent);
      EXPECT_EQ(v.em_height_ascent + v.em_height_descent,
                alphabetic.em_height_ascent + alphabetic.em_height_descent);
      // ... and moving the baseline moves everything by the same amount.
      const double shift =
          alphabetic.alphabetic_baseline - v.alphabetic_baseline;
      EXPECT_EQ(v.font_bounding_box_ascent + shift,
                alphabetic.font_bounding_box_ascent);
      EXPECT_EQ(v.hanging_baseline + shift, alphabetic.hanging_baseline);
      EXPECT_EQ(v.actual_bounding_box_ascent + shift,
                alphabetic.actual_bounding_box_ascent);
    }
    // A baseline measured against itself is 0, as upstream.
    EXPECT_EQ(all[0][0].values.em_height_ascent, 0);    // top
    EXPECT_EQ(all[0][5].values.em_height_descent, 0);   // bottom
    EXPECT_EQ(all[0][3].values.alphabetic_baseline, 0);
    if (!m.table_hanging) {
      EXPECT_EQ(all[0][1].values.hanging_baseline, 0);
    }
    if (!m.table_ideographic) {
      EXPECT_EQ(all[0][4].values.ideographic_baseline, 0);
    }
  }
}

TEST(MoltNetTextValuesTest, RoundingIsIdempotent) {
  TestRng rng(0x1de);
  for (double size : {16.0, 400.0}) {
    const double grid = TextMetricsGrid(size);
    for (int i = 0; i < 300; ++i) {
      const Measured m = RandomMeasured(rng, size);
      for (Align align : kAligns) {
        for (Baseline baseline : kBaselines) {
          Measurement once = Update(m, align, baseline);
          RoundTextMetrics(grid, once.values, once.frame);
          Measurement twice = once;
          RoundTextMetrics(grid, twice.values, twice.frame);
          ASSERT_EQ(Fields(once.values), Fields(twice.values));
          ASSERT_EQ(once.frame.align_dx, twice.frame.align_dx);
          ASSERT_EQ(once.frame.baseline, twice.frame.baseline);
        }
      }
    }
  }
}

// The values TextMetrics really holds are floats turned into doubles, with
// float rounding in their sums; that noise doesn't move an edge a step.
TEST(MoltNetTextValuesTest, FloatNoiseDoesNotMoveEdges) {
  Measured m;
  m.advance = 120.0f;
  m.ink_x = 1.0f;
  m.ink_y = -12.0f;
  m.ink_right = 119.0f;
  m.ink_bottom = 3.0f;
  m.ascent = 15;
  m.descent = 4;
  m.typo_ascent = 12.40625;
  m.typo_descent = 3.59375;
  Measurement got = Update(m, Align::kCenter, Baseline::kMiddle);
  // As Update() stores them: float sums.
  TextMetricsValues& v = got.values;
  v.actual_bounding_box_left = static_cast<float>(-1.0f + 60.0f) + 1e-6;
  v.actual_bounding_box_right = static_cast<float>(119.0f - 60.0f) - 1e-6;
  RoundTextMetrics(1, got.values, got.frame);
  EXPECT_EQ(got.values.actual_bounding_box_left, 59);
  EXPECT_EQ(got.values.actual_bounding_box_right, 59);
  EXPECT_EQ(got.values.width, 120);
}

TEST(MoltNetTextValuesTest, EmptyTextStaysEmpty) {
  Measurement got = Update(Measured{}, Align::kCenter, Baseline::kTop);
  RoundTextMetrics(1, got.values, got.frame);
  EXPECT_EQ(got.values.width, 0);
  EXPECT_EQ(got.values.actual_bounding_box_left, 0);
  EXPECT_EQ(got.values.actual_bounding_box_right, 0);
  EXPECT_EQ(got.frame.align_dx, 0);
}

TEST(MoltNetTextValuesTest, InvalidGridChangesNothing) {
  TestRng rng(0xbad);
  const Measured m = RandomMeasured(rng, 16);
  const Measurement exact = Update(m, Align::kCenter, Baseline::kMiddle);
  for (double grid : {0.0, -1.0, kNaN, kInf}) {
    Measurement got = exact;
    RoundTextMetrics(grid, got.values, got.frame);
    EXPECT_EQ(Fields(got.values), Fields(exact.values));
    EXPECT_EQ(got.frame.align_dx, exact.frame.align_dx);
    EXPECT_EQ(got.frame.baseline, exact.frame.baseline);
  }
}

// --- The ExtendedTextMetrics methods ----------------------------------------

TEST(MoltNetTextValuesTest, SpansAgreeWithTheWidth) {
  TestRng rng(0x5a);
  for (int i = 0; i < 1000; ++i) {
    const double grid = i % 2 ? 1 : 8;
    const double width = rng.Length(0, 2000);
    const double cut = rng.Length(0, width);
    // The whole text's span is the rounded width.
    double start = 0;
    double end = width;
    RoundSpan(grid, start, end);
    EXPECT_EQ(start, 0);
    EXPECT_EQ(end, RoundToGrid(width, grid));
    // Spans that touch still touch.
    double a_start = 0;
    double a_end = cut;
    double b_start = cut;
    double b_end = width;
    RoundSpan(grid, a_start, a_end);
    RoundSpan(grid, b_start, b_end);
    EXPECT_EQ(a_end, b_start);
    EXPECT_EQ(b_end - a_start, end - start);
  }
}

TEST(MoltNetTextValuesTest, InkBoxRoundsOutwards) {
  const InkBox box = RoundInkBox(1, {0.3, -11.7, 40.2, 3.1});
  EXPECT_EQ(box.left, 0);
  EXPECT_EQ(box.top, -12);
  EXPECT_EQ(box.right, 41);
  EXPECT_EQ(box.bottom, 4);
  const InkBox big = RoundInkBox(16, {-1, -700, 900, 10});
  EXPECT_EQ(big.left, -16);
  EXPECT_EQ(big.top, -704);
  EXPECT_EQ(big.right, 912);
  EXPECT_EQ(big.bottom, 16);
  // As measureText()'s box: the same lengths round to the same edges.
  Measured m;
  m.advance = 40;
  m.ink_x = 0.3;
  m.ink_y = -11.7;
  m.ink_right = 40.2;
  m.ink_bottom = 3.1;
  Measurement got = Update(m, Align::kLeft, Baseline::kAlphabetic);
  RoundTextMetrics(1, got.values, got.frame);
  EXPECT_EQ(-got.values.actual_bounding_box_left, box.left);
  EXPECT_EQ(-got.values.actual_bounding_box_ascent, box.top);
  EXPECT_EQ(got.values.actual_bounding_box_right, box.right);
  EXPECT_EQ(got.values.actual_bounding_box_descent, box.bottom);
}

// --- Emoji ------------------------------------------------------------------

TEST(MoltNetTextValuesTest, EmojiFamilyIsAllowListed) {
  const char* linux_family = UniformEmojiFamily(FontPlatform::kLinux);
  ASSERT_NE(linux_family, nullptr);
  EXPECT_EQ(std::string_view(linux_family), "Noto Color Emoji");
  const FontName name = FontName::Utf8(linux_family);
  // A listed family, not an alias, so pages may name it in MoltNet anyway.
  EXPECT_TRUE(IsAllowListedFamily(FontPlatform::kLinux, name, name.Hash()));
  // Elsewhere the platform's own fallback picks one font for everybody.
  EXPECT_EQ(UniformEmojiFamily(FontPlatform::kWindows), nullptr);
  EXPECT_EQ(UniformEmojiFamily(FontPlatform::kMac), nullptr);
  EXPECT_EQ(UniformEmojiFamily(FontPlatform::kAndroid), nullptr);
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
