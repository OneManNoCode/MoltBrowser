// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_text.h"

#include <atomic>

#include "third_party/blink/renderer/bindings/core/v8/v8_baselines.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_canvas_text_align.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/geometry/dom_rect_read_only.h"
#include "third_party/blink/renderer/core/html/canvas/text_cluster.h"
#include "third_party/blink/renderer/core/html/canvas/text_metrics.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_text_values.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_uniformity.h"
#include "third_party/blink/renderer/platform/fonts/font.h"
#include "third_party/blink/renderer/platform/fonts/font_platform_data.h"
#include "third_party/blink/renderer/platform/fonts/simple_font_data.h"

namespace blink::molt_fingerprinting {

namespace {

std::atomic<bool> g_uniform_for_testing{false};

moltnet::TextAlignPoint AlignPointOf(V8CanvasTextAlign::Enum align) {
  // TextMetrics::Update() resolves start and end to left or right.
  switch (align) {
    case V8CanvasTextAlign::Enum::kCenter:
      return moltnet::TextAlignPoint::kCenter;
    case V8CanvasTextAlign::Enum::kRight:
      return moltnet::TextAlignPoint::kRight;
    case V8CanvasTextAlign::Enum::kLeft:
    case V8CanvasTextAlign::Enum::kStart:
    case V8CanvasTextAlign::Enum::kEnd:
      return moltnet::TextAlignPoint::kLeft;
  }
  return moltnet::TextAlignPoint::kLeft;
}

}  // namespace

bool UsesUniformTextMetrics(ExecutionContext* context) {
  if (g_uniform_for_testing.load(std::memory_order_relaxed)) {
    // As UsesMoltNetUniformity() decides in kUniform.
    return !context || IsFarbled(context);
  }
  return UsesMoltNetUniformity(context);
}

void UniformTextMetrics(ExecutionContext* context, TextMetrics& metrics) {
  if (!UsesUniformTextMetrics(context)) {
    return;
  }
  TextMetricsAccess::Round(
      metrics, moltnet::TextMetricsGrid(TextMetricsAccess::FontSize(metrics)));
}

void UniformSelectionRects(double grid,
                           float align_dx,
                           HeapVector<Member<DOMRectReadOnly>>& rects) {
  if (!(grid > 0)) {
    return;
  }
  for (Member<DOMRectReadOnly>& rect : rects) {
    if (!rect) {
      continue;
    }
    // Into the text's frame, where the spans were measured.
    double start = rect->x() + align_dx;
    double end = rect->x() + rect->width() + align_dx;
    moltnet::RoundSpan(grid, start, end);
    rect = DOMRectReadOnly::Create(start - align_dx, rect->y(), end - start,
                                   rect->height());
  }
}

gfx::RectF UniformInkBounds(double grid, const gfx::RectF& bounds) {
  if (!(grid > 0)) {
    return bounds;
  }
  // Every edge, also of an empty box: a zero-width ink box still has a
  // position.
  const moltnet::InkBox box = moltnet::RoundInkBox(
      grid, {bounds.x(), bounds.y(), bounds.right(), bounds.bottom()});
  return gfx::RectF(static_cast<float>(box.left), static_cast<float>(box.top),
                    static_cast<float>(box.right - box.left),
                    static_cast<float>(box.bottom - box.top));
}

float UniformCaretPosition(double grid, float x) {
  return static_cast<float>(moltnet::RoundToGrid(x, grid));
}

void UniformClusterSpan(double grid, float& x, float& width) {
  if (!(grid > 0)) {
    return;
  }
  double start = x;
  double end = static_cast<double>(x) + width;
  moltnet::RoundSpan(grid, start, end);
  x = static_cast<float>(start);
  width = static_cast<float>(end - start);
}

void UniformClusterBaseline(double grid, TextCluster& cluster) {
  if (!(grid > 0)) {
    return;
  }
  cluster.OffsetPosition(0, moltnet::RoundToGrid(cluster.y(), grid) -
                                cluster.y());
}

void PinUniformTextRendering() {
  FontPlatformData::UniformTextRendering rendering;
  rendering.geometric_precision = true;
  rendering.emoji_family =
      moltnet::UniformEmojiFamily(CurrentFontPlatform());
  FontPlatformData::SetUniformTextRendering(rendering);
}

void SetUniformTextMetricsForTesting(bool uniform) {
  g_uniform_for_testing.store(uniform, std::memory_order_relaxed);
}

void TextMetricsAccess::Round(TextMetrics& metrics, double grid) {
  if (!metrics.font_ || !(grid > 0)) {
    // A TextMetrics of a frameless document: all zeros, nothing to round.
    return;
  }
  moltnet::TextMetricsValues values;
  values.width = metrics.width_;
  values.actual_bounding_box_left = metrics.actual_bounding_box_left_;
  values.actual_bounding_box_right = metrics.actual_bounding_box_right_;
  values.font_bounding_box_ascent = metrics.font_bounding_box_ascent_;
  values.font_bounding_box_descent = metrics.font_bounding_box_descent_;
  values.actual_bounding_box_ascent = metrics.actual_bounding_box_ascent_;
  values.actual_bounding_box_descent = metrics.actual_bounding_box_descent_;
  values.em_height_ascent = metrics.em_height_ascent_;
  values.em_height_descent = metrics.em_height_descent_;
  values.alphabetic_baseline = metrics.baselines_->alphabetic();
  values.hanging_baseline = metrics.baselines_->hanging();
  values.ideographic_baseline = metrics.baselines_->ideographic();
  moltnet::TextMetricsFrame frame;
  frame.align = AlignPointOf(metrics.ctx_text_align_);
  frame.align_dx = metrics.text_align_dx_;
  frame.baseline = metrics.baseline_y;

  moltnet::RoundTextMetrics(grid, values, frame);

  metrics.width_ = values.width;
  metrics.actual_bounding_box_left_ = values.actual_bounding_box_left;
  metrics.actual_bounding_box_right_ = values.actual_bounding_box_right;
  metrics.font_bounding_box_ascent_ = values.font_bounding_box_ascent;
  metrics.font_bounding_box_descent_ = values.font_bounding_box_descent;
  metrics.actual_bounding_box_ascent_ = values.actual_bounding_box_ascent;
  metrics.actual_bounding_box_descent_ = values.actual_bounding_box_descent;
  metrics.em_height_ascent_ = values.em_height_ascent;
  metrics.em_height_descent_ = values.em_height_descent;
  metrics.baselines_->setAlphabetic(values.alphabetic_baseline);
  metrics.baselines_->setHanging(values.hanging_baseline);
  metrics.baselines_->setIdeographic(values.ideographic_baseline);
  // Grid points and their halves: exact as floats for any text a page can
  // lay out (below 2^24 grid steps).
  metrics.text_align_dx_ = static_cast<float>(frame.align_dx);
  metrics.baseline_y = static_cast<float>(frame.baseline);
  metrics.molt_net_grid_ = grid;
}

double TextMetricsAccess::FontSize(const TextMetrics& metrics) {
  const Font* font = metrics.font_.Get();
  if (!font) {
    return 0;
  }
  // The size the glyphs were measured at (font-size-adjust included).
  if (const SimpleFontData* primary = font->PrimaryFont()) {
    return primary->PlatformData().size();
  }
  return font->GetFontDescription().ComputedSize();
}

double TextMetricsAccess::Grid(const TextMetrics& metrics) {
  return metrics.molt_net_grid_;
}

}  // namespace blink::molt_fingerprinting
