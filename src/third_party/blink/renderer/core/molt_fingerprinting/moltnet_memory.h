// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: what MoltNet windows (kUniform)
// report about memory. The values are in moltnet_memory_values.h.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   performance.memory   MemoryInfo's constructor and HeapSizeCache::Update
//                          (core/timing/memory_info.cc)
//                          -> UsesUniformMemoryInfo, UniformJsHeapSizeLimit,
//                             UniformHeapSize
//   measureUserAgent-    MeasurementComplete and ConvertResult
//   SpecificMemory()       (core/timing/measure_memory/
//                          measure_memory_controller.cc)
//                          -> MakeMemoryMeasurementUniform,
//                             UniformMemoryMeasurementBytes
//
// performance.memory: Chromium reports precise values (refreshed every 50 ms)
// when the renderer is locked to a site, as on desktop, or with
// --enable-precise-memory-info, and otherwise rounds them to one of 100
// sizes and refreshes them once every 20 minutes. MoltNet always does the
// latter, and its jsHeapSizeLimit is the real limit of the machine every
// MoltNet isolate is configured for (moltnet::kV8HeapPhysicalMemory,
// moltnet_cpu.h), rounded the same way. The used and total sizes stay the
// page's own, rounded. MoltNet rounds with a fixed table of Chromium's 100
// sizes (moltnet::QuantizedMemorySize) rather than computing them with the
// platform's floating-point library as Chromium does.
//
// measureUserAgentSpecificMemory() (cross-origin isolated pages, windows,
// shared and service workers): every byte count is rounded the same way (a
// zero stays zero), as precise counts would show the heap's layout and when
// the machine collects garbage. And every context the browser lists gets the
// same entries, a "JavaScript" and a "Canvas" one, whether its process
// measured it, it lives in another process (whose memory counts as 0) or it
// has no canvas: Chromium leaves out a context in this process that wasn't
// measured, and a Canvas entry for one without canvas buffers, so which
// entries a page gets depends on how its frames are spread over processes.
//
// Which contexts: the whole MoltNet renderer process (performance.memory
// only exists in windows). Normal and Incognito windows are unchanged.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_H_

#include <cstddef>
#include <cstdint>

#include "components/performance_manager/public/mojom/web_memory.mojom-blink-forward.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace blink::molt_fingerprinting {

// Whether performance.memory reports MoltNet's values: rounded, refreshed
// every 20 minutes, the uniform heap limit. Any thread.
CORE_EXPORT bool UsesUniformMemoryInfo();

// performance.memory.jsHeapSizeLimit in MoltNet: the uniform machine's,
// rounded like every performance.memory value (moltnet::QuantizedMemorySize,
// Chromium's QuantizeMemorySize).
CORE_EXPORT size_t UniformJsHeapSizeLimit();

// performance.memory's used or total heap size in MoltNet: `bytes` rounded
// up to the next of the 100 sizes (moltnet::QuantizedMemorySize). Any thread.
CORE_EXPORT size_t UniformHeapSize(size_t bytes);

// A byte count measureUserAgentSpecificMemory() reports: rounded in
// MoltNet like performance.memory's (0 stays 0), `bytes` otherwise. Any
// thread.
CORE_EXPORT uint64_t UniformMemoryMeasurementBytes(uint64_t bytes);

// In MoltNet, gives every breakdown entry of the browser's `measurement` a
// JavaScript and a canvas size (0 where there was none), and the
// measurement its shared and Blink sizes (0 where there was none), so that
// measureUserAgentSpecificMemory() lists the same entries for the same
// contexts whatever the process model. Nothing otherwise.
CORE_EXPORT void MakeMemoryMeasurementUniform(
    performance_manager::mojom::blink::WebMemoryMeasurement& measurement);

// Whether the isolates of this process ignore the system's memory pressure
// (MemoryPressureNotificationToAllIsolates, core/workers/
// worker_backing_thread.cc, which the content layer calls when the browser's
// memory coordinator asks this renderer to release memory). True in MoltNet:
// the browser signals pressure from the machine's free memory, to every
// renderer at once, and the garbage collections it triggers show in what
// pages observe of collection (FinalizationRegistry callbacks, WeakRefs
// emptied), at the same moment in every tab, on low-memory machines more
// often. V8 still collects as its own heuristics for the 4 GB machine decide.
// Any thread.
CORE_EXPORT bool IgnoresSystemMemoryPressure();

// Makes the functions above behave as in kUniform without configuring the
// process for it.
CORE_EXPORT void SetMoltNetMemoryForTesting(bool uniform);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_H_
