// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.h"

#include <algorithm>
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

}  // namespace blink::molt_fingerprinting::moltnet
