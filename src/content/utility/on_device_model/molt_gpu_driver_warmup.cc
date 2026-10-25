// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "content/utility/on_device_model/molt_gpu_driver_warmup.h"

#include <algorithm>
#include <unordered_set>

namespace on_device_model {

// glslang 16.6.0 (Chromium 156's), `-V --target-env vulkan1.0` of
//   #version 450
//   layout(local_size_x = 1) in;
//   void main() {}
// SPIRV-Tools' validator accepts it for Vulkan 1.2.
const uint32_t kMoltWarmupComputeShader[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000000a, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000009, 0x0000000b,
    0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017, 0x00000007,
    0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000008, 0x00000001,
    0x0006002c, 0x00000007, 0x00000009, 0x00000008, 0x00000008, 0x00000008,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x000100fd, 0x00010038,
};
const size_t kMoltWarmupComputeShaderWords =
    sizeof(kMoltWarmupComputeShader) / sizeof(kMoltWarmupComputeShader[0]);

bool IsWarmableDeviceType(VkPhysicalDeviceType type, bool include_cpu) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return true;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return include_cpu;
    default:
      return false;
  }
}

std::optional<uint32_t> FirstComputeQueueFamily(
    const std::vector<VkQueueFamilyProperties>& families) {
  for (size_t i = 0; i < families.size(); ++i) {
    if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
        families[i].queueCount > 0) {
      return static_cast<uint32_t>(i);
    }
  }
  return std::nullopt;
}

std::vector<uintptr_t> NewModules(const std::vector<uintptr_t>& before,
                                  const std::vector<uintptr_t>& after) {
  const std::unordered_set<uintptr_t> old(before.begin(), before.end());
  std::vector<uintptr_t> added;
  std::unordered_set<uintptr_t> seen;
  for (uintptr_t m : after) {
    if (m && !old.count(m) && seen.insert(m).second) added.push_back(m);
  }
  return added;
}

namespace {

// The entry points the warm-up uses, from the loader.
struct Api {
  PFN_vkGetInstanceProcAddr get = nullptr;
  PFN_vkCreateInstance create_instance = nullptr;
  PFN_vkDestroyInstance destroy_instance = nullptr;
  PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
  PFN_vkGetPhysicalDeviceProperties get_properties = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_families = nullptr;
  PFN_vkCreateDevice create_device = nullptr;
  PFN_vkDestroyDevice destroy_device = nullptr;
  PFN_vkCreateShaderModule create_shader_module = nullptr;
  PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
  PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
  PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
  PFN_vkCreateComputePipelines create_compute_pipelines = nullptr;
  PFN_vkDestroyPipeline destroy_pipeline = nullptr;

  template <typename T>
  bool Load(VkInstance instance, const char* name, T* out) {
    *out = reinterpret_cast<T>(get(instance, name));
    return *out != nullptr;
  }

  bool LoadInstanceLevel(VkInstance instance) {
    return Load(instance, "vkDestroyInstance", &destroy_instance) &&
           Load(instance, "vkEnumeratePhysicalDevices",
                &enumerate_physical_devices) &&
           Load(instance, "vkGetPhysicalDeviceProperties", &get_properties) &&
           Load(instance, "vkGetPhysicalDeviceQueueFamilyProperties",
                &get_queue_families) &&
           Load(instance, "vkCreateDevice", &create_device) &&
           Load(instance, "vkDestroyDevice", &destroy_device) &&
           Load(instance, "vkCreateShaderModule", &create_shader_module) &&
           Load(instance, "vkDestroyShaderModule", &destroy_shader_module) &&
           Load(instance, "vkCreatePipelineLayout", &create_pipeline_layout) &&
           Load(instance, "vkDestroyPipelineLayout",
                &destroy_pipeline_layout) &&
           Load(instance, "vkCreateComputePipelines",
                &create_compute_pipelines) &&
           Load(instance, "vkDestroyPipeline", &destroy_pipeline);
  }
};

// What one GPU's warm-up created, destroyed in reverse.
struct WarmDevice {
  VkDevice device = VK_NULL_HANDLE;
  VkShaderModule module = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
};

}  // namespace

