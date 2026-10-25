// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's uniform device, browser side: the values the browser decides
// for MoltNet windows (molt_net_device.h, molt_net_client_hints.h). Std-only
// and constexpr, so molt_ai_unittests checks them standalone and against
// the enums they mirror (molt_net_device.cc and molt_net_client_hints.cc
// static_assert the same against the real ones).
//
// The client-hint rule is Blink's (moltnet_device_values.h in
// third_party/blink/renderer/core/molt_fingerprinting/), duplicated because
// browser code can't include renderer headers;
// molt_net_device_values_unittest.cc checks that the two agree.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_VALUES_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_VALUES_H_

#include <cstddef>
#include <cstdint>

namespace molt_ai::fingerprinting {

// --- Client hints ------------------------------------------------------------

// Whether MoltNet windows send the client hint whose
// network::mojom::WebClientHintsType value is `hint`: only Sec-CH-UA,
// Sec-CH-UA-Mobile, Sec-CH-UA-Platform, Save-Data and the
// Sec-CH-Prefers-{Color-Scheme,Reduced-Motion,Reduced-Transparency} hints.
// Everything else (DPR, viewport, width, memory, network quality, the
// high-entropy user agent, and any later hint) is withheld.
constexpr bool MoltNetSendsClientHint(int32_t hint) {
  switch (hint) {
    case 8:   // kUA
    case 10:  // kUAPlatform
    case 12:  // kUAMobile
    case 15:  // kPrefersColorScheme
    case 27:  // kSaveData
    case 28:  // kPrefersReducedMotion
    case 30:  // kPrefersReducedTransparency
      return true;
    default:
      return false;
  }
}

// --- Touch, pointer and hover ------------------------------------------------
//
// A desktop with a mouse and no touchscreen, for every MoltNet user:
// navigator.maxTouchPoints 0, the pointer / any-pointer media queries "fine",
// hover / any-hover "hover", and no touch-event feature detection
// ('ontouchstart' in window is false, document.createEvent('TouchEvent')
// throws). Real touch input still works (see molt_net_device.h).

inline constexpr bool kMoltNetTouchEventFeatureDetection = false;
inline constexpr int kMoltNetMaxTouchPoints = 0;
// ui::POINTER_TYPE_FINE, the bit set of available pointers.
inline constexpr int kMoltNetAvailablePointerTypes = 1 << 2;
// blink::mojom::PointerType::kPointerFineType.
inline constexpr int kMoltNetPrimaryPointerType = 4;
// ui::HOVER_TYPE_HOVER, the bit set of available hover types.
inline constexpr int kMoltNetAvailableHoverTypes = 1 << 1;
// blink::mojom::HoverType::kHoverHoverType.
inline constexpr int kMoltNetPrimaryHoverType = 2;

// --- navigator.cpuPerformance ------------------------------------------------

// blink::mojom::PerformanceTier::kUnknown (0): the API's "no information",
// which pages must already handle (CPUs the tier tables don't know report
// it). The real tier (1 to 4) comes from the CPU model.
inline constexpr int kMoltNetCpuPerformanceTier = 0;

// --- Renderer processes
// --------------------------------------------------------

// The number of renderer processes from which a MoltNet frame tries to share
// an existing process of its site before starting a new one. Chromium starts
// sharing from a number that follows the RAM (content's
// GetMaxRendererProcessCount(): half the RAM in MiB over 85, at least 3), so
// whether two same-site MoltNet tabs share a process, and so what their
// scripts can observe of each other's load, depended on the machine. MoltNet
// uses 20 on every machine: at or below Chromium's own number on every machine
// MoltBrowser supports (4 GB gives 20 to 24, as the firmware and GPU keep
// some of it), so it never shares later than Chromium would, and the
// same-site sharing changes nothing for sites (site isolation still keeps
// different sites in different processes).
inline constexpr size_t kMoltNetRendererProcessLimit = 20;

// Chromium's limit for a machine with `memory_mib` MiB of RAM on 64-bit
// desktop, before its platform cap (the unit test's reference).
constexpr size_t ChromiumRendererProcessLimit(size_t memory_mib) {
  const size_t count = memory_mib / 2 / 85;
  return count < 3 ? 3 : count;
}

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_VALUES_H_
