// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet letterboxing, browser side: the geometry.
//
// In MoltNet windows a tab's web contents don't fill the content area. They
// get a centred part of it, with margins around them, sized so that the
// page's CSS size -- innerWidth/innerHeight, the width/height media queries,
// vw/vh -- is a multiple of kLetterboxStep{Width,Height}, at most
// kLetterboxMax{Width,Height}: the largest that fits. A page's size is then
// one of a few dozen, the same for everybody whose window rounds the same
// way, at every page zoom and device scale factor, and exactly what
// screen.width/height report in MoltNet: Blink's UniformScreenRect() rounds
// the page's CSS size with the same steps (LetterboxBucket in
// third_party/blink/renderer/core/molt_fingerprinting/farbling.h), and
// rounding an already rounded size changes nothing.
//
// A page's CSS size is its DIP size times the device scale factor, rounded
// up to device pixels, divided by the layout zoom (device scale factor x page
// zoom) and rounded again, all in the renderer. So the browser picks the DIP
// size by running the renderer's own arithmetic, replicated here bit for bit
// (PageDevicePixels() ... PageMediaQueryPixels(); float, like the originals,
// and Chromium builds with -ffp-contract=off, so both processes round
// alike). At 100% zoom on a 1x screen a CSS pixel is a DIP and the page gets
// exactly LetterboxLength() of the area, as before zoom was taken into
// account.
//
// The constants and the renderer's arithmetic are duplicated because
// browser code can't include Blink's renderer headers;
// molt_letterbox_unittest.cc checks that they match Blink's (farbling.h),
// and Blink's letterbox_test.cc that farbling.h's copy of the arithmetic is
// what Blink and gfx really compute.
//
// Std-only and constexpr, so it is unit-tested standalone and with
// static_asserts. Where it meets the window: molt_letterbox_view.h (the
// margins, the page's scale, background tabs) and
// chrome/browser/molt_ai/tor/molt_net_window.cc (the size new MoltNet
// windows open at, WindowLetterboxSize()).

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_H_

#include <limits>

