// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser additions to ggml-vulkan (GN switch molt_llama_vulkan; design:
// docs/design/SANDBOXED_INFERENCE.md section 11). Built with ggml-vulkan's
// own flags and exceptions; the functions declared in
// ggml-vulkan-moltbrowser.h never let one escape.

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "third_party/brotli/include/brotli/decode.h"
#include "third_party/llama_cpp/chromium/ggml-vulkan-moltbrowser.h"
#include "third_party/llama_cpp/chromium/ggml-vulkan-shader-pack.h"
#include "third_party/llama_cpp/ggml/src/ggml-vulkan/ggml-vulkan-common.h"

#if !defined(VK_NO_PROTOTYPES)
#error "ggml-vulkan must not link against the Vulkan loader (VK_NO_PROTOTYPES)"
#endif

// Declared by the MoltBrowser block in ggml-vulkan.cpp. ggml-vulkan is built
// with VK_NO_PROTOTYPES, so the browser binary has no link-time dependency on
// libvulkan.so.1 / vulkan-1.dll: the loader is opened here, only when the
// inference service registers the backend (that is, when the browser asked
// for the GPU). On Linux the sandbox's pre-sandbox hook has already loaded
// libvulkan.so.1 (RTLD_NODELETE), so dlopen() finds it by name without
// opening a file. The loader is never unloaded.
PFN_vkGetInstanceProcAddr ggml_vk_moltbrowser_get_instance_proc_addr() {
  static const PFN_vkGetInstanceProcAddr proc =
      []() -> PFN_vkGetInstanceProcAddr {
#if defined(_WIN32)
    // The loader the pre-lockdown step loaded from System32 and pinned
    // (content/utility/on_device_model/molt_gpu_inference_sandbox_win.h):
    // once the sandbox has locked down, only a module already mapped can be
    // used. Otherwise System32's, never one from another directory.
    HMODULE module = ::GetModuleHandleW(L"vulkan-1.dll");
    if (!module) {
      module = ::LoadLibraryExW(L"vulkan-1.dll", nullptr,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!module) return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        ::GetProcAddress(module, "vkGetInstanceProcAddr"));
#else
    void* module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!module) return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(module, "vkGetInstanceProcAddr"));
#endif
  }();
  if (!proc) {
    throw vk::SystemError(
        vk::make_error_code(vk::Result::eErrorInitializationFailed),
        "ggml_vulkan: no Vulkan loader");
  }
  return proc;
}

extern "C" bool ggml_vk_moltbrowser_get_device_info(
    int device,
    ggml_vk_moltbrowser_device_info* info) {
  if (!info || device < 0 ||
      static_cast<size_t>(device) >= vk_instance.device_indices.size() ||
      vk_instance.device_supports_membudget.size() !=
          vk_instance.device_indices.size()) {
    return false;
  }
  try {
    const std::vector<vk::PhysicalDevice> devices =
        vk_instance.instance.enumeratePhysicalDevices();
    const size_t index = vk_instance.device_indices[device];
    if (index >= devices.size()) return false;

    vk::PhysicalDeviceProperties2 props;
    vk::PhysicalDeviceDriverProperties driver;
    vk::PhysicalDeviceIDProperties ids;
    // Stays 0 (unknown) if the driver doesn't fill it in.
    driver.driverID = static_cast<vk::DriverId>(0);
    props.pNext = &driver;
    driver.pNext = &ids;
    devices[index].getProperties2(&props);

    size_t free_bytes = 0;
    size_t total_bytes = 0;
    ggml_backend_vk_get_device_memory(device, &free_bytes, &total_bytes);

    *info = {};
    info->vendor_id = props.properties.vendorID;
    info->device_id = props.properties.deviceID;
    info->device_type = static_cast<uint32_t>(props.properties.deviceType);
    info->driver_id = static_cast<uint32_t>(driver.driverID);
    info->driver_version = props.properties.driverVersion;
    info->api_version = props.properties.apiVersion;
    info->memory_total = total_bytes;
    info->memory_free = free_bytes;
    std::snprintf(info->name, sizeof(info->name), "%s",
                  props.properties.deviceName.data());
    static_assert(sizeof(info->driver_uuid) == VK_UUID_SIZE);
    static_assert(sizeof(info->pipeline_cache_uuid) == VK_UUID_SIZE);
    std::memcpy(info->driver_uuid, ids.driverUUID.data(), VK_UUID_SIZE);
    std::memcpy(info->pipeline_cache_uuid,
                props.properties.pipelineCacheUUID.data(), VK_UUID_SIZE);
    return true;
  } catch (...) {
    return false;
  }
}

