// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory.h"

#include <cstdint>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_testing.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.h"
#include "third_party/blink/renderer/core/timing/memory_info.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"

namespace blink::molt_fingerprinting {
namespace {

class MoltNetMemoryTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetMemoryForTesting(true);
  }
  void TearDown() override {
    SetMoltNetMemoryForTesting(false);
    ResetForTesting();
  }

  test::TaskEnvironment task_environment_;
};

bool IsBucket(size_t bytes) {
  return moltnet::QuantizedMemorySize(bytes) == bytes;
}

// performance.memory: the 8 GB machine's heap limit, the page's own sizes
// rounded, whatever precision the caller asked for.
TEST_F(MoltNetMemoryTest, PerformanceMemory) {
  V8TestingScope scope;
  for (const MemoryInfo::Precision precision :
       {MemoryInfo::Precision::kPrecise, MemoryInfo::Precision::kBucketized}) {
    auto* info = MakeGarbageCollected<MemoryInfo>(precision);
    EXPECT_EQ(info->jsHeapSizeLimit(), 3760000000u);
    EXPECT_TRUE(IsBucket(info->usedJSHeapSize()));
    EXPECT_TRUE(IsBucket(info->totalJSHeapSize()));
    EXPECT_LE(info->totalJSHeapSize(), info->jsHeapSizeLimit());
  }
}

TEST_F(MoltNetMemoryTest, Values) {
  EXPECT_EQ(UniformJsHeapSizeLimit(), 3760000000u);
  EXPECT_EQ(UniformHeapSize(0), 10000000u);
  EXPECT_EQ(UniformHeapSize(389472983), 410000000u);
  // measureUserAgentSpecificMemory(): rounded, a zero stays zero.
  EXPECT_EQ(UniformMemoryMeasurementBytes(0), 0u);
  EXPECT_EQ(UniformMemoryMeasurementBytes(1), 10000000u);
  EXPECT_EQ(UniformMemoryMeasurementBytes(38947298), 39600000u);
  SetMoltNetMemoryForTesting(false);
  EXPECT_FALSE(UsesUniformMemoryInfo());
  EXPECT_EQ(UniformMemoryMeasurementBytes(38947298), 38947298u);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
