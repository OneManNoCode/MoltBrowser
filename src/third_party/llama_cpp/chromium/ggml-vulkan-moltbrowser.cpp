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

#include <cstdio>
#include <vector>

#include "third_party/llama_cpp/chromium/ggml-vulkan-moltbrowser.h"
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
    HMODULE module = ::LoadLibraryExW(L"vulkan-1.dll", nullptr, 0);
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
    // Stays 0 (unknown) if the driver doesn't fill it in.
    driver.driverID = static_cast<vk::DriverId>(0);
    props.pNext = &driver;
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
    return true;
  } catch (...) {
    return false;
  }
}
