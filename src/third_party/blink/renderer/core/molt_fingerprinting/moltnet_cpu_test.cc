// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_cpu.h"

#include <cstdint>
#include <memory>

#include "gin/public/isolate_holder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-statistics.h"

namespace blink::molt_fingerprinting {
namespace {

constexpr uint64_t kGiB = uint64_t{1} << 30;

// The heap limit V8 gives an isolate configured for `physical_memory`.
uint64_t RealHeapSizeLimit(uint64_t physical_memory) {
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.constraints.ConfigureDefaults(physical_memory, 0);
  params.array_buffer_allocator = allocator.get();
  v8::Isolate* isolate = v8::Isolate::New(params);
  v8::HeapStatistics statistics;
  {
    v8::Isolate::Scope scope(isolate);
    isolate->GetHeapStatistics(&statistics);
  }
  isolate->Dispose();
  return statistics.heap_size_limit();
}

class MoltNetCpuTest : public testing::Test {
 protected:
  void TearDown() override {
    gin::IsolateHolder::SetPhysicalMemoryForIsolates(0);
  }

  test::TaskEnvironment task_environment_;
};

// moltnet::V8HeapSizeLimit, which performance.memory reports, is the limit
// V8 really gives the machine MoltNet configures, and others.
TEST_F(MoltNetCpuTest, TheHeapModelIsV8s) {
  for (const uint64_t memory : {moltnet::kV8HeapPhysicalMemory, 1 * kGiB,
                                2 * kGiB, 3 * kGiB, 8 * kGiB, 64 * kGiB}) {
    EXPECT_EQ(RealHeapSizeLimit(memory), moltnet::V8HeapSizeLimit(memory))
        << memory;
  }
  EXPECT_EQ(RealHeapSizeLimit(moltnet::kV8HeapPhysicalMemory),
            moltnet::UniformJsHeapSizeLimit());
}

// gin configures every isolate for the fixed machine once told to, and for
// this machine again once told 0.
TEST_F(MoltNetCpuTest, GinConfiguresTheFixedMachine) {
  const std::unique_ptr<v8::Isolate::CreateParams> machine =
      gin::IsolateHolder::getDefaultIsolateParams();
  gin::IsolateHolder::SetPhysicalMemoryForIsolates(
      moltnet::kV8HeapPhysicalMemory);
  const std::unique_ptr<v8::Isolate::CreateParams> fixed =
      gin::IsolateHolder::getDefaultIsolateParams();
  EXPECT_EQ(fixed->constraints.physical_memory_size_in_bytes(),
            moltnet::kV8HeapPhysicalMemory);
  EXPECT_EQ(fixed->constraints.max_old_generation_size_in_bytes(), 2 * kGiB);
  EXPECT_EQ(fixed->constraints.max_young_generation_size_in_bytes(),
            3 * (uint64_t{32} << 20));
  gin::IsolateHolder::SetPhysicalMemoryForIsolates(0);
  const std::unique_ptr<v8::Isolate::CreateParams> again =
      gin::IsolateHolder::getDefaultIsolateParams();
  EXPECT_EQ(again->constraints.physical_memory_size_in_bytes(),
            machine->constraints.physical_memory_size_in_bytes());
  EXPECT_EQ(again->constraints.max_old_generation_size_in_bytes(),
            machine->constraints.max_old_generation_size_in_bytes());
}

TEST_F(MoltNetCpuTest, Flags) {
  EXPECT_EQ(kMoltNetV8Flags,
            "--no-wasm-relaxed-simd --no-use-std-math-pow "
            "--no-use-std-math-tanh --portable-math");
}

}  // namespace
}  // namespace blink::molt_fingerprinting
