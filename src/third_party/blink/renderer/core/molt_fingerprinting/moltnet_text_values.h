// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: MoltNet's canvas text metrics, the
// arithmetic. Pure and std-only (tested standalone, like farbling.h); the
// Blink side is moltnet_text.h, the reasoning docs/FINGERPRINTING.md "Text
// metrics".
//
// measureText() reports lengths that come from the installed font file
// (advances, ascent, descent), from how the machine rasterises it (glyph ink
// bounds, which hinting moves by up to a pixel; advances and descents that
// some settings round to whole pixels) and from float noise along the way.
// In MoltNet windows every length scripts read is rounded to a grid: whole
// CSS pixels for text up to 127px, a 64th to a 128th of the font size above
// (TextMetricsGrid), so the sub-pixel differences between machines that
// draw the same font don't show, and drawing text larger doesn't buy back
// the precision.
//
// The rounding is applied to the lengths TextMetrics derives its attributes
// from, not to the attributes: the advance, the ink box, the font's ascent,
// descent and em box, its baselines and the offsets of the chosen textAlign
// and textBaseline. The attributes are then derived from the rounded lengths
// the way upstream derives them, so they stay consistent with each other
// whatever textAlign and textBaseline a page measures with (rounding the
// attributes instead would let a page recover sub-grid digits by measuring
// one text under several alignments). The ink box is rounded outwards, so it
// still contains every glyph (to within a 2048th of a grid step, the
// settling below); everything else to the nearest grid point.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_TEXT_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_TEXT_VALUES_H_

#include <cstdint>

#include "third_party/blink/renderer/core/molt_fingerprinting/font_allowlists.h"

namespace blink::molt_fingerprinting::moltnet {

// --- The grid ----------------------------------------------------------------

// The grid for a font of `font_size` CSS pixels: the largest power of two
// that is at most font_size / kTextGridEmDivisor, and at least
// kTextGridMin (1 CSS pixel, the pixel grid hinting snaps to at the sizes
// pages set text in). Powers of two keep every rounded length, and every
// sum, half and difference of them, exact in floating point. A size that
// isn't a positive finite number gets kTextGridMin.
inline constexpr double kTextGridMin = 1.0;
inline constexpr double kTextGridEmDivisor = 64.0;
inline constexpr double kTextGridMax = 1048576.0;  // 2^20.
double TextMetricsGrid(double font_size);

// Rounding to `grid`. A value is first settled on a lattice of
// 1/kTextGridSettle grid steps, so float noise from the way Blink computes
// it (12.0000005, a half that came out 0.4999999) neither moves an edge a
// whole step out nor decides a tie. A `grid` that isn't positive and
// finite, and a value that isn't finite or is beyond 2^42 steps, are left
// as they are.
inline constexpr double kTextGridSettle = 1024;
// The nearest grid point, halves away from zero (so -x rounds to the
// negation of x's grid point, which keeps opposite textBaselines symmetric).
// Zero is always +0.
double RoundToGrid(double value, double grid);
// The grid point at or below / at or above `value`.
double FloorToGrid(double value, double grid);
double CeilToGrid(double value, double grid);

// --- measureText() -----------------------------------------------------------

// TextMetrics' attributes (text_metrics.idl), as TextMetrics::Update()
// computes them.
struct TextMetricsValues {
  double width = 0;
  double actual_bounding_box_left = 0;
  double actual_bounding_box_right = 0;
  double font_bounding_box_ascent = 0;
  double font_bounding_box_descent = 0;
  double actual_bounding_box_ascent = 0;
  double actual_bounding_box_descent = 0;
  double em_height_ascent = 0;
  double em_height_descent = 0;
  double alphabetic_baseline = 0;
  double hanging_baseline = 0;
  double ideographic_baseline = 0;
};

// The alignment point TextMetrics::Update() resolved textAlign and the
// direction to (its ctx_text_align_): the text's left end, centre or right
// end.
enum class TextAlignPoint : uint8_t { kLeft, kCenter, kRight };

// The frame the attributes were computed in (TextMetrics' text_align_dx_
// and baseline_y): the alignment point's distance from the text's left end
// (0, width / 2 or width) and the textBaseline's height above the
// alphabetic baseline (TextMetrics::GetFontBaseline()).
struct TextMetricsFrame {
  TextAlignPoint align = TextAlignPoint::kLeft;
  double align_dx = 0;
  double baseline = 0;
};

// MoltNet's metrics: recovers from `values` and `frame` the lengths
// TextMetrics::Update() derived them from (in the text's own frame: x from
// its left end, y up from the alphabetic baseline), rounds those to `grid`
// (the ink box outwards, the rest to the nearest point; the alignment
// offset is the rounded width's 0, half or whole) and derives the
// attributes again. `frame` gets the rounded offsets, which the
// ExtendedTextMetrics methods use. Idempotent; nothing changes for an
// invalid `grid`.
void RoundTextMetrics(double grid,
                      TextMetricsValues& values,
                      TextMetricsFrame& frame);

// --- The ExtendedTextMetrics methods -----------------------------------------
// (an origin trial at 152: getSelectionRects(), getActualBoundingBox(),
// getIndexFromOffset(), getTextClusters()). In the text's own frame, x from
// its left end, y down from the alphabetic baseline, as the methods compute
// them before applying the alignment and baseline offsets.

// A span of the text (a selection, a cluster): both ends to the nearest
// grid point, so spans that touch still touch, and the whole text's span is
// the rounded width.
void RoundSpan(double grid, double& start, double& end);

// An ink box: each edge outwards to the grid, as in RoundTextMetrics().
struct InkBox {
  double left = 0;
  double top = 0;
  double right = 0;
  double bottom = 0;
};
InkBox RoundInkBox(double grid, const InkBox& box);

// --- Emoji -------------------------------------------------------------------

// The family MoltNet renderers on `platform` take emoji from first, a font
// on that platform's allow-list, or null where the platform's own fallback
// already picks one font for every user (Windows: Segoe UI Emoji, macOS:
// Apple Color Emoji, Android: Noto Color Emoji). On Linux, fontconfig picks
// the first colour emoji font of the machine's configuration, which may be
// one the user installed (Twemoji, JoyPixels, Symbola, ...); MoltNet takes
// Noto Color Emoji when it is installed, as on most desktop distributions.
const char* UniformEmojiFamily(FontPlatform platform);

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_TEXT_VALUES_H_
