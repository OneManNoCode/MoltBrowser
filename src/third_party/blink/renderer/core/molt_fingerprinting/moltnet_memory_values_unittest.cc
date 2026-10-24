// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.h"

#include <cstdint>
#include <limits>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

constexpr uint64_t kMiB = uint64_t{1} << 20;
constexpr uint64_t kGiB = uint64_t{1} << 30;

TEST(MoltNetMemoryValuesTest, UniformLimitIsAnEightGigabyteMachines) {
  // 4 GiB of old generation and 3 x 32 MiB of young: what Chromium 156
  // reports, unrounded, on any 64-bit desktop with 8 GiB or more.
  EXPECT_EQ(UniformJsHeapSizeLimit(), uint64_t{4395630592});
  EXPECT_EQ(UniformJsHeapSizeLimit(), 4 * kGiB + 96 * kMiB);
  EXPECT_EQ(kUniformPhysicalMemory, 8 * kGiB);
  EXPECT_EQ(V8HeapSizeLimit(16 * kGiB), UniformJsHeapSizeLimit());
  EXPECT_EQ(V8HeapSizeLimit(64 * kGiB), UniformJsHeapSizeLimit());
}

TEST(MoltNetMemoryValuesTest, SmallerMachinesShowTheirMemory) {
  // Why MoltNet reports a fixed limit: below 8 GiB the old generation is
  // half the memory, to the 256 KiB page.
  EXPECT_EQ(V8HeapSizeLimit(4 * kGiB), 2 * kGiB + 96 * kMiB);
  EXPECT_EQ(V8HeapSizeLimit(2 * kGiB), 1 * kGiB + 48 * kMiB);
  EXPECT_EQ(V8HeapSizeLimit(1 * kGiB), 512 * kMiB + 24 * kMiB);
  // What an "8 GB" machine with memory reserved by the system reports.
  const uint64_t reported = 8268000000;
  const uint64_t old_generation = (reported / 2 + 256 * 1024 - 1) /
                                  (256 * 1024) * (256 * 1024);
  EXPECT_EQ(V8HeapSizeLimit(reported), old_generation + 96 * kMiB);
  EXPECT_NE(V8HeapSizeLimit(reported), V8HeapSizeLimit(reported + kMiB));
  // A semi-space between powers of two is rounded up (3 GiB: 24 -> 32 MiB).
  EXPECT_EQ(V8HeapSizeLimit(3 * kGiB), 1536 * kMiB + 96 * kMiB);
}

TEST(MoltNetMemoryValuesTest, Extremes) {
  // The minimum heap: 256 MiB of old generation, 2 MiB semi-spaces.
  EXPECT_EQ(V8HeapSizeLimit(0), 256 * kMiB + 6 * kMiB);
  EXPECT_EQ(V8HeapSizeLimit(1), 256 * kMiB + 6 * kMiB);
  EXPECT_EQ(V8HeapSizeLimit(std::numeric_limits<uint64_t>::max()),
            UniformJsHeapSizeLimit());
}

TEST(MoltNetMemoryValuesTest, ChromiumsRounding) {
  // Golden values of Chromium 156's QuantizeMemorySize (memory_info.cc),
  // computed by the function itself.
  EXPECT_EQ(QuantizedMemorySize(0), uint64_t{10000000});
  EXPECT_EQ(QuantizedMemorySize(1), uint64_t{10000000});
  EXPECT_EQ(QuantizedMemorySize(10000000), uint64_t{10000000});
  EXPECT_EQ(QuantizedMemorySize(10000001), uint64_t{10600000});
  EXPECT_EQ(QuantizedMemorySize(97400001), uint64_t{103000000});
  EXPECT_EQ(QuantizedMemorySize(2172649472), uint64_t{2190000000});
  EXPECT_EQ(QuantizedMemorySize(3760000000), uint64_t{3760000000});
  EXPECT_EQ(QuantizedMemorySize(3760000001), uint64_t{3760000000});
  EXPECT_EQ(QuantizedMemorySize(std::numeric_limits<uint64_t>::max()),
            uint64_t{3760000000});
  // What MoltNet reports as jsHeapSizeLimit.
  EXPECT_EQ(QuantizedMemorySize(UniformJsHeapSizeLimit()),
            uint64_t{3760000000});
  // Rounding never goes down, and stays on the table.
  uint64_t last = 0;
  for (uint64_t bytes = 1; bytes < 5000000000; bytes = bytes * 9 / 8 + 1) {
    const uint64_t rounded = QuantizedMemorySize(bytes);
    EXPECT_TRUE(rounded >= bytes || rounded == 3760000000) << bytes;
    EXPECT_GE(rounded, last);
    EXPECT_EQ(QuantizedMemorySize(rounded), rounded);
    last = rounded;
  }
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
