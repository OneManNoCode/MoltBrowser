// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's uniform device, browser side: the input devices and the CPU
// performance tier MoltNet pages see (the values are in
// molt_net_device_values.h; Blink's side, the memory, client hints and
// user-agent values, is third_party/blink/renderer/core/molt_fingerprinting/
// moltnet_device.h; client hints on navigation requests are
// molt_net_client_hints.h).
//
// Hooks in chrome/browser/chrome_content_browser_client.cc (each marked
// "MoltBrowser:"):
//
//   OverrideWebPreferences         -> OverrideMoltNetWebPreferences
//   GetCpuPerformanceTierOverride  -> MoltNetCpuPerformanceTier
//
// Why WebPreferences. At 152 everything a page can learn about the
// touchscreen without being touched comes from six WebPreferences fields,
// which content fills from the real devices (SlowWebPreferenceCache: ui::
// GetAvailablePointerAndHoverTypes(), ui::MaxTouchPoints(), the
// --touch-events switch) and then hands to the embedder's
// OverrideWebPreferences on every recomputation, including the one a
// device change (a touchscreen plugged in, tablet mode) triggers. Blink
// reads them back through Settings: navigator.maxTouchPoints
// (NavigatorEvents), the pointer / any-pointer / hover / any-hover media
// queries (MediaValues), and touch-event feature detection (the
// RuntimeEnabledFeature behind the ontouch* attributes and
// document.createEvent('TouchEvent')). One browser-side override covers all
// of them, in every frame and process of the profile, and keeps the real
// values out of MoltNet renderers altogether, without a Blink hook.
//
// Real touch input is unaffected: input routing doesn't read these fields
// (the other readers found at 152 are DevTools' touch emulation and the
// media controls' initial mouse-or-touch guess). A touch still scrolls,
// zooms and taps; pages get it the way Chromium on a desktop delivers it
// when touch-event feature detection is off, its default on desktop:
// pointer events (pointerType "touch"), the compatibility mouse events and
// click of a tap, and touch events for listeners added with
// addEventListener. So a page that waits for the user to touch the screen
// still learns that it is a touchscreen; see docs/FINGERPRINTING.md for why
// that isn't hidden too.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_H_

#include <optional>

namespace blink::web_pref {
struct WebPreferences;
}  // namespace blink::web_pref

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace molt_ai::fingerprinting {

// End of ChromeContentBrowserClient::OverrideWebPreferences: in MoltNet
// profiles (RendererMode::kUniform, so not when the kill switch is off),
// ApplyMoltNetInputPreferences(). Other profiles are left alone.
void OverrideMoltNetWebPreferences(content::WebContents* web_contents,
                                   blink::web_pref::WebPreferences* prefs);

// A mouse-only desktop: no touch-event feature detection, no touch points,
// a fine pointer that can hover (molt_net_device_values.h).
void ApplyMoltNetInputPreferences(blink::web_pref::WebPreferences& prefs);

// ChromeContentBrowserClient::GetCpuPerformanceTierOverride, before the
// user's and the enterprise override: kMoltNetCpuPerformanceTier ("unknown")
// for MoltNet profiles, so navigator.cpuPerformance doesn't tell CPUs
// apart; nullopt otherwise.
std::optional<int> MoltNetCpuPerformanceTier(content::BrowserContext* context);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_H_
