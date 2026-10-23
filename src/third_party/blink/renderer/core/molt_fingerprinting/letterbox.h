// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: MoltNet letterboxing, Blink side.
//
// In MoltNet windows the browser letterboxes the page: its CSS size is
// rounded down to the letterbox steps of farbling.h, at whatever zoom level
// and scale factor, and centred
// (chrome/browser/molt_ai/fingerprinting/molt_letterbox.h). This file keeps
// every size and density the page can read consistent with that:
//
//   * UniformScreenSizeOf() is the size UniformScreenRect()
//     (molt_fingerprinting.h) reports for screen.* : the outermost main
//     frame's size, *including* its scrollbars, in the page's CSS pixels
//     (divided by its layout zoom, device scale factor x page zoom, and
//     rounded like innerWidth / innerHeight), then bucketed. Once the
//     browser has letterboxed the page, screen.width/height equal
//     innerWidth/innerHeight at every zoom level, so neither shows the zoom.
//   * MouseEvent / Touch screenX/Y are in the same CSS pixels
//     (UniformScreenScale(), which UniformScreenPoint() divides by).
//   * CSS device-width / device-height / device-aspect-ratio report the same
//     screen (hooked in MediaValues, through UniformScreenRect()).
//   * The device pixel ratio web content sees is kUniformDevicePixelRatio:
//     window.devicePixelRatio, the resolution / -webkit-device-pixel-ratio
//     media queries (matchMedia, <source media>, the preload scanner) and the
//     density srcset / <picture> and image-set() candidates are picked for.
//     Only what the page can read changes: Blink still renders at the real
//     scale, so text and CSS stay sharp.
//   * Classic (non-overlay) scrollbars are kUniformScrollbarThickness CSS
//     pixels thick (farbling.h) at every zoom level and scale factor and on
//     every OS, and drawn at the page's zoom (ScrollbarThicknessFor(),
//     ScrollbarScaleFromDIP()), so innerWidth -
//     document.documentElement.clientWidth shows neither the zoom nor the OS.
//   * The viewport lengths the page reads as floats -- the width / height
//     media queries, the viewport units, visualViewport.width / height -- are
//     whole CSS pixels wherever they are within float rounding of them
//     (UniformViewportSize(), UniformViewportLength()), like innerWidth /
//     innerHeight, so a strict (width > 1400px) doesn't show the zoom either.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   MediaValues::CalculateDeviceWidth/Height  -> UniformScreenRect
//   MediaValues::CalculateDevicePixelRatio    -> DevicePixelRatioFor
//   LocalDOMWindow::devicePixelRatio          -> DevicePixelRatioFor
//   HTMLImageElement::SelectSourceURL,
//     FindBestFitImageFromPictureParent       -> DevicePixelRatioFor
//   StyleResolverState (ElementStyleResources' density for image-set())
//                                             -> DevicePixelRatioFor
//   Scrollbar::ScrollbarThickness,
//     PaintLayerScrollableArea::
//       ComputeHypotheticalScrollbarThickness (scrollbar-gutter),
//     ScrollbarThickness (layout_text_control.cc: a textarea's width)
//                                             -> ScrollbarThicknessFor
//   PaintLayerScrollableArea::
//     ComputeScrollbarWidthsForViewportUnits
//                               -> ScrollbarThicknessForViewportUnits
//   ScrollableArea::ScaleFromDIP              -> ScrollbarScaleFromDIP
//   LocalFrameView::ViewportSizeForMediaQueries,
//     SmallViewportSizeForViewportUnits       -> UniformViewportSize
//   DOMVisualViewport::width/height           -> UniformViewportLength
//
// Normal and Incognito windows (kFarble) are left alone: their goal is
// per-site unlinkability, and a fixed screen or density would only make
// users look like other MoltBrowser users (docs/FINGERPRINTING.md).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_LETTERBOX_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_LETTERBOX_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/style/computed_style_constants.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_f.h"

