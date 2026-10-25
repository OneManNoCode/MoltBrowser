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

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
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

// What a projector GGUF (llama.cpp's "mmproj") says about its encoders,
// read from its own header the way llama.cpp's multimodal library reads
// it: clip.has_vision_encoder and clip.has_audio_encoder (booleans), the
// audio projector's type (clip.projector_type, or clip.audio.projector_type
// when that is absent or empty) and clip.audio.projector.stack_factor (an
// unsigned 32-bit value; 0 when absent or of another type). |read| is false
// if the header can't be parsed. Seeks |file| back to its start.
struct ProjectorFacts {
  ProjectorFacts();
  ~ProjectorFacts();
  ProjectorFacts(const ProjectorFacts&);
  ProjectorFacts& operator=(const ProjectorFacts&);

  bool read = false;
  bool has_vision_encoder = false;
  bool has_audio_encoder = false;
  std::string audio_projector_type;
  uint32_t audio_stack_factor = 0;
};
ProjectorFacts ReadProjectorFacts(FILE* file) noexcept;

// A GPU that llama.cpp's Vulkan backend can use, with what the device
// policy (chrome/services/molt_ai/inference/engine/gpu_offload.h) needs.
struct GpuDevice {
  GpuDevice();
  ~GpuDevice();
  GpuDevice(const GpuDevice&);
  GpuDevice& operator=(const GpuDevice&);

  ggml_backend_dev_t device = nullptr;
  // Its index among ggml-vulkan's devices.
  int index = -1;
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
  // VkPhysicalDeviceIDProperties::driverUUID and
  // VkPhysicalDeviceProperties::pipelineCacheUUID.
  std::array<uint8_t, 16> driver_uuid{};
  std::array<uint8_t, 16> pipeline_cache_uuid{};
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

// The Vulkan pipeline cache of a device from ListVulkanDevices
// (GpuDevice::index; docs/design/SANDBOXED_INFERENCE.md section 11.12).
// SetPipelineCacheData seeds it: call before the model is loaded on the
// device (its pipelines are created with that cache, compiled on first use);
// false without Vulkan, for a bad index, or once the device exists.
bool SetPipelineCacheData(int device, const std::vector<uint8_t>& data) noexcept;
// The cache's size (vkGetPipelineCacheData); nullopt without one.
std::optional<size_t> PipelineCacheSize(int device) noexcept;
// Its data, if at most |max_bytes|; false otherwise or on an error.
bool GetPipelineCacheData(int device,
                          size_t max_bytes,
                          std::vector<uint8_t>* data) noexcept;

// Whether the device exists and was lost (a GPU reset, a driver fault):
// tells a load that failed because the GPU went away from one that ran out
// of device memory.
bool DeviceLost(int device) noexcept;

}  // namespace molt_llama

#endif  // THIRD_PARTY_LLAMA_CPP_CHROMIUM_LLAMA_GUARD_H_
