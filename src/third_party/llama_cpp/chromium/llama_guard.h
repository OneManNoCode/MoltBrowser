// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The exception boundary between llama.cpp (built with exceptions) and the
// inference service (built without, like all Chromium code). On the CPU the
// llama.cpp calls the service makes don't throw on the inputs it uses (the
// audit at the top of chrome/services/molt_ai/inference/llama_engine.cc);
// with a GPU backend they can: ggml-vulkan throws vk::DeviceLostError when
// the GPU is reset or its driver faults, and vk::SystemError on other
// Vulkan errors, from inside a decode, a synchronization or a buffer clear
// (docs/design/SANDBOXED_INFERENCE.md section 11.6). An exception reaching
// code built without exceptions terminates the process, so the service
// makes those calls through these wrappers, which are built with exceptions
// and catch everything.
//
// After any wrapper caught an exception, BackendFailed() stays true for the
// life of the process: the context's state is unknown, so it must not be
// used again, and it must not be freed either (freeing on a lost device can
// throw from a destructor, which terminates even with exceptions on). The
// browser ends the process and reloads the model on the CPU.

#ifndef THIRD_PARTY_LLAMA_CPP_CHROMIUM_LLAMA_GUARD_H_
#define THIRD_PARTY_LLAMA_CPP_CHROMIUM_LLAMA_GUARD_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "third_party/llama_cpp/include/llama.h"

namespace molt_llama {

// What Decode() and Process() return when they caught an exception (never a
// llama_decode() result: those are 0, 1, 2, -1, -2 and -3).
inline constexpr int32_t kBackendFailedResult = -1000;

// True once any wrapper below caught an exception (sticky).
bool BackendFailed() noexcept;

// llama_decode().
int32_t Decode(llama_context* ctx, llama_batch batch) noexcept;
// llama_process().
int32_t Process(llama_context* ctx,
                llama_process_type type,
                llama_batch_ext* batch) noexcept;
// llama_get_logits_ith(); nullptr after an exception.
float* GetLogitsIth(llama_context* ctx, int32_t i) noexcept;
// llama_sampler_sample(); LLAMA_TOKEN_NULL after an exception.
llama_token SamplerSample(llama_sampler* sampler,
                          llama_context* ctx,
                          int32_t idx) noexcept;
// llama_memory_clear(); false after an exception.
bool MemoryClear(llama_memory_t mem, bool data) noexcept;

// The layer count of a GGUF model ("<general.architecture>.block_count"),
// read from its header; 0 if it can't be read. Seeks |file| back to its
// start. The service calls it before llama.cpp parses the same header, to
// plan how many layers fit on the GPU.
uint32_t ReadLayerCount(FILE* file) noexcept;

// A GPU that llama.cpp's Vulkan backend can use, with what the device
// policy (chrome/services/molt_ai/inference/engine/gpu_offload.h) needs.
struct GpuDevice {
  GpuDevice();
  ~GpuDevice();
  GpuDevice(const GpuDevice&);
  GpuDevice& operator=(const GpuDevice&);

  ggml_backend_dev_t device = nullptr;
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  // VkPhysicalDeviceType and VkDriverId values (driver_id 0 = unknown).
  uint32_t device_type = 0;
  uint32_t driver_id = 0;
  uint32_t driver_version = 0;
  uint32_t api_version = 0;
  uint64_t memory_total = 0;
  uint64_t memory_free = 0;
  std::string name;
};

// True when this build has llama.cpp's Vulkan backend (GN switch
// molt_llama_vulkan).
bool VulkanBuilt() noexcept;

// Brings up llama.cpp's Vulkan backend (once per process: opens the Vulkan
// loader, creates an instance, enumerates the GPUs ggml would use) and
// lists its devices. The backend is not added to ggml's registry, so
// nothing uses it unless a device is passed explicitly
// (llama_model_params::devices, mtmd_context_params::device). Returns false
// without Vulkan in this build, without a loader, or without a device.
bool ListVulkanDevices(std::vector<GpuDevice>* devices) noexcept;

}  // namespace molt_llama

#endif  // THIRD_PARTY_LLAMA_CPP_CHROMIUM_LLAMA_GUARD_H_
