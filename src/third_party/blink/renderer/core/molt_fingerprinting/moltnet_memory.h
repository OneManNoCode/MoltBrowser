// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: what MoltNet windows (kUniform)
// report about memory. The values are in moltnet_memory_values.h.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   performance.memory   MemoryInfo's constructor (core/timing/memory_info.cc)
//                          -> UsesUniformMemoryInfo, UniformJsHeapSizeLimit
//   measureUserAgent-    ConvertResult (core/timing/measure_memory/
//   SpecificMemory()       measure_memory_controller.cc)
//                          -> UniformMemoryMeasurementBytes
//
// performance.memory: Chromium reports precise values (refreshed every 50 ms)
// when the renderer is locked to a site, as on desktop, or with
// --enable-precise-memory-info, and otherwise rounds them to one of 100
// sizes and refreshes them once every 20 minutes. MoltNet always does the
// latter, and its jsHeapSizeLimit is that of the 8 GB machine
// navigator.deviceMemory describes, rounded the same way. The used and total
// sizes stay the page's own, rounded.
//
// measureUserAgentSpecificMemory() (cross-origin isolated pages, windows,
// shared and service workers): every byte count is rounded the same way (a
// zero stays zero), as precise counts would show the heap's layout and when
// the machine collects garbage.
//
// Which contexts: the whole MoltNet renderer process (performance.memory
// only exists in windows). Normal and Incognito windows are unchanged.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_H_

#include <cstddef>
#include <cstdint>

#include "third_party/blink/renderer/core/core_export.h"

namespace blink::molt_fingerprinting {

// Whether performance.memory reports MoltNet's values: rounded, refreshed
// every 20 minutes, the uniform heap limit. Any thread.
CORE_EXPORT bool UsesUniformMemoryInfo();

// performance.memory.jsHeapSizeLimit in MoltNet: the uniform machine's,
// rounded like every performance.memory value (moltnet::QuantizedMemorySize,
// Chromium's QuantizeMemorySize).
CORE_EXPORT size_t UniformJsHeapSizeLimit();

// A byte count measureUserAgentSpecificMemory() reports: rounded in
// MoltNet like performance.memory's (0 stays 0), `bytes` otherwise. Any
// thread.
CORE_EXPORT uint64_t UniformMemoryMeasurementBytes(uint64_t bytes);

// Makes the functions above behave as in kUniform without configuring the
// process for it.
CORE_EXPORT void SetMoltNetMemoryForTesting(bool uniform);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_H_
