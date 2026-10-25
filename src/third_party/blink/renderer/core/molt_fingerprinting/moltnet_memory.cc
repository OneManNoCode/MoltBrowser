// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory.h"

#include <atomic>

#include "base/byte_size.h"
#include "components/performance_manager/public/mojom/web_memory.mojom-blink.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.h"

namespace blink::molt_fingerprinting {

namespace {

std::atomic<bool> g_memory_for_testing{false};

}  // namespace

bool UsesUniformMemoryInfo() {
  return IsUniform() || g_memory_for_testing.load(std::memory_order_relaxed);
}

size_t UniformJsHeapSizeLimit() {
  return static_cast<size_t>(
      moltnet::QuantizedMemorySize(moltnet::UniformJsHeapSizeLimit()));
}

size_t UniformHeapSize(size_t bytes) {
  return static_cast<size_t>(
      moltnet::QuantizedMemorySize(static_cast<uint64_t>(bytes)));
}

uint64_t UniformMemoryMeasurementBytes(uint64_t bytes) {
  if (!UsesUniformMemoryInfo() || bytes == 0) {
    return bytes;
  }
  return moltnet::QuantizedMemorySize(bytes);
}

void MakeMemoryMeasurementUniform(
    performance_manager::mojom::blink::WebMemoryMeasurement& measurement) {
  if (!UsesUniformMemoryInfo()) {
    return;
  }
  for (auto& entry : measurement.breakdown) {
    if (!entry) {
      continue;
    }
    if (!entry->memory) {
      entry->memory = base::ByteSize(0);
    }
    if (!entry->canvas_memory) {
      entry->canvas_memory = base::ByteSize(0);
    }
  }
  if (!measurement.shared_memory) {
    measurement.shared_memory = base::ByteSize(0);
  }
  if (!measurement.blink_memory) {
    measurement.blink_memory = base::ByteSize(0);
  }
}

void SetMoltNetMemoryForTesting(bool uniform) {
  g_memory_for_testing.store(uniform, std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
