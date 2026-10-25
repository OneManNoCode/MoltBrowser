// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory.h"

#include <cstdint>
#include <utility>

#include "base/byte_size.h"
#include "components/performance_manager/public/mojom/web_memory.mojom-blink.h"
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

// performance.memory: the configured machine's heap limit, the page's own sizes
// rounded, whatever precision the caller asked for.
TEST_F(MoltNetMemoryTest, PerformanceMemory) {
  V8TestingScope scope;
  for (const MemoryInfo::Precision precision :
       {MemoryInfo::Precision::kPrecise, MemoryInfo::Precision::kBucketized}) {
    auto* info = MakeGarbageCollected<MemoryInfo>(precision);
    EXPECT_EQ(info->jsHeapSizeLimit(), 2330000000u);
    EXPECT_TRUE(IsBucket(info->usedJSHeapSize()));
    EXPECT_TRUE(IsBucket(info->totalJSHeapSize()));
    EXPECT_LE(info->totalJSHeapSize(), info->jsHeapSizeLimit());
  }
}

TEST_F(MoltNetMemoryTest, Values) {
  EXPECT_EQ(UniformJsHeapSizeLimit(), 2330000000u);
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

// measureUserAgentSpecificMemory(): every context gets a JavaScript and a
// canvas size, measured or not; the measured sizes are kept.
TEST_F(MoltNetMemoryTest, MeasurementEntries) {
  using performance_manager::mojom::blink::WebMemoryBreakdownEntry;
  using performance_manager::mojom::blink::WebMemoryMeasurement;
  auto make = [] {
    auto measurement = WebMemoryMeasurement::New();
    // A measured context with a canvas, a measured one without, one in
    // this process the measurement missed.
    auto measured = WebMemoryBreakdownEntry::New();
    measured->memory = base::ByteSize(1234);
    measured->canvas_memory = base::ByteSize(99);
    measurement->breakdown.push_back(std::move(measured));
    auto no_canvas = WebMemoryBreakdownEntry::New();
    no_canvas->memory = base::ByteSize(5678);
    measurement->breakdown.push_back(std::move(no_canvas));
    measurement->breakdown.push_back(WebMemoryBreakdownEntry::New());
    return measurement;
  };

  auto uniform = make();
  MakeMemoryMeasurementUniform(*uniform);
  ASSERT_EQ(uniform->breakdown.size(), 3u);
  EXPECT_EQ(uniform->breakdown[0]->memory, base::ByteSize(1234));
  EXPECT_EQ(uniform->breakdown[0]->canvas_memory, base::ByteSize(99));
  EXPECT_EQ(uniform->breakdown[1]->memory, base::ByteSize(5678));
  EXPECT_EQ(uniform->breakdown[1]->canvas_memory, base::ByteSize(0));
  EXPECT_EQ(uniform->breakdown[2]->memory, base::ByteSize(0));
  EXPECT_EQ(uniform->breakdown[2]->canvas_memory, base::ByteSize(0));
  EXPECT_EQ(uniform->shared_memory, base::ByteSize(0));
  EXPECT_EQ(uniform->blink_memory, base::ByteSize(0));

  // Outside MoltNet the measurement is left alone.
  SetMoltNetMemoryForTesting(false);
  auto normal = make();
  MakeMemoryMeasurementUniform(*normal);
  EXPECT_FALSE(normal->breakdown[1]->canvas_memory);
  EXPECT_FALSE(normal->breakdown[2]->memory);
  EXPECT_FALSE(normal->shared_memory);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
