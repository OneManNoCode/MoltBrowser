// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.h"

#include <algorithm>
#include <array>
#include <bit>

namespace blink::molt_fingerprinting::moltnet {

namespace {

constexpr uint64_t kKB = 1024;
constexpr uint64_t kMB = kKB * 1024;
constexpr uint64_t kGB = kMB * 1024;

// V8's NormalPage::kPageSize on desktop (kPageSizeBits = 18).
constexpr uint64_t kPage = 256 * kKB;
// Heap::kDefaultMinHeapSize, kDefaultMaxHeapSize (64-bit), and the pointer-
// compression cage that caps the old generation
// (kAllocatorLimitOnMaxOldGenerationSize).
constexpr uint64_t kMinOldGeneration = 256 * kMB;
constexpr uint64_t kMaxOldGeneration = 4 * kGB;
constexpr uint64_t kCage = 4 * kGB;
// Heap::HeapSizeToSemiSpaceRatio and DefaultMaxSemiSpaceSize (the scavenger,
// not on Android), DefaultMinSemiSpaceSize.
constexpr uint64_t kHeapToSemiSpace = 32;
constexpr uint64_t kMinSemiSpace = 2 * kMB;
constexpr uint64_t kMaxSemiSpace = 32 * kMB;
constexpr uint64_t kSmallestSemiSpace = 512 * kKB;
// Two semi-spaces and a new large-object space of the same size.
constexpr uint64_t kYoungSpaces = 3;

// QuantizeMemorySize's buckets: 100 steps from 10 MB growing by
// (4 GB / 10 MB)^(1/100) in float, cut to three significant digits.
constexpr std::array<uint64_t, 100> kMemoryBuckets = {
    10000000,   10600000,   11200000,   11900000,   12700000,   13400000,
    14300000,   15200000,   16100000,   17100000,   18200000,   19300000,
    20500000,   21700000,   23100000,   24500000,   26000000,   27600000,
    29400000,   31200000,   33100000,   35100000,   37300000,   39600000,
    42100000,   44700000,   47400000,   50400000,   53500000,   56800000,
    60300000,   64000000,   68000000,   72200000,   76600000,   81400000,
    86400000,   91700000,   97400000,   103000000,  109000000,  116000000,
    123000000,  131000000,  139000000,  148000000,  157000000,  167000000,
    177000000,  188000000,  199000000,  212000000,  225000000,  239000000,
    254000000,  269000000,  286000000,  304000000,  322000000,  342000000,
    364000000,  386000000,  410000000,  435000000,  462000000,  491000000,
    521000000,  553000000,  588000000,  624000000,  662000000,  703000000,
    747000000,  793000000,  842000000,  894000000,  949000000,  1000000000,
    1070000000, 1130000000, 1200000000, 1280000000, 1360000000, 1440000000,
    1530000000, 1620000000, 1720000000, 1830000000, 1940000000, 2060000000,
    2190000000, 2330000000, 2470000000, 2620000000, 2790000000, 2960000000,
    3140000000, 3340000000, 3540000000, 3760000000,
};

uint64_t RoundUp(uint64_t value, uint64_t unit) {
  return (value + unit - 1) / unit * unit;
}

uint64_t RoundDown(uint64_t value, uint64_t unit) {
  return value / unit * unit;
}

}  // namespace

uint64_t V8HeapSizeLimit(uint64_t physical_memory) {
  // ResourceConstraints::ConfigureDefaults:
  // Heap::OldGenerationSizeFromPhysicalMemory (64-bit: half the memory) and
  // Heap::YoungGenerationSizeFromPhysicalMemory (a quarter, over 32 per
  // semi-space).
  const uint64_t old_generation = RoundUp(
      std::clamp(physical_memory / 2, kMinOldGeneration, kMaxOldGeneration),
      kPage);
  const uint64_t semi_space = RoundUp(
      std::clamp(physical_memory / 4 / kHeapToSemiSpace, kMinSemiSpace,
                 kMaxSemiSpace),
      kPage);
  const uint64_t young_generation = semi_space * kYoungSpaces;

  // Heap::ConfigureHeap.
  uint64_t max_semi_space = std::bit_ceil(young_generation / kYoungSpaces);
  max_semi_space = std::max(max_semi_space, RoundUp(kSmallestSemiSpace, kPage));
  max_semi_space = std::min(RoundDown(max_semi_space, kPage), kCage / 2);
  const uint64_t max_old_generation =
      RoundDown(std::min(old_generation, kCage), kPage);

  // Heap::MaxReserved.
  return kYoungSpaces * max_semi_space + max_old_generation;
}

uint64_t UniformJsHeapSizeLimit() {
  return V8HeapSizeLimit(kUniformPhysicalMemory);
}

uint64_t QuantizedMemorySize(uint64_t bytes) {
  const auto bucket = std::lower_bound(kMemoryBuckets.begin(),
                                       kMemoryBuckets.end(), bytes);
  return bucket == kMemoryBuckets.end() ? kMemoryBuckets.back() : *bucket;
}

}  // namespace blink::molt_fingerprinting::moltnet
