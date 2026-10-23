// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Vulkan pipeline cache of GPU inference (llama.cpp's Vulkan backend, GN
// switch molt_llama_vulkan; docs/design/SANDBOXED_INFERENCE.md section
// 11.12): the only part of a cache either side reads is its header.
//
// The browser keeps one cache file per GPU, driver and shader set, hands it
// to the inference service read-only at load, and stores what the service
// sends back after the model has run. The service seeds ggml-vulkan's
// VkPipelineCache from it; the GPU driver parses the rest. Both sides check
// the header against the device the service reported, so a file made for
// another device, driver build or format never reaches a driver, and the
// browser bounds the size. Nothing here parses past the 32-byte header.
//
// Std only (linked by the browser's runtime and by the service), so it is
// unit-tested and fuzzed standalone.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GPU_PIPELINE_CACHE_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GPU_PIPELINE_CACHE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace molt_ai {

// VkPipelineCacheHeaderVersionOne: headerSize, headerVersion, vendorID,
// deviceID (four uint32, least significant byte first whatever the host's
// byte order) and pipelineCacheUUID.
inline constexpr size_t kPipelineCacheHeaderBytes = 32;
inline constexpr size_t kPipelineCacheUuidBytes = 16;
// VK_PIPELINE_CACHE_HEADER_VERSION_ONE.
inline constexpr uint32_t kPipelineCacheHeaderVersionOne = 1;

// The largest pipeline cache the browser stores and the service reads or
// sends. A cache for every pipeline one model uses is far smaller (section
// 11.12 has the sizes measured); one this large has been growing across
// models and is started again.
inline constexpr size_t kMaxPipelineCacheBytes = 32u * 1024 * 1024;

// The device a cache must be for, as the service reported it
// (VkPhysicalDeviceProperties).
struct PipelineCacheDevice {
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  std::array<uint8_t, kPipelineCacheUuidBytes> uuid{};
};

enum class PipelineCacheCheck {
  kOk,
  // No bytes at all.
  kEmpty,
  // Shorter than a version-one header.
  kTruncated,
  // Over the size cap.
  kTooLarge,
  // headerSize below 32, or past the end of the data.
  kBadHeaderSize,
  // Not VK_PIPELINE_CACHE_HEADER_VERSION_ONE (e.g. Vulkan SC's).
  kBadHeaderVersion,
  kWrongVendor,
  kWrongDevice,
  // Another driver build or cache format: drivers change the UUID when
  // their cache can't be read by the new build.
  kWrongUuid,
  // The device reported an all-zero pipelineCacheUUID: its driver can't tell
  // its own caches from another build's, so none is kept for it.
  kNoDeviceUuid,
};

const char* PipelineCacheCheckName(PipelineCacheCheck check);

// Whether |data| (the whole cache) is a version-one pipeline cache of at
// most |max_bytes| for |device|. Reads the header only. Never kOk for a
// device without a pipelineCacheUUID.
PipelineCacheCheck CheckPipelineCacheHeader(
    std::string_view data,
    const PipelineCacheDevice& device,
    size_t max_bytes = kMaxPipelineCacheBytes);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GPU_PIPELINE_CACHE_H_
