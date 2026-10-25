// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_memory_valve_policy.h"

#include <cstdint>

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

constexpr uint64_t kReserve = static_cast<uint64_t>(
    molt_ai::fingerprinting::kMoltNetStorageMemoryReserveBytes);
constexpr uint64_t kMiB = 1024 * 1024;

// One reading at a time, at most every interval, and none once ended.
TEST(MoltNetMemoryValvePolicyTest, Reads) {
  EXPECT_TRUE(MoltNetMemoryValveReads(false, false,
                                      kMoltNetMemoryValveIntervalMs));
  EXPECT_TRUE(MoltNetMemoryValveReads(false, false, 1000000));
  EXPECT_FALSE(MoltNetMemoryValveReads(false, false,
                                       kMoltNetMemoryValveIntervalMs - 1));
  EXPECT_FALSE(MoltNetMemoryValveReads(false, true, 1000000));
  EXPECT_FALSE(MoltNetMemoryValveReads(true, false, 1000000));
  // A clock that went back (or the first reading, from 0): only forward.
  EXPECT_FALSE(MoltNetMemoryValveReads(false, false, -1));
}

// Ends below the reserve; never for an unknown reading.
TEST(MoltNetMemoryValvePolicyTest, Ends) {
  EXPECT_FALSE(MoltNetMemoryValveEnds(0, kReserve));
  EXPECT_TRUE(MoltNetMemoryValveEnds(1, kReserve));
  EXPECT_TRUE(MoltNetMemoryValveEnds(kReserve - 1, kReserve));
  EXPECT_FALSE(MoltNetMemoryValveEnds(kReserve, kReserve));
  EXPECT_FALSE(MoltNetMemoryValveEnds(kReserve + 512 * kMiB, kReserve));
}

// Readings stay rare: at most five a second while sites grow past a floor,
// and each is a cheap system call on a worker.
TEST(MoltNetMemoryValvePolicyTest, Interval) {
  static_assert(kMoltNetMemoryValveIntervalMs >= 100);
  static_assert(kMoltNetMemoryValveIntervalMs <= 1000);
  EXPECT_EQ(kMoltNetMemoryValveIntervalMs, 200);
}

}  // namespace
}  // namespace molt_ai::tor
