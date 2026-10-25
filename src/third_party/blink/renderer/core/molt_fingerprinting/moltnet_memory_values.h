// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the memory MoltNet's renderers give
// V8 and describe to performance.memory, the same for every MoltNet user.
// Pure and std-only (tested standalone, like moltnet_media_values.h); the
// Blink side is moltnet_memory.h and moltnet_cpu.h.
//
// V8 sizes each isolate's heap from the physical memory gin passes it: at
// Chromium 156 an old generation of half the memory, up to 4 GiB, rounded
// to V8's 256 KiB pages, and its garbage collector's heuristics follow the
// same figure. A page sees it in performance.memory.jsHeapSizeLimit, which
// Chromium reports unrounded whenever the renderer is locked to a site (as
// on desktop), in where allocation runs out of heap and in how often the
// collector runs. MoltNet's renderers configure every isolate as on one
// machine, the smallest MoltBrowser supports (kV8HeapPhysicalMemory, set by
// moltnet_cpu.h before V8 starts), and report that machine's limit.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_VALUES_H_

#include <cstdint>

namespace blink::molt_fingerprinting::moltnet {

// The physical memory every isolate of a MoltNet renderer is configured for,
// in bytes: 4 GiB, the smallest machine MoltBrowser supports (README,
// "Hardware Requirements"). It is the configuration V8 already picks on that
// machine, so a page can reach the heap limit there as on any other; larger
// machines run as that one. navigator.deviceMemory describes the same machine
// (moltnet::kDeviceMemory, 4).
inline constexpr uint64_t kV8HeapPhysicalMemory = uint64_t{4} << 30;

// v8::HeapStatistics::heap_size_limit() on a 64-bit desktop with
// `physical_memory` bytes, as V8 at Chromium 156 computes it from the
// physical memory gin passes (ResourceConstraints::ConfigureDefaults,
// Heap::ConfigureHeap, Heap::MaxReserved, with no V8 flags): an old
// generation of half the memory within [256 MiB, 4 GiB], and two semi-spaces
// and a new large-object space of 1/128 of it within [2 MiB, 32 MiB] each,
// sizes rounded to 256 KiB pages (and the semi-space up to a power of two).
uint64_t V8HeapSizeLimit(uint64_t physical_memory);

// MoltNet's real heap limit, which it reports (before Chromium's usual
// rounding of performance.memory values): the kV8HeapPhysicalMemory
// machine's, 2 GiB + 96 MiB.
uint64_t UniformJsHeapSizeLimit();

// Chromium's rounding of performance.memory values when they aren't precise
// (core/timing/memory_info.cc, QuantizeMemorySize): up to the next of 100
// sizes from 10 MB to 3.76 GB, three significant digits each (the last one
// for anything larger). The same table, fixed rather than computed in
// floating point at run time, and safe on any thread.
uint64_t QuantizedMemorySize(uint64_t bytes);

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_VALUES_H_