namespace {

// What ggml_vk_moltbrowser_set_pipeline_cache_data() set, per ggml device,
// until the device is created.
std::mutex g_pipeline_cache_mutex;
std::vector<uint8_t> g_pipeline_cache_data[GGML_VK_MAX_DEVICES];

}  // namespace

// Declared in the MoltBrowser block of ggml-vulkan.cpp; called by
// ggml_vk_get_device() right after it created the device. Uses vulkan.hpp's
// result-returning overloads only (they don't throw).
vk::PipelineCache ggml_vk_moltbrowser_create_pipeline_cache(vk::Device device,
                                                            size_t idx) {
  std::vector<uint8_t> data;
  {
    std::lock_guard<std::mutex> lock(g_pipeline_cache_mutex);
    if (idx < GGML_VK_MAX_DEVICES) {
      data.swap(g_pipeline_cache_data[idx]);
    }
  }
  vk::PipelineCacheCreateInfo info;
  info.initialDataSize = data.size();
  info.pInitialData = data.empty() ? nullptr : data.data();
  vk::PipelineCache cache;
  if (device.createPipelineCache(&info, nullptr, &cache) ==
      vk::Result::eSuccess) {
    return cache;
  }
  // Drivers create an empty cache from data they can't use; one that fails
  // instead gets no data.
  info.initialDataSize = 0;
  info.pInitialData = nullptr;
  if (!data.empty() && device.createPipelineCache(&info, nullptr, &cache) ==
                           vk::Result::eSuccess) {
    return cache;
  }
  return VK_NULL_HANDLE;
}

