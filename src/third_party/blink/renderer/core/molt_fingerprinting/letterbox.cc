// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/letterbox.h"

#include <atomic>

#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/layout/adjust_for_absolute_zoom.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/page/frame_tree.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace blink::molt_fingerprinting {

namespace {

std::atomic<bool> g_uniform_device_pixel_ratio_for_testing{false};

bool UniformDevicePixelRatio() {
  return IsUniform() || g_uniform_device_pixel_ratio_for_testing.load(
                            std::memory_order_relaxed);
}

// Whether the documents of `frame` are web content: the top-level frame's
// origin is http(s), or opaque (a sandboxed top-level document, a new tab's
// about:blank). The top frame's origin is replicated into every process, so
// this is a few pointer reads, no string building. Internal pages
// (chrome://, extensions, file://) keep their real density.
bool IsWebContent(const LocalFrame& frame) {
  const SecurityContext* top = frame.Tree().Top().GetSecurityContext();
  const SecurityOrigin* origin = top ? top->GetSecurityOrigin() : nullptr;
  if (!origin || origin->IsOpaque()) {
    return true;
  }
  return origin->Protocol() == "https" || origin->Protocol() == "http";
}

template <typename T>
T PickDevicePixelRatio(const LocalFrame* frame, T real) {
  if (!UniformDevicePixelRatio() || !frame || !IsWebContent(*frame)) {
    return real;
  }
  return static_cast<T>(kUniformDevicePixelRatio);
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

void SetUniformDevicePixelRatioForTesting(bool uniform) {
  g_uniform_device_pixel_ratio_for_testing.store(uniform,
                                                 std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
