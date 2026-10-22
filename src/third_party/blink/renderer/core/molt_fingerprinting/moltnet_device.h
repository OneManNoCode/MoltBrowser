// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: what MoltNet windows (kUniform) say
// about the device beyond the screen, the GPU and the CPU count: the
// memory, the client hints a page's requests carry, and the high-entropy
// user-agent values. The fixed values are in moltnet_device_values.h.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   memory  NavigatorDeviceMemory::deviceMemory (window and workers)
//                                                      -> DeviceMemory
//   hints   ShouldSendClientHint (frame_fetch_context.cc; every client hint
//             a document's subresource requests carry)
//                                                      -> WithholdsClientHint
//   UA      NavigatorUA::userAgentData (navigator.userAgentData and
//             getHighEntropyValues(), window and workers)
//                                                -> UniformUserAgentMetadata
//
// The browser withholds the same hints on navigation requests (the
// documents themselves, iframes, prefetches, Critical-CH and ACCEPT_CH
// restarts) through the MoltNet profile's ClientHintsControllerDelegate
// (chrome/browser/molt_ai/fingerprinting/molt_net_client_hints.h), and makes
// the touch, pointer and hover values and the CPU performance tier uniform
// (molt_net_device.h).
//
// Which contexts: the whole MoltNet process, like navigator.hardwareConcurrency
// and the user agent itself (internal pages included; none of this matters
// to them). Normal and Incognito windows (kFarble) are left alone: these
// values can't usefully vary per site, and fixing them would only make a
// user look like other MoltBrowser users, which normal windows don't try.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DEVICE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DEVICE_H_

#include "services/network/public/mojom/web_client_hints_types.mojom-shared.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace blink {

struct UserAgentMetadata;

namespace molt_fingerprinting {

// navigator.deviceMemory: moltnet::kDeviceMemory (8) in MoltNet, else
// `real`.
CORE_EXPORT float DeviceMemory(float real);

// Whether a request must not carry client hint `type` although the page
// asked for it: in MoltNet, every hint but the low-entropy user-agent ones,
// Save-Data and the user-preference ones (moltnet::SendsClientHint()). A
// withheld hint is simply absent, as in browsers without client hints
// (Firefox, Safari, Tor Browser).
CORE_EXPORT bool WithholdsClientHint(network::mojom::WebClientHintsType type);

// navigator.userAgentData: in MoltNet, keeps the low-entropy values (brands
// with their major versions, mobile, platform) and blanks the high-entropy
// ones, as Chromium itself does when it hides them (a user agent set with
// --user-agent): architecture, bitness, model, platformVersion and
// uaFullVersion become "", fullVersionList and formFactors empty, wow64
// false. Otherwise leaves `metadata` alone.
CORE_EXPORT void UniformUserAgentMetadata(UserAgentMetadata& metadata);

// Makes the functions above behave as in kUniform without configuring the
// process for it (which would pin the time zone and locale for every later
// test).
CORE_EXPORT void SetMoltNetDeviceForTesting(bool uniform);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DEVICE_H_