namespace blink {

class Document;
class LocalFrame;
class ScrollableArea;
class ScrollbarTheme;

namespace molt_fingerprinting {

// The device pixel ratio every MoltNet page reports: one CSS pixel per
// device pixel, as on a standard-density screen (Tor Browser's choice too).
inline constexpr double kUniformDevicePixelRatio = 1.0;

// The uniform screen size for documents of `frame`, whatever the mode: the
// outermost main frame's size in its CSS pixels, scrollbars included and
// rounded the way innerWidth / innerHeight round, then LetterboxBucket()ed.
// A frame in another process than the main frame only knows the main
// frame's size without its scrollbars (docs/FINGERPRINTING.md).
CORE_EXPORT gfx::Size UniformScreenSizeOf(LocalFrame& frame);

// Device pixels per CSS pixel of the page `frame` is in: the layout zoom of
// its outermost main frame (device scale factor x page zoom), which that
// frame's innerWidth / innerHeight divide by. A frame in another process
// than the main frame has the same zoom times the CSS zoom of the element
// embedding it, which is taken out again. Always positive.
CORE_EXPORT float UniformScreenScale(const LocalFrame& frame);

// The device pixel ratio documents of `frame` report and pick images for:
// kUniformDevicePixelRatio in MoltNet for web content (the top-level frame's
// origin is http(s) or opaque), `real` otherwise (other modes, chrome://
// and other internal pages, no frame). Cheap: called for every element
// whose style is resolved.
CORE_EXPORT double DevicePixelRatioFor(const LocalFrame* frame, double real);
CORE_EXPORT float DevicePixelRatioFor(const LocalFrame* frame, float real);

// The thickness in device pixels of a scrollbar of `width` that `theme`
// draws in `document`, where Blink asks the theme for it
// (theme.ScrollbarThickness(scale_from_dip, width) upstream). In MoltNet,
// for web content (as DevicePixelRatioFor() decides) and a classic theme:
// kUniformScrollbarThickness CSS pixels of `document` at its frame's layout
// zoom (kUniformThinScrollbarThickness for scrollbar-width: thin, 0 for
// none), UniformScrollbarDevicePixels() in farbling.h. Overlay scrollbars,
// which take no room in the layout, other modes, internal pages and no
// document: what the theme says. Custom (::-webkit-scrollbar) scrollbars
// never get here.
CORE_EXPORT int ScrollbarThicknessFor(const ScrollbarTheme& theme,
                                      float scale_from_dip,
                                      EScrollbarWidth width,
                                      const Document* document);

// The scrollbar the viewport units leave out of a root that always has one
// (overflow: scroll, scrollbar-gutter: stable; SmallerViewportUnits),
// PaintLayerScrollableArea::ComputeScrollbarWidthsForViewportUnits(): the
// theme's thickness in DIPs, taken as CSS pixels, upstream. In MoltNet, as
// above, the uniform thickness in CSS pixels, so 100vw is innerWidth - 16
// there, whatever the OS.
CORE_EXPORT int ScrollbarThicknessForViewportUnits(const ScrollbarTheme& theme,
                                                   EScrollbarWidth width,
                                                   const Document* document);

// The scale `area`'s scrollbars are drawn at (ScrollableArea::ScaleFromDIP();
// `scale_from_dip`, the device scale factor, upstream). In MoltNet, for web
// content with classic scrollbars, the layout zoom of `area`'s frame (device
// scale factor x page zoom), so their arrows, thumb and insets follow the
// page zoom as their thickness above does. `document` is `area`'s
// (ScrollableArea::GetDocument(), which only the area itself can call).
CORE_EXPORT float ScrollbarScaleFromDIP(const ScrollableArea& area,
                                        const Document& document,
                                        float scale_from_dip);

// A viewport size or length in CSS pixels of `frame` as the page reads it:
// in MoltNet, for web content, each length within kViewportSnapTolerance of
// a whole number of CSS pixels is that whole number (SnapViewportLength() in
// farbling.h); otherwise unchanged.
CORE_EXPORT gfx::SizeF UniformViewportSize(const LocalFrame& frame,
                                           const gfx::SizeF& css_size);
CORE_EXPORT double UniformViewportLength(const LocalFrame& frame,
                                         double css_length);

// kUniform can't be configured in blink_unittests (it pins the process's
// time zone and locale), so tests make this file's functions behave as in
// MoltNet with this (the screen size aside: UniformScreenSizeOf() is what
// UniformScreenRect() returns in kUniform, whatever the mode).
CORE_EXPORT void SetUniformForTesting(bool uniform);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_LETTERBOX_H_