namespace molt_ai::fingerprinting {

// Must equal blink::molt_fingerprinting::kLetterbox* (farbling.h).
inline constexpr int kLetterboxStepWidth = 200;
inline constexpr int kLetterboxStepHeight = 100;
inline constexpr int kLetterboxMaxWidth = 1400;
inline constexpr int kLetterboxMaxHeight = 900;

struct LetterboxMargins {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  friend constexpr bool operator==(const LetterboxMargins&,
                                   const LetterboxMargins&) = default;
};

// The letterbox step a `length`-CSS-pixel page is rounded to: down to a
// multiple of `step`, at most `max` (a multiple of `step`). A length shorter
// than one step keeps its length (there is nothing to round down to; Blink
// reports one step there, see docs/FINGERPRINTING.md); a negative one counts
// as empty. At 100% zoom on a 1x screen CSS pixels are DIPs, and this is the
// page's DIP length in a `length`-DIP area too.
constexpr int LetterboxLength(int length, int step, int max) {
  if (length <= 0) {
    return 0;
  }
  if (length < step) {
    return length;
  }
  const int rounded = length - length % step;
  return rounded < max ? rounded : max;
}

// The scale a page is shown at, as its renderer has it: the device scale
// factor of its RenderWidgetHostView (the screen's) and its page zoom factor
// (blink::ZoomLevelToZoomFactor() of the zoom level content sends it, cast
// to float as the renderer casts it). The default, 100% on a 1x screen, is
// where CSS pixels are DIPs.
struct PageScale {
  float device_scale_factor = 1.0f;
  float zoom_factor = 1.0f;
};

// The renderer's arithmetic for a page shown in a `dips`-DIP-long area,
// replicated from Chromium 152 (and checked against it, see above).

namespace internal {

// base::ClampCeil(float) for the non-negative lengths here.
constexpr int CeilToInt(float value) {
  if (!(value > 0.0f)) {
    return 0;
  }
  if (value >= 2147483648.0f) {
    return std::numeric_limits<int>::max();
  }
  const int truncated = static_cast<int>(value);
  return static_cast<float>(truncated) < value ? truncated + 1 : truncated;
}

}  // namespace internal

// The page's length in device pixels as content sends it to the renderer:
// gfx::ScaleToCeiledSize() of the DIP size
// (RenderWidgetHostViewBase::GetRequestedRendererSizeDevicePx()).
constexpr int PageDevicePixels(int dips, float device_scale_factor) {
  if (device_scale_factor == 1.0f) {
    return dips;
  }
  return internal::CeilToInt(static_cast<float>(dips) * device_scale_factor);
}

// The page's layout zoom: WebFrameWidgetImpl::SetZoomInternal(), device
// scale factor x page zoom factor (x the CSS zoom of the frame's embedder,
// 1 for a page's main frame).
constexpr float PageLayoutZoom(const PageScale& scale) {
  return scale.device_scale_factor * scale.zoom_factor;
}

// innerWidth / innerHeight of a page `device_pixels` long:
// AdjustForAbsoluteZoom::AdjustInt(), RoundForImpreciseConversion()
// included.
constexpr int PageCssPixels(int device_pixels, float layout_zoom) {
  if (layout_zoom == 1.0f) {
    return device_pixels;
  }
  float value = static_cast<float>(device_pixels);
  if (layout_zoom > 1.0f) {
    value += device_pixels < 0 ? -0.5f : 0.5f;
  }
  double css = static_cast<double>(value / layout_zoom);
  css += css < 0 ? -0.01 : 0.01;
  return (css > std::numeric_limits<int>::max() ||
          css < std::numeric_limits<int>::min())
             ? 0
             : static_cast<int>(css);
}

// The page's length for the width/height media queries and viewport units:
// LocalFrameView::ViewportSizeForMediaQueries().
constexpr float PageMediaQueryPixels(int device_pixels, float layout_zoom) {
  return static_cast<float>(device_pixels) * (1.0f / layout_zoom);
}

// innerWidth / innerHeight of a page `dips` DIPs long at `scale`.
constexpr int PageCssLength(int dips, const PageScale& scale) {
  return PageCssPixels(PageDevicePixels(dips, scale.device_scale_factor),
                       PageLayoutZoom(scale));
}

namespace internal {

// A scale the renderer could have (a NaN fails both comparisons).
constexpr bool IsUsableScale(const PageScale& scale) {
  return scale.device_scale_factor > 0.0f &&
         scale.device_scale_factor < 1000.0f && scale.zoom_factor > 0.0f &&
         scale.zoom_factor < 1000.0f;
}

// How many DIP lengths with the same CSS length DipsForCssLength() compares:
// about the zoom factor's worth (at most 5x), more only on a screen far below
// 1x.
inline constexpr int kMaxCandidates = 64;

// How far a media-query length may be from a whole number of CSS pixels and
// still match it: LayoutUnit::Epsilon(), the tolerance of the media queries'
// =, <= and >= comparisons (MediaQueryEvaluator's CompareDoubleValue()).
inline constexpr float kMediaQueryTolerance = 1.0f / 64;

struct DipLength {
  int dips = 0;        // 0: none
  float error = 0.0f;  // how far its media-query length is from the target
};

// The DIP length, at most `available`, that gives a page a CSS length of
// exactly `css` at `scale`; of those, the one whose media-query length is
// closest to `css` (the shortest of equals). None if the renderer's rounding
// jumps over `css` (at 33% zoom on a 1x screen, for example, one DIP is
// three CSS pixels) or it doesn't fit.
constexpr DipLength DipsForCssLength(int css,
                                     int available,
                                     const PageScale& scale) {
  if (css <= 0 || available <= 0 || PageCssLength(available, scale) < css) {
    return {};
  }
  // PageCssLength() never decreases as the DIP length grows: the first DIP
  // length that reaches `css`.
  int low = 1;
  int high = available;
  while (low < high) {
    const int middle = low + (high - low) / 2;
    if (PageCssLength(middle, scale) < css) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  const float zoom = PageLayoutZoom(scale);
  const float target = static_cast<float>(css);
  DipLength best;
  for (int dips = low, tries = 0; dips <= available && tries < kMaxCandidates;
       ++dips, ++tries) {
    const int device_pixels = PageDevicePixels(dips, scale.device_scale_factor);
    if (PageCssPixels(device_pixels, zoom) != css) {
      break;
    }
    const float media = PageMediaQueryPixels(device_pixels, zoom);
    const float error = media > target ? media - target : target - media;
    if (best.dips == 0 || error < best.error) {
      best = {dips, error};
    }
  }
  return best;
}

// The page's DIP length in an `available`-DIP area: for the largest multiple
// of `step`, at most `max`, that fits, that DipsForCssLength() can hit and
// whose media-query length then matches it too (within
// kMediaQueryTolerance); if no step's does, for the largest that
// DipsForCssLength() can hit at all. 0 if none can (the area is shorter than
// one step at this zoom).
//
// Why the media queries come first: on most screens at most zoom levels a
// page's media-query length is its innerWidth/innerHeight, give or take a
// float rounding. Where the renderer's device pixels don't divide evenly (a
// 4/3 screen, 67% zoom, ...), a step can come out half a CSS pixel off, which
// (max-width: ...) queries and rounded getBoundingClientRect() widths see;
// such a page is better off one step smaller and exact, like everybody
// else's.
constexpr int LetterboxedLength(int available,
                                int step,
                                int max,
                                const PageScale& scale) {
  if (available <= 0 || step <= 0 || max < step) {
    return 0;
  }
  const int css_available = PageCssLength(available, scale);
  if (css_available < step) {
    return 0;
  }
  int fallback = 0;
  for (int css = LetterboxLength(css_available, step, max); css >= step;
       css -= step) {
    const DipLength length = DipsForCssLength(css, available, scale);
    if (length.dips == 0) {
      continue;
    }
    if (length.error <= kMediaQueryTolerance) {
      return length.dips;
    }
    if (fallback == 0) {
      fallback = length.dips;
    }
  }
  return fallback;
}

}  // namespace internal

// How much of an `available`-DIP-long area the page gets at `scale`: the DIP
// length that gives it the CSS length of the largest multiple of `step`, at
// most `max`, that fits and that the renderer's rounding can give it exactly.
// At a few zoom levels and scale factors some steps can't be hit (at 33% and
// 67% zoom only every third 100 CSS pixels, at a scale factor of 1.2 for
// example 1400); the page then gets the next smaller one. An area too short
// for one step keeps its length; a negative one counts as empty. A scale the
// renderer can't have is taken as 100% on a 1x screen.
constexpr int LetterboxPageLength(int available,
                                  int step,
                                  int max,
                                  const PageScale& scale = {}) {
  if (available <= 0) {
    return 0;
  }
  const int letterboxed = internal::LetterboxedLength(
      available, step, max,
      internal::IsUsableScale(scale) ? scale : PageScale());
  return letterboxed > 0 ? letterboxed : available;
}

// The margins that letterbox web contents in a `width` x `height` DIP area
// at `scale`: what LetterboxPageLength() leaves in each dimension, split
// evenly (the odd DIP goes to the right / bottom).
constexpr LetterboxMargins LetterboxMarginsFor(int width,
                                               int height,
                                               const PageScale& scale = {}) {
  const int spare_width =
      width - LetterboxPageLength(width, kLetterboxStepWidth,
                                  kLetterboxMaxWidth, scale);
  const int spare_height =
      height - LetterboxPageLength(height, kLetterboxStepHeight,
                                   kLetterboxMaxHeight, scale);
  LetterboxMargins margins;
  if (spare_width > 0) {
    margins.left = spare_width / 2;
    margins.right = spare_width - margins.left;
  }
  if (spare_height > 0) {
    margins.top = spare_height / 2;
    margins.bottom = spare_height - margins.top;
  }
  return margins;
}

struct LetterboxSize {
  int width = 0;
  int height = 0;

