// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: text measurement in MoltNet
// windows, the Blink side (docs/FINGERPRINTING.md "Text metrics"). The
// arithmetic is in moltnet_text_values.h.
//
// MoltNet already lets pages name only the OS's default fonts (fonts.h) and
// gives the generic families fixed per-OS fonts (the browser's
// molt_net_locale_prefs.h), so a page measures the same fonts on every
// MoltNet machine of an OS. What still differed between those machines:
//
//   * how the renderer rasterises them. On Linux (FreeType) hinting style,
//     anti-aliasing and the screen's scale factor decide whether glyphs are
//     measured hinted at their pixel size or unhinted, which changes ink
//     bounds by up to a pixel and, with full hinting, advances and the
//     font's descent; 1x and 2x screens differed even with default
//     settings. PinUniformTextRendering() makes MoltNet renderers lay out
//     all text as upstream does for text-rendering: geometricPrecision
//     (unhinted, subpixel positioning, linear metrics), and anti-aliased
//     even where the desktop turns anti-aliasing off, so metrics depend on
//     the font file only (one hook in font_platform_data.cc). Windows and
//     macOS measure unhinted already.
//   * which emoji font Linux falls back to (fontconfig's first colour emoji
//     font): Noto Color Emoji when installed (one hook in
//     font_cache_linux.cc).
//   * sub-pixel digits of canvas measureText(): rounded to a grid
//     (UniformTextMetrics(); one hook in base_rendering_context_2d.cc,
//     the TextMetrics members through TextMetricsAccess, and one line in
//     each ExtendedTextMetrics method of text_metrics.cc).
//
// Element geometry (getBoundingClientRect(), Range rects, offsetWidth, ...)
// is not rounded: layout reaches scripts through too many paths (hit
// testing, container queries, ResizeObserver, line breaks, transforms that
// scale sub-pixels up) for rounding a few of them to hold, and changing
// layout itself is out of bounds. It benefits from the uniform rendering.
//
// Which contexts. The rendering pin is process-wide (fonts are shared by
// every frame and worker of the renderer), like the time zone and the
// locale. The measureText() rounding follows UsesMoltNetUniformity(): web
// content in MoltNet windows, documents and workers (OffscreenCanvas);
// chrome:// and other non-http(s) top-level pages keep exact values; Shields
// off doesn't lift it, as elsewhere in MoltNet. Normal and Incognito windows
// are unchanged.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_TEXT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_TEXT_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "ui/gfx/geometry/rect_f.h"

namespace blink {

class DOMRectReadOnly;
class ExecutionContext;
class TextCluster;
class TextMetrics;

namespace molt_fingerprinting {

// Whether measureText() in `context` reports MoltNet's rounded metrics:
// kUniform and web content (UsesMoltNetUniformity()). A null context counts
// as web content.
CORE_EXPORT bool UsesUniformTextMetrics(ExecutionContext* context);

// BaseRenderingContext2D::measureText(), right after `metrics` was
// measured: in MoltNet web content, rounds every attribute as
// moltnet::RoundTextMetrics() describes, on the grid of the font's size
// (moltnet::TextMetricsGrid()), and keeps the grid for the
// ExtendedTextMetrics methods below. Otherwise nothing changes.
CORE_EXPORT void UniformTextMetrics(ExecutionContext* context,
                                    TextMetrics& metrics);

// The ExtendedTextMetrics methods of a TextMetrics whose grid is `grid` (0:
// it wasn't rounded, and these change nothing).
//
// getSelectionRects(): each rect's ends to the grid, in the text's frame
// (`align_dx` is the TextMetrics' alignment offset, already rounded).
CORE_EXPORT void UniformSelectionRects(
    double grid,
    float align_dx,
    HeapVector<Member<DOMRectReadOnly>>& rects);
// getActualBoundingBox(): the glyphs' ink box, in the text's frame before
// the alignment and baseline offsets, outwards to the grid.
CORE_EXPORT gfx::RectF UniformInkBounds(double grid, const gfx::RectF& bounds);
// getIndexFromOffset(): the point a caret is looked up for, in the text's
// frame, moved to the nearest grid point, so the answer shows where caret
// positions are only to the grid.
CORE_EXPORT float UniformCaretPosition(double grid, float x);
// getTextClusters(): a cluster's start and advance in the text's frame,
// both ends to the grid (before the alignment offsets are applied) ...
CORE_EXPORT void UniformClusterSpan(double grid, float& x, float& width);
// ... and its y after the baseline offsets, to the grid (the TextMetrics'
// own baseline offset is rounded already; the cluster's may not be).
CORE_EXPORT void UniformClusterBaseline(double grid, TextCluster& cluster);

// Configure(kUniform), once, on the main thread, before any font is
// created: the renderer lays all text out as for text-rendering:
// geometricPrecision and takes emoji from moltnet::UniformEmojiFamily()
// first (Linux; FontPlatformData::SetUniformTextRendering). Tests undo it
// with FontPlatformData::SetUniformTextRendering({}).
CORE_EXPORT void PinUniformTextRendering();

// Makes UsesUniformTextMetrics() behave as in kUniform without configuring
// the process for it (which would pin the time zone and locale for every
// later test). The context must still be farbled, so tests configure
// kFarble. The rendering pin isn't affected.
CORE_EXPORT void SetUniformTextMetricsForTesting(bool uniform);

// The TextMetrics members UniformTextMetrics() reads and writes; a friend of
// TextMetrics (text_metrics.h), so upstream needs no accessors.
struct CORE_EXPORT TextMetricsAccess {
  // Rounds `metrics` to `grid` and stores the grid in it.
  static void Round(TextMetrics& metrics, double grid);
  // The size of the font `metrics` was measured with, in CSS pixels, or 0.
  static double FontSize(const TextMetrics& metrics);
  // The grid `metrics` was rounded to (0: not rounded).
  static double Grid(const TextMetrics& metrics);
};

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_TEXT_H_
