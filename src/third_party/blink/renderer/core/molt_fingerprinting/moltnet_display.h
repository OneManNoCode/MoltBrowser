// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the display MoltNet windows
// (kUniform) describe beyond its size and density (letterbox.h): its colour
// (the color-gamut, dynamic-range, video-dynamic-range, color and
// monochrome media queries) and the style of its scrollbars (overlay or
// classic). The fixed values are in moltnet_values.h.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   colour      MediaValues::CalculateColorGamut, CalculateDeviceSupportsHDR,
//                 CalculateColorBitsPerComponent,
//                 CalculateMonochromeBitsPerComponent (media_values.cc; every
//                 media query evaluation, matchMedia, <source media>, the
//                 preload scanner)                   -> UniformDisplay*()
//   scrollbars  WebViewImpl::UpdateRendererPreferences (web_view_impl.cc;
//                 RendererPreferences::use_overlay_scrollbar, the browser's
//                 overlay-or-classic decision for every desktop OS at 156)
//                                                    -> UniformOverlayScrollbars
//
// Which contexts: the whole MoltNet renderer process, like screen.colorDepth
// and the screen values. A renderer has one scrollbar style for all its
// pages (the web NativeTheme and Blink's scrollbar settings are process
// globals), and MoltNet's renderers only host MoltNet pages. DevTools'
// media-feature emulation still applies on top (the colour gamut override
// wins), and so does its own scrollbar emulation.
//
// Normal and Incognito windows (kFarble) are left alone: none of these
// values can usefully vary per site, and fixing them would only make a user
// look like other MoltBrowser users, which normal windows don't try.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DISPLAY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DISPLAY_H_

#include <optional>

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/graphics/color_space_gamut.h"

namespace blink::molt_fingerprinting {

// The colour the media queries report in MoltNet: an sRGB, standard
// dynamic range, 8-bit colour display. nullopt outside MoltNet (the caller
// keeps the real screen's value).
CORE_EXPORT std::optional<ColorSpaceGamut> UniformDisplayColorGamut();
CORE_EXPORT std::optional<bool> UniformDisplaySupportsHDR();
CORE_EXPORT std::optional<int> UniformDisplayColorBitsPerComponent();
CORE_EXPORT std::optional<int> UniformDisplayMonochromeBitsPerComponent();

// RendererPreferences::use_overlay_scrollbar as a MoltNet renderer applies
// it: the platform's default style (overlay on macOS, classic on Windows
// and Linux; moltnet::UsesOverlayScrollbars()), whatever the browser sent
// from the OS setting. `requested` outside MoltNet.
CORE_EXPORT bool UniformOverlayScrollbars(bool requested);

// Makes the functions above behave as in kUniform without configuring the
// process for it (which would pin the time zone and locale for every later
// test).
CORE_EXPORT void SetMoltNetDisplayForTesting(bool uniform);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DISPLAY_H_