  constexpr bool IsEmpty() const { return width <= 0 || height <= 0; }
  friend constexpr bool operator==(const LetterboxSize&,
                                   const LetterboxSize&) = default;
};

// The page size a new MoltNet window opens with (molt_net_window.cc), so it
// starts without margins: the largest letterbox size -- whole steps, at
// most kLetterboxMax{Width,Height} (1400 x 900) in CSS pixels at `scale` --
// that fits on the screen together with the browser UI around the page.
// `window_*` is the window's current size and `page_*` its page area's, so
// the difference is that UI (frame, tab strip, toolbar, side panel, ...);
// `work_area_*` is the size of the screen's work area. All in DIPs. Empty
// when there is no room for one step, or the sizes make no sense: the window
// then keeps its size and the letterbox margins do the rest.
constexpr LetterboxSize WindowLetterboxSize(int window_width,
                                            int window_height,
                                            int page_width,
                                            int page_height,
                                            int work_area_width,
                                            int work_area_height,
                                            const PageScale& scale = {}) {
  if (page_width <= 0 || page_height <= 0 || window_width < page_width ||
      window_height < page_height || work_area_width <= 0 ||
      work_area_height <= 0) {
    return {};
  }
  // Both differences are in [0, window size], so nothing overflows.
  const int room_width = work_area_width - (window_width - page_width);
  const int room_height = work_area_height - (window_height - page_height);
  const PageScale usable = internal::IsUsableScale(scale) ? scale : PageScale();
  const int width = internal::LetterboxedLength(room_width, kLetterboxStepWidth,
                                                kLetterboxMaxWidth, usable);
  const int height = internal::LetterboxedLength(
      room_height, kLetterboxStepHeight, kLetterboxMaxHeight, usable);
  if (width <= 0 || height <= 0) {
    return {};
  }
  return {width, height};
}

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_H_
