// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// GPU inference on Linux and Windows (llama.cpp's Vulkan backend, GN switch
// molt_llama_vulkan; docs/design/SANDBOXED_INFERENCE.md section 11.5): which
// GPU the inference service uses, and how many of a model's layers go on
// it. Pure functions over what the driver reports. Std only, so they are
// unit-tested standalone.
//
// What a driver reports is trusted here (it is the user's own driver), but
// every computation is overflow-safe and bounded anyway.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GPU_OFFLOAD_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GPU_OFFLOAD_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace molt_ai {

// VkPhysicalDeviceType values.
enum class GpuDeviceType : uint32_t {
  kOther = 0,
  kIntegrated = 1,
  kDiscrete = 2,
  kVirtual = 3,
  kCpu = 4,
};

// The VkDriverId values the policy refuses.
namespace gpu_driver {
// Software rasterizers.
inline constexpr uint32_t kGoogleSwiftShader = 10;
inline constexpr uint32_t kMesaLlvmpipe = 13;
// Vulkan on another graphics API or through a virtual machine.
inline constexpr uint32_t kMoltenVk = 14;
inline constexpr uint32_t kMesaVenus = 22;
inline constexpr uint32_t kMesaDozen = 23;
inline constexpr uint32_t kVulkanScEmulation = 27;
inline constexpr uint32_t kMesaKosmicKrisp = 28;
inline constexpr uint32_t kMesaGfxstream = 29;
}  // namespace gpu_driver

// One GPU as ggml-vulkan sees it (molt_llama::GpuDevice without the handle).
struct GpuDeviceInfo {
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  // VkPhysicalDeviceType.
  uint32_t device_type = 0;
  // VkDriverId; 0 when the driver doesn't report one.
  uint32_t driver_id = 0;
  // The vendor's own encoding.
  uint32_t driver_version = 0;
  // VK_MAKE_API_VERSION.
  uint32_t api_version = 0;
  // Device-local memory (all heaps on an integrated GPU); free is the
  // driver's budget minus its usage where it reports one.
  uint64_t memory_total = 0;
  uint64_t memory_free = 0;
};

enum class GpuDeviceVerdict {
  kUsable,
  // A CPU-type device or a software driver (llvmpipe, SwiftShader), whatever
  // type it reports: slower than the CPU backend.
  kSoftware,
  // Vulkan layered on another API or carried out of a virtual machine
  // (Dozen, Venus, gfxstream, MoltenVK, KosmicKrisp, SC emulation).
  kLayered,
  // A virtual or "other" device: neither a discrete nor an integrated GPU.
  kNotAGpu,
  // Below Vulkan 1.2, ggml-vulkan's minimum.
  kApiTooOld,
  // Less than kMinGpuMemoryBytes of device-local memory.
  kTooLittleMemory,
  // A driver on the blocklist.
  kBlocklisted,
};

// A driver found to misbehave: its vendor (PCI id), driver id and an
// inclusive range of driverVersion values (the vendor's own encoding,
// compared as numbers).
struct GpuDriverBlock {
  uint32_t vendor_id = 0;
  uint32_t driver_id = 0;
  uint32_t min_driver_version = 0;
  uint32_t max_driver_version = 0;
};

// Device-local memory a GPU needs at all: a small model and its context.
inline constexpr uint64_t kMinGpuMemoryBytes = 2ULL << 30;
// Share of the device's free memory the model may use.
inline constexpr uint64_t kGpuUsablePercent = 90;
// Compute buffers (llama.cpp's graph at n_batch) on the device.
inline constexpr uint64_t kGpuComputeReserveBytes = 512ULL << 20;
// The smallest KV cache worth planning for.
inline constexpr uint64_t kGpuMinKvReserveBytes = 256ULL << 20;
// Fewer offloaded layers than 1/kGpuMinLayerFraction of the model isn't
// worth the transfers between the GPU and the CPU layers.
inline constexpr uint32_t kGpuMinLayerFraction = 4;
// Vulkan 1.2 (VK_MAKE_API_VERSION(0, 1, 2, 0)), ggml-vulkan's minimum.
inline constexpr uint32_t kGpuMinApiVersion = (1u << 22) | (2u << 12);

// The built-in blocklist. Empty until testing on real hardware finds a driver
// that ggml-vulkan's own workarounds don't cover.
std::vector<GpuDriverBlock> DefaultGpuDriverBlocklist();

GpuDeviceVerdict CheckGpuDevice(const GpuDeviceInfo& device,
                                const std::vector<GpuDriverBlock>& blocklist);

// The device to use: the usable one, discrete before integrated, then with
// the most free memory, then the first listed. nullopt if none is usable.
// One device only: a model is never split across GPUs.
std::optional<size_t> ChooseGpuDevice(
    const std::vector<GpuDeviceInfo>& devices,
    const std::vector<GpuDriverBlock>& blocklist);

struct GpuOffloadInput {
  uint64_t model_file_bytes = 0;
  // The model's repeating layers ("<arch>.block_count"); 0 = unknown.
  uint32_t layer_count = 0;
  uint64_t memory_total = 0;
  uint64_t memory_free = 0;
};

enum class GpuOffloadReason {
  // The whole model fits.
  kAllLayers,
  // Part of it fits; the rest stays on the CPU.
  kSomeLayers,
  // Not even the reserve for compute buffers and a small context fits.
  kTooLittleMemory,
  // Fewer than 1/kGpuMinLayerFraction of the layers would fit.
  kTooFewLayers,
  // The layer count is unknown and the whole model doesn't fit.
  kUnknownLayers,
};

struct GpuOffloadPlan {
  // llama_model_params::n_gpu_layers: 0 = the CPU only, -1 = every layer
  // (the output layer included).
  int32_t gpu_layers = 0;
  GpuOffloadReason reason = GpuOffloadReason::kTooLittleMemory;
  // Estimated bytes of weights on the device.
  uint64_t weight_bytes = 0;
};

// How many layers to put on the device: as many as fit in kGpuUsablePercent
// of its free memory after kGpuComputeReserveBytes + kGpuMinKvReserveBytes,
// counting each layer as an equal share of the file (the output layer as one
// more). All of them when the whole file fits.
GpuOffloadPlan PlanGpuOffload(const GpuOffloadInput& input);

// After the load: the most context tokens whose KV cache fits on the device
// next to the planned weights and the compute buffers. |kv_bytes_per_token|
// is the whole model's; the offloaded layers' share of it lives on the
// device. nullopt when nothing is on the device or the size is unknown.
std::optional<uint32_t> GpuContextLimit(const GpuOffloadInput& input,
                                        const GpuOffloadPlan& plan,
                                        uint64_t kv_bytes_per_token);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GPU_OFFLOAD_H_
