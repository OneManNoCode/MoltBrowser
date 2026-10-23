// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/gpu_offload.h"

#include <algorithm>
#include <limits>

namespace molt_ai {

namespace {

// The API version without its variant bits (VK_API_VERSION_VARIANT).
uint32_t ApiWithoutVariant(uint32_t api_version) {
  return api_version & 0x1FFFFFFFu;
}

bool IsSoftwareDriver(uint32_t driver_id) {
  return driver_id == gpu_driver::kGoogleSwiftShader ||
         driver_id == gpu_driver::kMesaLlvmpipe;
}

bool IsLayeredDriver(uint32_t driver_id) {
  return driver_id == gpu_driver::kMoltenVk ||
         driver_id == gpu_driver::kMesaVenus ||
         driver_id == gpu_driver::kMesaDozen ||
         driver_id == gpu_driver::kVulkanScEmulation ||
         driver_id == gpu_driver::kMesaKosmicKrisp ||
         driver_id == gpu_driver::kMesaGfxstream;
}

// Discrete GPUs first.
int TypeRank(uint32_t device_type) {
  return device_type == static_cast<uint32_t>(GpuDeviceType::kDiscrete) ? 0
                                                                          : 1;
}

// min(free, total) when the total is known: some drivers report a budget
// above the heap size.
uint64_t AvailableMemory(uint64_t total, uint64_t free) {
  return total > 0 ? std::min(total, free) : free;
}

uint64_t UsableMemory(const GpuOffloadInput& input) {
  return AvailableMemory(input.memory_total, input.memory_free) / 100 *
         kGpuUsablePercent;
}

uint64_t SaturatingMul(uint64_t a, uint64_t b) {
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
    return std::numeric_limits<uint64_t>::max();
  }
  return a * b;
}

// One layer's share of the file; the output layer counts as one more.
uint64_t BytesPerLayer(const GpuOffloadInput& input) {
  const uint64_t shares = static_cast<uint64_t>(input.layer_count) + 1;
  return std::max<uint64_t>(
      1, input.model_file_bytes / shares +
             (input.model_file_bytes % shares != 0 ? 1 : 0));
}

}  // namespace

std::vector<GpuDriverBlock> DefaultGpuDriverBlocklist() {
  return {};
}

GpuDeviceVerdict CheckGpuDevice(const GpuDeviceInfo& device,
                                const std::vector<GpuDriverBlock>& blocklist) {
  if (device.device_type == static_cast<uint32_t>(GpuDeviceType::kCpu) ||
      IsSoftwareDriver(device.driver_id)) {
    return GpuDeviceVerdict::kSoftware;
  }
  if (IsLayeredDriver(device.driver_id)) {
    return GpuDeviceVerdict::kLayered;
  }
  if (device.device_type !=
          static_cast<uint32_t>(GpuDeviceType::kDiscrete) &&
      device.device_type !=
          static_cast<uint32_t>(GpuDeviceType::kIntegrated)) {
    return GpuDeviceVerdict::kNotAGpu;
  }
  if (ApiWithoutVariant(device.api_version) < kGpuMinApiVersion) {
    return GpuDeviceVerdict::kApiTooOld;
  }
  for (const GpuDriverBlock& block : blocklist) {
    if (block.vendor_id == device.vendor_id &&
        block.driver_id == device.driver_id &&
        device.driver_version >= block.min_driver_version &&
        device.driver_version <= block.max_driver_version) {
      return GpuDeviceVerdict::kBlocklisted;
    }
  }
  if (device.memory_total < kMinGpuMemoryBytes) {
    return GpuDeviceVerdict::kTooLittleMemory;
  }
  // Last, so only a device that would otherwise be used reports it.
  if (device.turned_off) {
    return GpuDeviceVerdict::kTurnedOff;
  }
  return GpuDeviceVerdict::kUsable;
}

std::optional<size_t> ChooseGpuDevice(
    const std::vector<GpuDeviceInfo>& devices,
    const std::vector<GpuDriverBlock>& blocklist) {
  std::optional<size_t> best;
  for (size_t i = 0; i < devices.size(); ++i) {
    const GpuDeviceInfo& d = devices[i];
    if (CheckGpuDevice(d, blocklist) != GpuDeviceVerdict::kUsable) continue;
    if (!best) {
      best = i;
      continue;
    }
    const GpuDeviceInfo& b = devices[*best];
    const int rank = TypeRank(d.device_type);
    const int best_rank = TypeRank(b.device_type);
    if (rank < best_rank ||
        (rank == best_rank &&
         AvailableMemory(d.memory_total, d.memory_free) >
             AvailableMemory(b.memory_total, b.memory_free))) {
      best = i;
    }
  }
  return best;
}

GpuOffloadPlan PlanGpuOffload(const GpuOffloadInput& input) {
  GpuOffloadPlan plan;
  const uint64_t usable = UsableMemory(input);
  const uint64_t reserve = kGpuComputeReserveBytes + kGpuMinKvReserveBytes;
  if (input.model_file_bytes == 0 || usable <= reserve) {
    plan.reason = GpuOffloadReason::kTooLittleMemory;
    return plan;
  }
  const uint64_t room = usable - reserve;
  if (input.model_file_bytes <= room) {
    plan.gpu_layers = -1;
    plan.reason = GpuOffloadReason::kAllLayers;
    plan.weight_bytes = input.model_file_bytes;
    return plan;
  }
  if (input.layer_count == 0) {
    plan.reason = GpuOffloadReason::kUnknownLayers;
    return plan;
  }
  const uint64_t per_layer = BytesPerLayer(input);
  const uint64_t fit =
      std::min<uint64_t>(room / per_layer, input.layer_count);
  if (fit == 0 || SaturatingMul(fit, kGpuMinLayerFraction) <
                      static_cast<uint64_t>(input.layer_count)) {
    plan.reason = GpuOffloadReason::kTooFewLayers;
    return plan;
  }
  plan.gpu_layers = static_cast<int32_t>(fit);
  plan.reason = GpuOffloadReason::kSomeLayers;
  plan.weight_bytes = SaturatingMul(fit, per_layer);
  return plan;
}

std::optional<uint32_t> GpuContextLimit(const GpuOffloadInput& input,
                                        const GpuOffloadPlan& plan,
                                        uint64_t kv_bytes_per_token) {
  if (plan.gpu_layers == 0 || kv_bytes_per_token == 0) return std::nullopt;
  // The KV cache of the offloaded layers lives on the device.
  uint64_t kv_on_device = kv_bytes_per_token;
  if (plan.gpu_layers > 0 && input.layer_count > 0 &&
      static_cast<uint32_t>(plan.gpu_layers) < input.layer_count) {
    kv_on_device = std::max<uint64_t>(
        1, SaturatingMul(kv_bytes_per_token,
                         static_cast<uint64_t>(plan.gpu_layers)) /
               input.layer_count);
  }
  const uint64_t usable = UsableMemory(input);
  const uint64_t used = plan.weight_bytes + kGpuComputeReserveBytes;
  if (plan.weight_bytes > usable || used > usable) return 0u;
  const uint64_t tokens = (usable - used) / kv_on_device;
  return static_cast<uint32_t>(
      std::min<uint64_t>(tokens, std::numeric_limits<uint32_t>::max()));
}

}  // namespace molt_ai
