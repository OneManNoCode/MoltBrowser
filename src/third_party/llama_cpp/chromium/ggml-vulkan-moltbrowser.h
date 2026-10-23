// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser additions to llama.cpp's Vulkan backend (ggml-vulkan), built
// only with the GN switch molt_llama_vulkan. See
// docs/design/SANDBOXED_INFERENCE.md section 11.
//
// Nothing here throws: these are called from code built without exceptions.

#ifndef THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_VULKAN_MOLTBROWSER_H_
#define THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_VULKAN_MOLTBROWSER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the inference service's device policy needs about one ggml-vulkan
// device (the index ggml_backend_vk_reg()'s devices use). Values are
// Vulkan's: device_type is a VkPhysicalDeviceType, driver_id a VkDriverId
// (0 when the driver doesn't report one), versions as VK_MAKE_API_VERSION /
// the vendor's own driverVersion encoding. Memory as
// ggml_backend_vk_get_device_memory() counts it: device-local heaps (every
// heap on an integrated GPU), free = the driver's budget minus its usage
// where VK_EXT_memory_budget exists, else the heap size.
struct ggml_vk_moltbrowser_device_info {
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t device_type;
  uint32_t driver_id;
  uint32_t driver_version;
  uint32_t api_version;
  uint64_t memory_total;
  uint64_t memory_free;
  // NUL-terminated (VkPhysicalDeviceProperties::deviceName).
  char name[256];
  // VkPhysicalDeviceIDProperties::driverUUID (the driver build) and
  // VkPhysicalDeviceProperties::pipelineCacheUUID (what its pipeline caches
  // are valid for).
  uint8_t driver_uuid[16];
  uint8_t pipeline_cache_uuid[16];
};

// Fills |info| for ggml-vulkan device |device|. Needs a successful
// ggml_backend_vk_reg(). Returns false for an index out of range or if a
// Vulkan query failed.
bool ggml_vk_moltbrowser_get_device_info(
    int device,
    struct ggml_vk_moltbrowser_device_info* info);

// Pipeline cache (docs/design/SANDBOXED_INFERENCE.md section 11.12).
// ggml-vulkan creates every pipeline of a device with that device's
// VkPipelineCache, created with the device (its first model buffer or
// context) from the data set here; data the driver finds incompatible gives
// an empty cache. Returns false for an index out of range or once the
// device exists (the data is then not used). |data| is copied; |size| 0
// clears it.
bool ggml_vk_moltbrowser_set_pipeline_cache_data(int device,
                                                 const void* data,
                                                 size_t size);

// vkGetPipelineCacheData for device |device|: with |data| null, *size gets
// the cache's size; else up to *size bytes are written and *size set to
// what was written. Returns false if the device or its cache doesn't
// exist, on a Vulkan error, or if *size was too small (VK_INCOMPLETE).
bool ggml_vk_moltbrowser_get_pipeline_cache_data(int device,
                                                 void* data,
                                                 size_t* size);

// Whether device |device| exists and was lost (vkGetFenceStatus on its
// fence returns VK_ERROR_DEVICE_LOST), e.g. after a load on it failed.
bool ggml_vk_moltbrowser_device_lost(int device);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_VULKAN_MOLTBROWSER_H_
