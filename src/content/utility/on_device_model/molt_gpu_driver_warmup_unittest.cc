// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "content/utility/on_device_model/molt_gpu_driver_warmup.h"

#include <cstring>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace on_device_model {
namespace {

TEST(MoltGpuDriverWarmupTest, WarmsGpusOnly) {
  EXPECT_TRUE(IsWarmableDeviceType(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                                   false));
  EXPECT_TRUE(IsWarmableDeviceType(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                                   false));
  EXPECT_FALSE(IsWarmableDeviceType(VK_PHYSICAL_DEVICE_TYPE_CPU, false));
  EXPECT_TRUE(IsWarmableDeviceType(VK_PHYSICAL_DEVICE_TYPE_CPU, true));
  EXPECT_FALSE(IsWarmableDeviceType(VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                                    true));
  EXPECT_FALSE(IsWarmableDeviceType(VK_PHYSICAL_DEVICE_TYPE_OTHER, true));
  EXPECT_FALSE(
      IsWarmableDeviceType(static_cast<VkPhysicalDeviceType>(77), true));
}

TEST(MoltGpuDriverWarmupTest, FirstComputeQueueFamily) {
  std::vector<VkQueueFamilyProperties> families(3);
  families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
  families[0].queueCount = 1;
  families[1].queueFlags = VK_QUEUE_COMPUTE_BIT;
  families[1].queueCount = 0;  // no queues: unusable
  families[2].queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
  families[2].queueCount = 2;
  EXPECT_EQ(2u, FirstComputeQueueFamily(families));
  families[1].queueCount = 1;
  EXPECT_EQ(1u, FirstComputeQueueFamily(families));
  EXPECT_FALSE(FirstComputeQueueFamily({}));
  families.resize(1);
  EXPECT_FALSE(FirstComputeQueueFamily(families));
}

TEST(MoltGpuDriverWarmupTest, NewModules) {
  EXPECT_EQ((std::vector<uintptr_t>{0x30, 0x50}),
            NewModules({0x10, 0x20}, {0x10, 0x30, 0x20, 0x50, 0x30}));
  EXPECT_TRUE(NewModules({0x10}, {0x10}).empty());
  EXPECT_TRUE(NewModules({}, {0}).empty());  // never a null handle
  EXPECT_EQ((std::vector<uintptr_t>{0x10}), NewModules({}, {0x10}));
}

TEST(MoltGpuDriverWarmupTest, ShaderIsAComputeShader) {
  ASSERT_GE(kMoltWarmupComputeShaderWords, 5u);
  EXPECT_EQ(0x07230203u, kMoltWarmupComputeShader[0]);  // SPIR-V magic
  EXPECT_EQ(0x00010000u, kMoltWarmupComputeShader[1]);  // version 1.0
  // Every instruction's word count adds up to the module's length.
  size_t i = 5;
  bool entry_point_compute = false;
  while (i < kMoltWarmupComputeShaderWords) {
    const uint32_t words = kMoltWarmupComputeShader[i] >> 16;
    const uint32_t opcode = kMoltWarmupComputeShader[i] & 0xffff;
    ASSERT_GT(words, 0u);
    if (opcode == 15) {  // OpEntryPoint: execution model GLCompute (5)
      entry_point_compute = kMoltWarmupComputeShader[i + 1] == 5;
    }
    i += words;
  }
  EXPECT_EQ(kMoltWarmupComputeShaderWords, i);
  EXPECT_TRUE(entry_point_compute);
  // The last instruction is OpFunctionEnd.
  EXPECT_EQ(0x00010038u,
            kMoltWarmupComputeShader[kMoltWarmupComputeShaderWords - 1]);
}

// A fake Vulkan loader and driver: what it reports and what it was asked.
struct Fake {
  bool instance_ok = true;
  std::vector<VkPhysicalDeviceType> types;
  std::vector<std::vector<VkQueueFamilyProperties>> families;
  std::vector<bool> device_ok;
  bool pipeline_ok = true;
  // Calls.
  int instances = 0, instances_destroyed = 0;
  int devices = 0, devices_destroyed = 0;
  int modules = 0, modules_destroyed = 0;
  int layouts = 0, layouts_destroyed = 0;
  int pipelines = 0, pipelines_destroyed = 0;
  int alive_at_callback = 0;
  size_t shader_bytes = 0;
};
Fake* g_fake = nullptr;

// Fake handles: small integers, never dereferenced.
template <typename T>
T Handle(uintptr_t v) {
  return reinterpret_cast<T>(v);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo*,
                                              const VkAllocationCallbacks*,
                                              VkInstance* out) {
  if (!g_fake->instance_ok) return VK_ERROR_INCOMPATIBLE_DRIVER;
  ++g_fake->instances;
  *out = Handle<VkInstance>(0x1000);
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance,
                                           const VkAllocationCallbacks*) {
  ++g_fake->instances_destroyed;
}
VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance,
                                                        uint32_t* count,
                                                        VkPhysicalDevice* out) {
  if (!out) {
    *count = static_cast<uint32_t>(g_fake->types.size());
    return VK_SUCCESS;
  }
  for (uint32_t i = 0; i < *count && i < g_fake->types.size(); ++i) {
    out[i] = Handle<VkPhysicalDevice>(0x2000 + i);
  }
  return VK_SUCCESS;
}
size_t Index(VkPhysicalDevice d) {
  return reinterpret_cast<uintptr_t>(d) - 0x2000;
}
VKAPI_ATTR void VKAPI_CALL GetProperties(VkPhysicalDevice d,
                                         VkPhysicalDeviceProperties* p) {
  std::memset(p, 0, sizeof(*p));
  p->deviceType = g_fake->types[Index(d)];
}
VKAPI_ATTR void VKAPI_CALL
GetQueueFamilies(VkPhysicalDevice d,
                 uint32_t* count,
                 VkQueueFamilyProperties* out) {
  const auto& f = g_fake->families[Index(d)];
  if (!out) {
    *count = static_cast<uint32_t>(f.size());
    return;
  }
  for (uint32_t i = 0; i < *count && i < f.size(); ++i) out[i] = f[i];
}
VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice d,
                                            const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks*,
                                            VkDevice* out) {
  if (!g_fake->device_ok[Index(d)]) return VK_ERROR_INITIALIZATION_FAILED;
  EXPECT_EQ(1u, info->queueCreateInfoCount);
  ++g_fake->devices;
  *out = Handle<VkDevice>(0x3000 + Index(d));
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice,
                                         const VkAllocationCallbacks*) {
  ++g_fake->devices_destroyed;
}
VKAPI_ATTR VkResult VKAPI_CALL
CreateShaderModule(VkDevice,
                   const VkShaderModuleCreateInfo* info,
                   const VkAllocationCallbacks*,
                   VkShaderModule* out) {
  ++g_fake->modules;
  g_fake->shader_bytes = info->codeSize;
  *out = Handle<VkShaderModule>(0x4000);
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice,
                                               VkShaderModule,
                                               const VkAllocationCallbacks*) {
  ++g_fake->modules_destroyed;
}
VKAPI_ATTR VkResult VKAPI_CALL
CreatePipelineLayout(VkDevice,
                     const VkPipelineLayoutCreateInfo*,
                     const VkAllocationCallbacks*,
                     VkPipelineLayout* out) {
  ++g_fake->layouts;
  *out = Handle<VkPipelineLayout>(0x5000);
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL
DestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) {
  ++g_fake->layouts_destroyed;
}
VKAPI_ATTR VkResult VKAPI_CALL
CreateComputePipelines(VkDevice,
                       VkPipelineCache,
                       uint32_t count,
                       const VkComputePipelineCreateInfo* info,
                       const VkAllocationCallbacks*,
                       VkPipeline* out) {
  EXPECT_EQ(1u, count);
  EXPECT_STREQ("main", info->stage.pName);
  if (!g_fake->pipeline_ok) {
    *out = Handle<VkPipeline>(0xbad);  // undefined on failure: never used
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  ++g_fake->pipelines;
  *out = Handle<VkPipeline>(0x6000);
  return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice,
                                           VkPipeline p,
                                           const VkAllocationCallbacks*) {
  EXPECT_EQ(Handle<VkPipeline>(0x6000), p);
  ++g_fake->pipelines_destroyed;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetProc(VkInstance,
                                                 const char* name) {
  const std::string n(name);
#define FAKE(fn, impl) \
  if (n == fn) return reinterpret_cast<PFN_vkVoidFunction>(&impl);
  FAKE("vkCreateInstance", CreateInstance)
  FAKE("vkDestroyInstance", DestroyInstance)
  FAKE("vkEnumeratePhysicalDevices", EnumeratePhysicalDevices)
  FAKE("vkGetPhysicalDeviceProperties", GetProperties)
  FAKE("vkGetPhysicalDeviceQueueFamilyProperties", GetQueueFamilies)
  FAKE("vkCreateDevice", CreateDevice)
  FAKE("vkDestroyDevice", DestroyDevice)
  FAKE("vkCreateShaderModule", CreateShaderModule)
  FAKE("vkDestroyShaderModule", DestroyShaderModule)
  FAKE("vkCreatePipelineLayout", CreatePipelineLayout)
  FAKE("vkDestroyPipelineLayout", DestroyPipelineLayout)
  FAKE("vkCreateComputePipelines", CreateComputePipelines)
  FAKE("vkDestroyPipeline", DestroyPipeline)
#undef FAKE
  return nullptr;
}

void CountAlive(void* context) {
  Fake* f = static_cast<Fake*>(context);
  f->alive_at_callback = f->devices - f->devices_destroyed;
}

class MoltGpuDriverWarmupFakeTest : public testing::Test {
 protected:
  void SetUp() override { g_fake = &fake_; }
  void TearDown() override { g_fake = nullptr; }

  void AddDevice(VkPhysicalDeviceType type, bool compute, bool ok = true) {
    fake_.types.push_back(type);
    VkQueueFamilyProperties graphics = {};
    graphics.queueFlags = VK_QUEUE_GRAPHICS_BIT;
    graphics.queueCount = 1;
    VkQueueFamilyProperties comp = {};
    comp.queueFlags = VK_QUEUE_COMPUTE_BIT;
    comp.queueCount = 1;
    fake_.families.push_back(compute ? std::vector<VkQueueFamilyProperties>{
                                           graphics, comp}
                                     : std::vector<VkQueueFamilyProperties>{
                                           graphics});
    fake_.device_ok.push_back(ok);
  }

  void ExpectBalanced() {
    EXPECT_EQ(fake_.instances, fake_.instances_destroyed);
    EXPECT_EQ(fake_.devices, fake_.devices_destroyed);
    EXPECT_EQ(fake_.modules, fake_.modules_destroyed);
    EXPECT_EQ(fake_.layouts, fake_.layouts_destroyed);
    EXPECT_EQ(fake_.pipelines, fake_.pipelines_destroyed);
  }

  Fake fake_;
};

TEST_F(MoltGpuDriverWarmupFakeTest, EachGpuGetsADeviceAndAPipeline) {
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, true);
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_CPU, true);  // skipped
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, true);
  const MoltGpuWarmupResult r =
      WarmUpVulkanDrivers(&GetProc, &CountAlive, &fake_);
  EXPECT_TRUE(r.instance);
  EXPECT_EQ(2u, r.gpus);
  EXPECT_EQ(2u, r.devices);
  EXPECT_EQ(2u, r.pipelines);
  EXPECT_TRUE(r.while_loaded_called);
  // Both devices still existed when the caller pinned the modules.
  EXPECT_EQ(2, fake_.alive_at_callback);
  EXPECT_EQ(sizeof(uint32_t) * kMoltWarmupComputeShaderWords,
            fake_.shader_bytes);
  ExpectBalanced();
}

