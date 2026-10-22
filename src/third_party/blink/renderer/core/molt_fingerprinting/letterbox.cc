// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/letterbox.h"

#include <atomic>
#include <optional>

#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/layout/adjust_for_absolute_zoom.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/page/frame_tree.h"
#include "third_party/blink/renderer/core/scroll/scrollable_area.h"
#include "third_party/blink/renderer/core/scroll/scrollbar_theme.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace blink::molt_fingerprinting {

namespace {

std::atomic<bool> g_uniform_for_testing{false};

// MoltNet, or a test standing in for it. Cheap: an atomic load in every
// other mode.
bool UniformPage() {
  return IsUniform() || g_uniform_for_testing.load(std::memory_order_relaxed);
}

// Whether the documents of `frame` are web content: the top-level frame's
// origin is http(s), or opaque (a sandboxed top-level document, a new tab's
// about:blank). The top frame's origin is replicated into every process, so
// this is a few pointer reads, no string building. Internal pages
// (chrome://, extensions, file://) keep their real density, scrollbars and
// viewport lengths.
bool IsWebContent(const LocalFrame& frame) {
  const SecurityContext* top = frame.Tree().Top().GetSecurityContext();
  const SecurityOrigin* origin = top ? top->GetSecurityOrigin() : nullptr;
  if (!origin || origin->IsOpaque()) {
    return true;
  }
  return origin->Protocol() == "https" || origin->Protocol() == "http";
}

// Whether `frame`'s documents get MoltNet's page geometry: MoltNet (or a test
// standing in for it) and web content.
bool UniformGeometry(const LocalFrame* frame) {
  return UniformPage() && frame && IsWebContent(*frame);
}

template <typename T>
T PickDevicePixelRatio(const LocalFrame* frame, T real) {
  if (!UniformGeometry(frame)) {
    return real;
  }
  return static_cast<T>(kUniformDevicePixelRatio);
}

// MoltNet's thickness in CSS pixels for a scrollbar of `width` that `theme`
// draws in a document of `frame`, or nullopt where the theme's own applies
// (not MoltNet web content, overlay scrollbars).
std::optional<int> UniformScrollbarCssThickness(const ScrollbarTheme& theme,
                                                EScrollbarWidth width,
                                                const LocalFrame* frame) {
  if (!UniformGeometry(frame) || theme.UsesOverlayScrollbars()) {
    return std::nullopt;
  }
  if (width == EScrollbarWidth::kNone) {
    return 0;
  }
  return width == EScrollbarWidth::kThin ? kUniformThinScrollbarThickness
                                         : kUniformScrollbarThickness;
}

}  // namespace

gfx::Size UniformScreenSizeOf(LocalFrame& frame) {
  gfx::Size viewport;
  const LocalFrame& root = frame.LocalFrameRoot();
  if (!root.IsOutermostMainFrame()) {
    // A frame in another process than the main frame (or in a fenced
    // frame): the main frame's size as its renderer passes it down, which
    // leaves out the main frame's scrollbars.
    viewport = frame.GetOutermostMainFrameSize();
  } else if (const LocalFrameView* view = root.View()) {
    // The size innerWidth / innerHeight report: the main frame's frame
    // size, scrollbars included. (Very early in a main frame's life there is
    // no view yet; the size is then empty, which buckets to one step.)
    viewport = view->Size();
  }
  // Device pixels to the page's CSS pixels with innerWidth / innerHeight's
  // own rounding and zoom, so a letterboxed page's screen size and inner
  // size are the same number at every zoom level.
  const float scale = UniformScreenScale(frame);
  const LetterboxSize size = LetterboxBucket(
      AdjustForAbsoluteZoom::AdjustInt(viewport.width(), scale),
      AdjustForAbsoluteZoom::AdjustInt(viewport.height(), scale));
  return gfx::Size(size.width, size.height);
}

float UniformScreenScale(const LocalFrame& frame) {
  const LocalFrame& root = frame.LocalFrameRoot();
  float scale = root.LayoutZoomFactor();
  if (!root.IsOutermostMainFrame()) {
    // WebFrameWidgetImpl::SetZoomInternal() gives a frame's local root the
    // page's layout zoom times the CSS zoom of the element embedding it.
    const float css_zoom = root.CssZoomFactor();
    if (css_zoom > 0) {
      scale /= css_zoom;
    }
  }
  // A NaN fails the comparison too.
  return scale > 0 ? scale : 1.0f;
}

double DevicePixelRatioFor(const LocalFrame* frame, double real) {
  return PickDevicePixelRatio(frame, real);
}

float DevicePixelRatioFor(const LocalFrame* frame, float real) {
  return PickDevicePixelRatio(frame, real);
}

int ScrollbarThicknessFor(const ScrollbarTheme& theme,
                          float scale_from_dip,
                          EScrollbarWidth width,
                          const Document* document) {
  const LocalFrame* frame = document ? document->GetFrame() : nullptr;
  const std::optional<int> css_thickness =
      UniformScrollbarCssThickness(theme, width, frame);
  if (!css_thickness) {
    return theme.ScrollbarThickness(scale_from_dip, width);
  }
  // The CSS pixels of the document the scrollbar is in: its frame's layout
  // zoom, which is what that document's layout divides by.
  return UniformScrollbarDevicePixels(*css_thickness,
                                      frame->LayoutZoomFactor());
}

int ScrollbarThicknessForViewportUnits(const ScrollbarTheme& theme,
                                       EScrollbarWidth width,
                                       const Document* document) {
  const std::optional<int> css_thickness = UniformScrollbarCssThickness(
      theme, width, document ? document->GetFrame() : nullptr);
  return css_thickness ? *css_thickness : theme.ScrollbarThickness(1.f, width);
}

float ScrollbarScaleFromDIP(const ScrollableArea& area, float scale_from_dip) {
  if (!UniformPage()) {
    return scale_from_dip;
  }
  const Document* document = area.GetDocument();
  const LocalFrame* frame = document ? document->GetFrame() : nullptr;
  if (!UniformGeometry(frame) ||
      area.GetPageScrollbarTheme().UsesOverlayScrollbars()) {
    return scale_from_dip;
  }
  const float zoom = frame->LayoutZoomFactor();
  // A NaN fails the comparison too.
  return zoom > 0 ? zoom : scale_from_dip;
}

gfx::SizeF UniformViewportSize(const LocalFrame& frame,
                               const gfx::SizeF& css_size) {
  if (!UniformGeometry(&frame)) {
    return css_size;
  }
  return gfx::SizeF(static_cast<float>(SnapViewportLength(css_size.width())),
                    static_cast<float>(SnapViewportLength(css_size.height())));
}

double UniformViewportLength(const LocalFrame& frame, double css_length) {
  return UniformGeometry(&frame) ? SnapViewportLength(css_length) : css_length;
}

void SetUniformForTesting(bool uniform) {
  g_uniform_for_testing.store(uniform, std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
