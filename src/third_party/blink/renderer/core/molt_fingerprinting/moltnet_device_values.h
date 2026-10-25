// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: what MoltNet windows say about the
// device in navigator.deviceMemory and in client hints. Pure and std-only
// (tested standalone, like farbling.h); the Blink side is moltnet_device.h.
// The browser applies the same client-hint rule to navigation requests
// (chrome/browser/molt_ai/fingerprinting/molt_net_device_values.h, checked
// against this file by its unit test).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DEVICE_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DEVICE_VALUES_H_

#include <cstdint>

namespace blink::molt_fingerprinting::moltnet {

// --- navigator.deviceMemory --------------------------------------------------

// The real value is the RAM rounded to the nearest power of two, in [2, 32]
// GiB on desktop (crbug.com/454354290 raised the cap from 8). MoltNet reports
// 4 for everybody: the machine every MoltNet isolate's V8 heap is configured
// as (moltnet_memory_values.h, kV8HeapPhysicalMemory), so that the value and
// the heap a page can fill or read in performance.memory describe one
// machine, as they do in Chromium. It is what Chromium reports on every
// machine with a little over 3 to 6 GiB, it is at or below the RAM of every
// machine MoltBrowser supports (4 GB is the smallest), and it is above the 1
// GiB at and below which Chromium's own tests see sites serve lighter pages
// (approximated_device_memory.cc).
inline constexpr float kDeviceMemory = 4.0f;

// --- Client hints ------------------------------------------------------------

// Whether MoltNet windows send the client hint whose
// network::mojom::WebClientHintsType value is `hint`. An allow-list: the
// low-entropy defaults sites rely on (Sec-CH-UA, Sec-CH-UA-Mobile,
// Sec-CH-UA-Platform, Save-Data) and the user-preference hints, which only
// repeat media queries every page can evaluate (Sec-CH-Prefers-Color-Scheme,
// -Reduced-Motion, -Reduced-Transparency). Everything else is never sent,
// whatever the site asks for in Accept-CH, <meta> or delegation: the device
// hints (DPR, Viewport-Width/Height, Sec-CH-Width, Device-Memory), the
// network-quality hints (RTT, Downlink, ECT) and the high-entropy
// user-agent hints (architecture, bitness, model, platform version, full
// versions, WoW64, form factors), and any hint a later Chromium adds.
bool SendsClientHint(int32_t hint);

// The values of the allowed hints in network::mojom::WebClientHintsType
// (services/network/public/mojom/web_client_hints_types.mojom;
// moltnet_device.cc checks them against the generated enum).
inline constexpr int32_t kHintUA = 8;
inline constexpr int32_t kHintUAPlatform = 10;
inline constexpr int32_t kHintUAMobile = 12;
inline constexpr int32_t kHintPrefersColorScheme = 15;
inline constexpr int32_t kHintSaveData = 27;
inline constexpr int32_t kHintPrefersReducedMotion = 28;
inline constexpr int32_t kHintPrefersReducedTransparency = 30;

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_DEVICE_VALUES_H_