TEST_F(MoltGpuDriverWarmupFakeTest, SoftwareDevicesOnlyForTests) {
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_CPU, true);
  MoltGpuWarmupResult r = WarmUpVulkanDrivers(&GetProc, nullptr, nullptr);
  EXPECT_EQ(0u, r.gpus);
  EXPECT_EQ(0, fake_.devices);
  r = WarmUpVulkanDrivers(&GetProc, nullptr, nullptr,
                          /*include_cpu_devices_for_testing=*/true);
  EXPECT_EQ(1u, r.devices);
  EXPECT_FALSE(r.while_loaded_called);  // no callback given
  ExpectBalanced();
}

TEST_F(MoltGpuDriverWarmupFakeTest, FailuresEndOnlyThatGpu) {
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, false);       // no compute
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, true, false);  // no device
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, true);
  fake_.pipeline_ok = false;
  const MoltGpuWarmupResult r =
      WarmUpVulkanDrivers(&GetProc, &CountAlive, &fake_);
  EXPECT_EQ(3u, r.gpus);
  EXPECT_EQ(1u, r.devices);
  EXPECT_EQ(0u, r.pipelines);
  EXPECT_TRUE(r.while_loaded_called);
  EXPECT_EQ(0, fake_.pipelines_destroyed);  // the failed one never destroyed
  ExpectBalanced();
}

TEST_F(MoltGpuDriverWarmupFakeTest, NoInstanceNoLoader) {
  fake_.instance_ok = false;
  AddDevice(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, true);
  MoltGpuWarmupResult r = WarmUpVulkanDrivers(&GetProc, &CountAlive, &fake_);
  EXPECT_FALSE(r.instance);
  EXPECT_FALSE(r.while_loaded_called);
  r = WarmUpVulkanDrivers(nullptr, &CountAlive, &fake_);
  EXPECT_FALSE(r.instance);
  ExpectBalanced();
}

}  // namespace
}  // namespace on_device_model
