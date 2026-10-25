// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: brings each GPU's Vulkan driver up once in MoltBrowser's
// inference service before its sandbox locks down, so the driver's modules
// are already loaded when they no longer could be. Used by the Windows
// pre-lockdown step (molt_gpu_inference_sandbox_win.h,
// docs/design/SANDBOXED_INFERENCE.md section 11.15).
//
// Standard library and the Vulkan headers only (built with
// VK_NO_PROTOTYPES): the loader's vkGetInstanceProcAddr is passed in, so
// this runs against any Vulkan driver and is tested on Linux.

#ifndef CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_DRIVER_WARMUP_H_
#define CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_DRIVER_WARMUP_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace on_device_model {

// What WarmUpVulkanDrivers() got through. Each later step needs the one
// before; a GPU counts once even if several of its steps ran.
struct MoltGpuWarmupResult {
  bool instance = false;      // vkCreateInstance succeeded
  uint32_t gpus = 0;          // integrated or discrete GPUs listed
  uint32_t devices = 0;       // VkDevices created on them
  uint32_t pipelines = 0;     // compute pipelines compiled on those
  bool while_loaded_called = false;
};

// A minimal compute shader (`layout(local_size_x = 1) in; void main() {}`,
// SPIR-V 1.0, validated for Vulkan 1.2): compiling it makes the driver load
// its shader compiler, which most drivers load only at the first pipeline.
extern const uint32_t kMoltWarmupComputeShader[];
extern const size_t kMoltWarmupComputeShaderWords;

// Integrated and discrete GPUs are warmed up: the device types ggml-vulkan
// uses and MoltBrowser's device policy accepts (engine/gpu_offload.h).
// CPU-type devices (software rasterizers) only for tests.
bool IsWarmableDeviceType(VkPhysicalDeviceType type, bool include_cpu);

// The first queue family with compute, or nullopt.
std::optional<uint32_t> FirstComputeQueueFamily(
    const std::vector<VkQueueFamilyProperties>& families);

// The handles in |after| that aren't in |before|, in |after|'s order.
std::vector<uintptr_t> NewModules(const std::vector<uintptr_t>& before,
                                  const std::vector<uintptr_t>& after);

// Creates a Vulkan instance through |get_instance_proc_addr|, and on each
// warmable GPU a device with one compute queue and a compute pipeline of
// kMoltWarmupComputeShader. Then calls |while_loaded| (with |context|) once
// everything is still alive, so the caller can pin the modules the driver
// loaded, and destroys everything. Never throws (C API only); any failing
// step just ends that GPU's warm-up. Tests may include CPU-type devices.
MoltGpuWarmupResult WarmUpVulkanDrivers(
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    void (*while_loaded)(void* context),
    void* context,
    bool include_cpu_devices_for_testing = false);

}  // namespace on_device_model

#endif  // CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_DRIVER_WARMUP_H_
