// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the memory MoltNet windows describe
// to performance.memory, the same for every MoltNet user. Pure and std-only
// (tested standalone, like moltnet_media_values.h); the Blink side is
// moltnet_memory.h.
//
// performance.memory.jsHeapSizeLimit is V8's heap limit, which V8 sizes from
// the machine's physical memory: at Chromium 156 half of it, up to 4 GiB,
// for the old generation, rounded to V8's 256 KiB pages. On machines with
// less than 8 GiB it gives the memory away to half a megabyte, and
// Chromium reports it unrounded whenever the renderer is locked to a site
// (as on desktop). MoltNet reports the limit of the machine
// navigator.deviceMemory describes, 8 GB.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_VALUES_H_

#include <cstdint>

namespace blink::molt_fingerprinting::moltnet {

// The physical memory of the machine MoltNet describes: the 8 GB of
// navigator.deviceMemory (moltnet_device_values.h), in bytes.
inline constexpr uint64_t kUniformPhysicalMemory = uint64_t{8} << 30;

// v8::HeapStatistics::heap_size_limit() on a 64-bit desktop with
// `physical_memory` bytes, as V8 at Chromium 156 computes it from the
// physical memory gin passes (ResourceConstraints::ConfigureDefaults,
// Heap::ConfigureHeap, Heap::MaxReserved, with no V8 flags): an old
// generation of half the memory within [256 MiB, 4 GiB], and two semi-spaces
// and a new large-object space of 1/128 of it within [2 MiB, 32 MiB] each,
// sizes rounded to 256 KiB pages (and the semi-space up to a power of two).
uint64_t V8HeapSizeLimit(uint64_t physical_memory);

// The heap limit MoltNet reports (before Chromium's usual rounding of
// performance.memory values): an 8 GiB machine's, 4 GiB + 96 MiB.
uint64_t UniformJsHeapSizeLimit();

// Chromium's rounding of performance.memory values when they aren't precise
// (core/timing/memory_info.cc, QuantizeMemorySize): up to the next of 100
// sizes from 10 MB to 3.76 GB, three significant digits each (the last one
// for anything larger). The same table, fixed rather than computed in
// floating point at run time, and safe on any thread.
uint64_t QuantizedMemorySize(uint64_t bytes);

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_MEMORY_VALUES_H_