MoltGpuWarmupResult WarmUpVulkanDrivers(
    PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    void (*while_loaded)(void* context),
    void* context,
    bool include_cpu_devices_for_testing) {
  MoltGpuWarmupResult result;
  Api api;
  api.get = get_instance_proc_addr;
  if (!api.get ||
      !api.Load(VK_NULL_HANDLE, "vkCreateInstance", &api.create_instance)) {
    return result;
  }

  VkApplicationInfo app = {};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "MoltBrowser GPU warm-up";
  // 1.0: every loader and driver accepts it; the devices created here only
  // need to load the driver.
  app.apiVersion = VK_API_VERSION_1_0;
  VkInstanceCreateInfo instance_info = {};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (api.create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS ||
      instance == VK_NULL_HANDLE) {
    return result;
  }
  result.instance = true;
  if (!api.LoadInstanceLevel(instance)) {
    if (api.destroy_instance) api.destroy_instance(instance, nullptr);
    return result;
  }

  std::vector<VkPhysicalDevice> physical;
  uint32_t count = 0;
  if (api.enumerate_physical_devices(instance, &count, nullptr) ==
          VK_SUCCESS &&
      count > 0) {
    // At most 16: a machine has a few GPUs; a driver reporting thousands
    // shouldn't make this allocate or loop for long.
    count = std::min<uint32_t>(count, 16);
    physical.resize(count);
    const VkResult r =
        api.enumerate_physical_devices(instance, &count, physical.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) count = 0;
    physical.resize(std::min<size_t>(count, physical.size()));
  }

  std::vector<WarmDevice> warmed;
  for (VkPhysicalDevice gpu : physical) {
    VkPhysicalDeviceProperties props = {};
    api.get_properties(gpu, &props);
    if (!IsWarmableDeviceType(props.deviceType,
                              include_cpu_devices_for_testing)) {
      continue;
    }
    ++result.gpus;

    uint32_t family_count = 0;
    api.get_queue_families(gpu, &family_count, nullptr);
    family_count = std::min<uint32_t>(family_count, 64);
    std::vector<VkQueueFamilyProperties> families(family_count);
    api.get_queue_families(gpu, &family_count, families.data());
    families.resize(std::min<size_t>(family_count, families.size()));
    const std::optional<uint32_t> family = FirstComputeQueueFamily(families);
    if (!family) continue;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = *family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue;
    WarmDevice w;
    if (api.create_device(gpu, &device_info, nullptr, &w.device) !=
            VK_SUCCESS ||
        w.device == VK_NULL_HANDLE) {
      continue;
    }
    ++result.devices;

    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(kMoltWarmupComputeShader);
    module_info.pCode = kMoltWarmupComputeShader;
    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    // Vulkan leaves a failed call's output handle undefined: keep only
    // what was created.
    VkShaderModule module = VK_NULL_HANDLE;
    if (api.create_shader_module(w.device, &module_info, nullptr, &module) ==
        VK_SUCCESS) {
      w.module = module;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (w.module && api.create_pipeline_layout(w.device, &layout_info,
                                               nullptr, &layout) ==
                        VK_SUCCESS) {
      w.layout = layout;
    }
    if (w.module && w.layout) {
      VkComputePipelineCreateInfo pipeline_info = {};
      pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      pipeline_info.stage.sType =
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      pipeline_info.stage.module = w.module;
      pipeline_info.stage.pName = "main";
      pipeline_info.layout = w.layout;
      VkPipeline pipeline = VK_NULL_HANDLE;
      if (api.create_compute_pipelines(w.device, VK_NULL_HANDLE, 1,
                                       &pipeline_info, nullptr,
                                       &pipeline) == VK_SUCCESS &&
          pipeline != VK_NULL_HANDLE) {
        w.pipeline = pipeline;
        ++result.pipelines;
      }
    }
    warmed.push_back(w);
  }

  // Everything the drivers loaded is still loaded here.
  if (while_loaded) {
    while_loaded(context);
    result.while_loaded_called = true;
  }

  for (auto it = warmed.rbegin(); it != warmed.rend(); ++it) {
    if (it->pipeline) api.destroy_pipeline(it->device, it->pipeline, nullptr);
    if (it->layout) {
      api.destroy_pipeline_layout(it->device, it->layout, nullptr);
    }
    if (it->module) {
      api.destroy_shader_module(it->device, it->module, nullptr);
    }
    api.destroy_device(it->device, nullptr);
  }
  api.destroy_instance(instance, nullptr);
  return result;
}

}  // namespace on_device_model
