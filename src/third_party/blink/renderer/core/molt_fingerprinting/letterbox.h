// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: MoltNet letterboxing, Blink side.
//
// In MoltNet windows the browser letterboxes the page: its content area is
// rounded down to the letterbox steps of farbling.h and centred
// (chrome/browser/molt_ai/fingerprinting/molt_letterbox.h). This file keeps
// every size and density the page can read consistent with that:
//
//   * UniformScreenSizeOf() is the size UniformScreenRect()
//     (molt_fingerprinting.h) reports for screen.* : the outermost main
//     frame's size, *including* its scrollbars and rounded like innerWidth /
//     innerHeight, then bucketed. Once the browser has letterboxed the page,
//     screen.width/height equal innerWidth/innerHeight at 100% zoom.
//   * CSS device-width / device-height / device-aspect-ratio report the same
//     screen (hooked in MediaValues, through UniformScreenRect()).
//   * The device pixel ratio web content sees is kUniformDevicePixelRatio:
//     window.devicePixelRatio, the resolution / -webkit-device-pixel-ratio
//     media queries (matchMedia, <source media>, the preload scanner) and the
//     density srcset / <picture> and image-set() candidates are picked for.
//     Only what the page can read changes: Blink still renders at the real
//     scale, so text and CSS stay sharp.
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
//
// Normal and Incognito windows (kFarble) are left alone: their goal is
// per-site unlinkability, and a fixed screen or density would only make
// users look like other MoltBrowser users (docs/FINGERPRINTING.md).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_LETTERBOX_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_LETTERBOX_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "ui/gfx/geometry/size.h"

namespace blink {

class LocalFrame;

namespace molt_fingerprinting {

// The device pixel ratio every MoltNet page reports: one CSS pixel per
// device pixel, as on a standard-density screen (Tor Browser's choice too).
inline constexpr double kUniformDevicePixelRatio = 1.0;

// The uniform screen size for documents of `frame`, whatever the mode: the
// outermost main frame's size in DIPs, scrollbars included and rounded the
// way innerWidth / innerHeight round, then LetterboxBucket()ed. A frame in
// another process than the main frame only knows the main frame's size
// without its scrollbars (docs/FINGERPRINTING.md).
CORE_EXPORT gfx::Size UniformScreenSizeOf(LocalFrame& frame);

// The device pixel ratio documents of `frame` report and pick images for:
// kUniformDevicePixelRatio in MoltNet for web content (the top-level frame's
// origin is http(s) or opaque), `real` otherwise (other modes, chrome://
// and other internal pages, no frame). Cheap: called for every element
// whose style is resolved.
CORE_EXPORT double DevicePixelRatioFor(const LocalFrame* frame, double real);
CORE_EXPORT float DevicePixelRatioFor(const LocalFrame* frame, float real);

// kUniform can't be configured in blink_unittests (it pins the process's
// time zone and locale), so tests make DevicePixelRatioFor() behave as in
// MoltNet with this.
CORE_EXPORT void SetUniformDevicePixelRatioForTesting(bool uniform);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_LETTERBOX_H_