extern "C" bool ggml_vk_moltbrowser_set_pipeline_cache_data(int device,
                                                            const void* data,
                                                            size_t size) {
  if (device < 0 || device >= GGML_VK_MAX_DEVICES || (size && !data)) {
    return false;
  }
  try {
    if (vk_instance.devices[device]) {
      return false;  // created already: its cache is fixed
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> copy(bytes, bytes + size);
    std::lock_guard<std::mutex> lock(g_pipeline_cache_mutex);
    g_pipeline_cache_data[device].swap(copy);
    return true;
  } catch (...) {
    return false;
  }
}

extern "C" bool ggml_vk_moltbrowser_get_pipeline_cache_data(int device,
                                                            void* data,
                                                            size_t* size) {
  if (!size || device < 0 || device >= GGML_VK_MAX_DEVICES) {
    return false;
  }
  try {
    const vk_device& dev = vk_instance.devices[device];
    if (!dev || !dev->pipeline_cache) {
      return false;
    }
    // vkGetPipelineCacheData: VK_INCOMPLETE when |data| is too small.
    return dev->device.getPipelineCacheData(dev->pipeline_cache, size, data) ==
           vk::Result::eSuccess;
  } catch (...) {
    return false;
  }
}

extern "C" bool ggml_vk_moltbrowser_device_lost(int device) {
  if (device < 0 || device >= GGML_VK_MAX_DEVICES) {
    return false;
  }
  try {
    const vk_device& dev = vk_instance.devices[device];
    if (!dev || !dev->fence) {
      return false;
    }
    // The C entry point: vulkan.hpp's getFenceStatus() throws on a lost
    // device.
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetFenceStatus(
               static_cast<VkDevice>(dev->device),
               static_cast<VkFence>(dev->fence)) == VK_ERROR_DEVICE_LOST;
  } catch (...) {
    return false;
  }
}

// Declared in the MoltBrowser block of ggml-vulkan.cpp, which calls it from
// ggml_vk_create_pipeline_func() for the one variant it compiles. The data is
// part of this binary (ggml_vk_shader_pack wrote it at build time), so a
// failure here is a build mistake or a variant the build left out; it
// throws like a Vulkan error would, which the inference service's guard
// turns into a GPU failure (the model then runs on the CPU).
std::vector<uint32_t> ggml_vk_moltbrowser_unpack_shader(const void* data,
                                                        size_t size) {
  using ggml_vk_moltbrowser::kMaxBlockRawSize;
  if (size == 0) {
    throw std::runtime_error(
        "ggml_vulkan: a shader variant this build left out was requested");
  }
  ggml_vk_moltbrowser::ShaderDescriptor d;
  if (!ggml_vk_moltbrowser::DecodeDescriptor(data, size, &d) ||
      d.family >= ggml_vk_moltbrowser_shader_family_count) {
    throw std::runtime_error("ggml_vulkan: not a packed shader");
  }
  const ggml_vk_moltbrowser::PackedShaderFamily* family =
      ggml_vk_moltbrowser_shader_families[d.family];
  if (!family || !family->blocks || d.block >= family->block_count) {
    throw std::runtime_error("ggml_vulkan: bad packed shader");
  }
  const ggml_vk_moltbrowser::PackedShaderBlock& block =
      family->blocks[d.block];
  if (block.raw_size > kMaxBlockRawSize || d.size > block.raw_size ||
      d.offset > block.raw_size - d.size) {
    throw std::runtime_error(std::string("ggml_vulkan: bad packed shader in ") +
                             family->name);
  }
  std::vector<uint8_t> raw(block.raw_size);
  size_t raw_size = raw.size();
  if (BrotliDecoderDecompress(block.size, block.data, &raw_size, raw.data()) !=
          BROTLI_DECODER_RESULT_SUCCESS ||
      raw_size != block.raw_size ||
      ggml_vk_moltbrowser::ShaderChecksum(raw.data() + d.offset, d.size) !=
          d.checksum) {
    throw std::runtime_error(
        std::string("ggml_vulkan: packed shader doesn't inflate in ") +
        family->name);
  }
  std::vector<uint32_t> spirv(d.size / sizeof(uint32_t));
  std::memcpy(spirv.data(), raw.data() + d.offset, d.size);
  return spirv;
}

namespace {

// Whether this build has the shaders of each ggml type: false for the
// weight types BUILD.gn left out (ggml_vk_moltbrowser_dropped_types, the
// names ggml_type_name() gives, in lower case).
const std::array<bool, GGML_TYPE_COUNT>& BuiltTypes() {
  static const std::array<bool, GGML_TYPE_COUNT> built = [] {
    std::array<bool, GGML_TYPE_COUNT> table;
    table.fill(true);
    for (int t = 0; t < GGML_TYPE_COUNT; ++t) {
      const char* name = ggml_type_name(static_cast<ggml_type>(t));
      if (!name) continue;
      std::string lower(name);
      for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      for (const char* const* d = ggml_vk_moltbrowser_dropped_types; *d;
           ++d) {
        if (lower == *d) table[t] = false;
      }
    }
    return table;
  }();
  return built;
}

bool TypeBuilt(ggml_type type) {
  return type < 0 || type >= GGML_TYPE_COUNT || BuiltTypes()[type];
}

}  // namespace

// Declared in the MoltBrowser block of ggml-vulkan.cpp, which asks it first
// in ggml_backend_vk_device_supports_op(): an op on a left-out weight type,
// as its result or any of its sources, stays on the CPU (llama.cpp then
// keeps that weight in CPU memory too), so its shaders are never needed.
bool ggml_vk_moltbrowser_op_types_built(const ggml_tensor* op) {
  if (!TypeBuilt(op->type)) return false;
  for (int i = 0; i < GGML_MAX_SRC; ++i) {
    if (op->src[i] && !TypeBuilt(op->src[i]->type)) return false;
  }
  return true;
}
