// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_device.h"

#include <atomic>
#include <cstdint>

#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_device_values.h"

namespace blink::molt_fingerprinting {

namespace {

using network::mojom::WebClientHintsType;

// The allow-list names the generated enum's values.
static_assert(static_cast<int32_t>(WebClientHintsType::kUA) ==
              moltnet::kHintUA);
static_assert(static_cast<int32_t>(WebClientHintsType::kUAPlatform) ==
              moltnet::kHintUAPlatform);
static_assert(static_cast<int32_t>(WebClientHintsType::kUAMobile) ==
              moltnet::kHintUAMobile);
static_assert(static_cast<int32_t>(WebClientHintsType::kPrefersColorScheme) ==
              moltnet::kHintPrefersColorScheme);
static_assert(static_cast<int32_t>(WebClientHintsType::kSaveData) ==
              moltnet::kHintSaveData);
static_assert(static_cast<int32_t>(WebClientHintsType::kPrefersReducedMotion) ==
              moltnet::kHintPrefersReducedMotion);
static_assert(
    static_cast<int32_t>(WebClientHintsType::kPrefersReducedTransparency) ==
    moltnet::kHintPrefersReducedTransparency);

std::atomic<bool> g_device_for_testing{false};

bool UniformMode() {
  return IsUniform() || g_device_for_testing.load(std::memory_order_relaxed);
}

}  // namespace

float DeviceMemory(float real) {
  return UniformMode() ? moltnet::kDeviceMemory : real;
}

bool WithholdsClientHint(WebClientHintsType type) {
  return UniformMode() && !moltnet::SendsClientHint(static_cast<int32_t>(type));
}

void UniformUserAgentMetadata(UserAgentMetadata& metadata) {
  if (!UniformMode()) {
    return;
  }
  // What embedder_support::GetUserAgentMetadata(/*only_low_entropy_ch=*/true)
  // leaves at its defaults.
  metadata.brand_full_version_list.clear();
  metadata.full_version.clear();
  metadata.platform_version.clear();
  metadata.architecture.clear();
  metadata.model.clear();
  metadata.bitness.clear();
  metadata.wow64 = false;
  metadata.form_factors.clear();
}

void SetMoltNetDeviceForTesting(bool uniform) {
  g_device_for_testing.store(uniform, std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
