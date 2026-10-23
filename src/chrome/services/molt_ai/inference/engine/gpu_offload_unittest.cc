// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/gpu_offload.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

constexpr uint64_t kGiB = 1ULL << 30;
constexpr uint64_t kMiB = 1ULL << 20;
constexpr uint32_t kVendorAmd = 0x1002;
constexpr uint32_t kVendorIntel = 0x8086;
constexpr uint32_t kVendorNvidia = 0x10de;
constexpr uint32_t kVendorMesaSoftware = 0x10005;
constexpr uint32_t kDriverRadv = 3;
constexpr uint32_t kDriverNvidia = 4;
constexpr uint32_t kDriverAnv = 6;

constexpr uint32_t Api(uint32_t major, uint32_t minor) {
  return (major << 22) | (minor << 12);
}

GpuDeviceInfo Discrete(uint64_t total = 8 * kGiB, uint64_t free = 7 * kGiB) {
  GpuDeviceInfo d;
  d.vendor_id = kVendorNvidia;
  d.device_type = static_cast<uint32_t>(GpuDeviceType::kDiscrete);
  d.driver_id = kDriverNvidia;
  d.driver_version = 0x8e8a0000;
  d.api_version = Api(1, 4);
  d.memory_total = total;
  d.memory_free = free;
  return d;
}

GpuDeviceInfo Integrated(uint64_t total = 16 * kGiB,
                         uint64_t free = 12 * kGiB) {
  GpuDeviceInfo d;
  d.vendor_id = kVendorIntel;
  d.device_type = static_cast<uint32_t>(GpuDeviceType::kIntegrated);
  d.driver_id = kDriverAnv;
  d.api_version = Api(1, 4);
  d.memory_total = total;
  d.memory_free = free;
  return d;
}

// What this machine's lavapipe reports (vulkaninfo): llvmpipe, type CPU.
GpuDeviceInfo Llvmpipe() {
  GpuDeviceInfo d;
  d.vendor_id = kVendorMesaSoftware;
  d.device_type = static_cast<uint32_t>(GpuDeviceType::kCpu);
  d.driver_id = gpu_driver::kMesaLlvmpipe;
  d.api_version = Api(1, 4);
  d.memory_total = 15 * kGiB;
  d.memory_free = 15 * kGiB;
  return d;
}

const std::vector<GpuDriverBlock> kNoBlocks;

TEST(GpuOffloadTest, UsableGpus) {
  EXPECT_EQ(GpuDeviceVerdict::kUsable, CheckGpuDevice(Discrete(), kNoBlocks));
  EXPECT_EQ(GpuDeviceVerdict::kUsable,
            CheckGpuDevice(Integrated(), kNoBlocks));
  // Exactly the minimum memory and API version.
  GpuDeviceInfo d = Discrete(kMinGpuMemoryBytes, kMinGpuMemoryBytes);
  d.api_version = Api(1, 2);
  EXPECT_EQ(GpuDeviceVerdict::kUsable, CheckGpuDevice(d, kNoBlocks));
  // A driver that doesn't report its id is judged by type.
  d.driver_id = 0;
  EXPECT_EQ(GpuDeviceVerdict::kUsable, CheckGpuDevice(d, kNoBlocks));
}

TEST(GpuOffloadTest, SoftwareRasterizersRefused) {
  EXPECT_EQ(GpuDeviceVerdict::kSoftware, CheckGpuDevice(Llvmpipe(), kNoBlocks));
  // Whatever type a software driver claims.
  GpuDeviceInfo d = Llvmpipe();
  d.device_type = static_cast<uint32_t>(GpuDeviceType::kIntegrated);
  EXPECT_EQ(GpuDeviceVerdict::kSoftware, CheckGpuDevice(d, kNoBlocks));
  d.driver_id = gpu_driver::kGoogleSwiftShader;
  EXPECT_EQ(GpuDeviceVerdict::kSoftware, CheckGpuDevice(d, kNoBlocks));
  // A CPU-type device from an unknown driver.
  d = Discrete();
  d.device_type = static_cast<uint32_t>(GpuDeviceType::kCpu);
  d.driver_id = 0;
  EXPECT_EQ(GpuDeviceVerdict::kSoftware, CheckGpuDevice(d, kNoBlocks));
}

TEST(GpuOffloadTest, LayeredAndVirtualRefused) {
  for (uint32_t id :
       {gpu_driver::kMesaDozen, gpu_driver::kMesaVenus,
        gpu_driver::kMesaGfxstream, gpu_driver::kMoltenVk,
        gpu_driver::kMesaKosmicKrisp, gpu_driver::kVulkanScEmulation}) {
    GpuDeviceInfo d = Discrete();
    d.driver_id = id;
    EXPECT_EQ(GpuDeviceVerdict::kLayered, CheckGpuDevice(d, kNoBlocks)) << id;
  }
  GpuDeviceInfo d = Discrete();
  d.device_type = static_cast<uint32_t>(GpuDeviceType::kVirtual);
  EXPECT_EQ(GpuDeviceVerdict::kNotAGpu, CheckGpuDevice(d, kNoBlocks));
  d.device_type = static_cast<uint32_t>(GpuDeviceType::kOther);
  EXPECT_EQ(GpuDeviceVerdict::kNotAGpu, CheckGpuDevice(d, kNoBlocks));
  d.device_type = 77;  // not a VkPhysicalDeviceType
  EXPECT_EQ(GpuDeviceVerdict::kNotAGpu, CheckGpuDevice(d, kNoBlocks));
}

TEST(GpuOffloadTest, OldApiAndSmallMemoryRefused) {
  GpuDeviceInfo d = Discrete();
  d.api_version = Api(1, 1) | 0xfff;  // 1.1 with the highest patch
  EXPECT_EQ(GpuDeviceVerdict::kApiTooOld, CheckGpuDevice(d, kNoBlocks));
  // The variant bits don't count as a version.
  d.api_version = Api(1, 1) | (7u << 29);
  EXPECT_EQ(GpuDeviceVerdict::kApiTooOld, CheckGpuDevice(d, kNoBlocks));
  d = Discrete(kMinGpuMemoryBytes - 1, kMinGpuMemoryBytes - 1);
  EXPECT_EQ(GpuDeviceVerdict::kTooLittleMemory, CheckGpuDevice(d, kNoBlocks));
}

TEST(GpuOffloadTest, Blocklist) {
  const std::vector<GpuDriverBlock> blocks = {
      {kVendorAmd, kDriverRadv, 100, 200}};
  GpuDeviceInfo d = Discrete();
  d.vendor_id = kVendorAmd;
  d.driver_id = kDriverRadv;
  for (uint32_t v : {100u, 150u, 200u}) {
    d.driver_version = v;
    EXPECT_EQ(GpuDeviceVerdict::kBlocklisted, CheckGpuDevice(d, blocks)) << v;
  }
  for (uint32_t v : {99u, 201u}) {
    d.driver_version = v;
    EXPECT_EQ(GpuDeviceVerdict::kUsable, CheckGpuDevice(d, blocks)) << v;
  }
  // Same version, another vendor or driver.
  d.driver_version = 150;
  d.driver_id = kDriverNvidia;
  EXPECT_EQ(GpuDeviceVerdict::kUsable, CheckGpuDevice(d, blocks));
  d.driver_id = kDriverRadv;
  d.vendor_id = kVendorIntel;
  EXPECT_EQ(GpuDeviceVerdict::kUsable, CheckGpuDevice(d, blocks));
  EXPECT_TRUE(DefaultGpuDriverBlocklist().empty());
}

TEST(GpuOffloadTest, ChooseDevice) {
  EXPECT_EQ(std::nullopt, ChooseGpuDevice({}, kNoBlocks));
  EXPECT_EQ(std::nullopt, ChooseGpuDevice({Llvmpipe()}, kNoBlocks));
  // This machine: only lavapipe, never chosen.
  EXPECT_EQ(std::nullopt, ChooseGpuDevice({Llvmpipe(), Llvmpipe()}, kNoBlocks));
  // Discrete before integrated, even with less free memory.
  EXPECT_EQ(std::optional<size_t>(1),
            ChooseGpuDevice({Integrated(32 * kGiB, 30 * kGiB),
                             Discrete(4 * kGiB, 3 * kGiB)},
                            kNoBlocks));
  // Between two discrete GPUs, the most free memory; ties keep the first.
  EXPECT_EQ(std::optional<size_t>(2),
            ChooseGpuDevice({Discrete(8 * kGiB, 2 * kGiB), Llvmpipe(),
                             Discrete(8 * kGiB, 6 * kGiB)},
                            kNoBlocks));
  EXPECT_EQ(std::optional<size_t>(0),
            ChooseGpuDevice({Discrete(), Discrete()}, kNoBlocks));
  // A budget above the heap counts as the heap.
  EXPECT_EQ(std::optional<size_t>(1),
            ChooseGpuDevice({Discrete(4 * kGiB, 100 * kGiB),
                             Discrete(8 * kGiB, 5 * kGiB)},
                            kNoBlocks));
  // A refused device is skipped wherever it is.
  GpuDeviceInfo dozen = Discrete(24 * kGiB, 24 * kGiB);
  dozen.driver_id = gpu_driver::kMesaDozen;
  EXPECT_EQ(std::optional<size_t>(1),
            ChooseGpuDevice({dozen, Integrated()}, kNoBlocks));
}

// A driver the browser's GPU record turned off (section 11.10): the next
// usable GPU is chosen instead, or none.
TEST(GpuOffloadTest, TurnedOffDriverSkipped) {
  GpuDeviceInfo off = Discrete();
  off.turned_off = true;
  EXPECT_EQ(GpuDeviceVerdict::kTurnedOff, CheckGpuDevice(off, kNoBlocks));
  EXPECT_EQ(std::optional<size_t>(1),
            ChooseGpuDevice({off, Integrated()}, kNoBlocks));
  EXPECT_EQ(std::nullopt, ChooseGpuDevice({off}, kNoBlocks));
  // Refused for another reason first: that reason is reported.
  GpuDeviceInfo software = Llvmpipe();
  software.turned_off = true;
  EXPECT_EQ(GpuDeviceVerdict::kSoftware, CheckGpuDevice(software, kNoBlocks));
}

GpuOffloadInput Input(uint64_t file, uint32_t layers, uint64_t total,
                      uint64_t free) {
  GpuOffloadInput in;
  in.model_file_bytes = file;
  in.layer_count = layers;
  in.memory_total = total;
  in.memory_free = free;
  return in;
}

TEST(GpuOffloadTest, WholeModelFits) {
  // An 8B Q4_K_M (4.7 GiB) on an 8 GiB card with 7 GiB free.
  const GpuOffloadPlan plan =
      PlanGpuOffload(Input(4700 * kMiB, 32, 8 * kGiB, 7 * kGiB));
  EXPECT_EQ(-1, plan.gpu_layers);
  EXPECT_EQ(GpuOffloadReason::kAllLayers, plan.reason);
  EXPECT_EQ(4700 * kMiB, plan.weight_bytes);
  // The layer count doesn't matter then.
  EXPECT_EQ(-1, PlanGpuOffload(Input(1 * kGiB, 0, 8 * kGiB, 7 * kGiB))
                    .gpu_layers);
}

TEST(GpuOffloadTest, PartialOffload) {
  // 8 GiB model, 33 shares of ~248 MiB; 6 GiB free -> 5.4 GiB usable,
  // 4.65 GiB after the reserve -> 19 layers.
  const GpuOffloadPlan plan =
      PlanGpuOffload(Input(8 * kGiB, 32, 8 * kGiB, 6 * kGiB));
  EXPECT_EQ(GpuOffloadReason::kSomeLayers, plan.reason);
  EXPECT_EQ(19, plan.gpu_layers);
  EXPECT_LE(plan.weight_bytes + kGpuComputeReserveBytes + kGpuMinKvReserveBytes,
            6 * kGiB / 100 * kGpuUsablePercent);
  // Never more than the repeating layers when the output layer doesn't fit.
  const GpuOffloadPlan almost =
      PlanGpuOffload(Input(8 * kGiB, 32, 12 * kGiB, 9 * kGiB));
  EXPECT_LE(almost.gpu_layers, 32);
  EXPECT_GT(almost.gpu_layers, 0);
}

TEST(GpuOffloadTest, NotWorthIt) {
  // A 20 GiB model on a 4 GiB card: under a quarter of the layers.
  const GpuOffloadPlan plan =
      PlanGpuOffload(Input(20 * kGiB, 48, 4 * kGiB, 4 * kGiB));
  EXPECT_EQ(0, plan.gpu_layers);
  EXPECT_EQ(GpuOffloadReason::kTooFewLayers, plan.reason);
  // An 11 GiB card: 90% is 9.9 GiB, 9.15 GiB after the reserve, so 9 layers
  // of 1 GiB (file / (layers + 1)) fit.
  EXPECT_EQ(9, PlanGpuOffload(Input(33 * kGiB, 32, 11 * kGiB, 11 * kGiB))
                   .gpu_layers);
  // Exactly a quarter of the layers is enough (9 of 36)...
  EXPECT_EQ(9, PlanGpuOffload(Input(37 * kGiB, 36, 11 * kGiB, 11 * kGiB))
                   .gpu_layers);
  // ...one layer short of it isn't (9 of 37).
  const GpuOffloadPlan short_plan =
      PlanGpuOffload(Input(38 * kGiB, 37, 11 * kGiB, 11 * kGiB));
  EXPECT_EQ(0, short_plan.gpu_layers);
  EXPECT_EQ(GpuOffloadReason::kTooFewLayers, short_plan.reason);
  // Unknown layer count and the model doesn't fit: CPU.
  const GpuOffloadPlan unknown =
      PlanGpuOffload(Input(20 * kGiB, 0, 8 * kGiB, 8 * kGiB));
  EXPECT_EQ(0, unknown.gpu_layers);
  EXPECT_EQ(GpuOffloadReason::kUnknownLayers, unknown.reason);
}

TEST(GpuOffloadTest, TooLittleMemory) {
  for (uint64_t free : {uint64_t{0}, 100 * kMiB, 800 * kMiB}) {
    const GpuOffloadPlan plan =
        PlanGpuOffload(Input(1 * kGiB, 16, 8 * kGiB, free));
    EXPECT_EQ(0, plan.gpu_layers) << free;
    EXPECT_EQ(GpuOffloadReason::kTooLittleMemory, plan.reason) << free;
  }
  // An empty file is never planned.
  EXPECT_EQ(0, PlanGpuOffload(Input(0, 16, 8 * kGiB, 8 * kGiB)).gpu_layers);
  // A budget above the heap is capped at the heap.
  EXPECT_EQ(GpuOffloadReason::kTooLittleMemory,
            PlanGpuOffload(Input(1 * kGiB, 16, 512 * kMiB, 64 * kGiB)).reason);
}

TEST(GpuOffloadTest, HostileNumbers) {
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  // Huge memory and file sizes don't overflow.
  GpuOffloadPlan plan = PlanGpuOffload(Input(max, 4096, max, max));
  EXPECT_GE(plan.gpu_layers, 0);
  plan = PlanGpuOffload(Input(max - 1, 1, max, max));
  EXPECT_GE(plan.gpu_layers, 0);
  plan = PlanGpuOffload(Input(1, std::numeric_limits<uint32_t>::max(), max,
                              max));
  EXPECT_EQ(-1, plan.gpu_layers);
  // More layers than bytes: each layer counts as at least one byte.
  plan = PlanGpuOffload(
      Input(100 * kGiB, std::numeric_limits<uint32_t>::max(), 2 * kGiB,
            2 * kGiB));
  EXPECT_EQ(0, plan.gpu_layers);
  // The context limit saturates instead of wrapping.
  GpuOffloadPlan all;
  all.gpu_layers = -1;
  all.weight_bytes = 1;
  EXPECT_EQ(std::optional<uint32_t>(std::numeric_limits<uint32_t>::max()),
            GpuContextLimit(Input(1, 0, max, max), all, 1));
  GpuOffloadPlan some;
  some.gpu_layers = 1;
  some.weight_bytes = 1;
  EXPECT_TRUE(GpuContextLimit(Input(1, 2, max, max), some, max).has_value());
}

TEST(GpuOffloadTest, ContextLimit) {
  const GpuOffloadInput in = Input(4700 * kMiB, 32, 8 * kGiB, 7 * kGiB);
  const GpuOffloadPlan plan = PlanGpuOffload(in);
  // Qwen3-8B: about 147 KB of KV cache per token at f16.
  const std::optional<uint32_t> limit = GpuContextLimit(in, plan, 147456);
  ASSERT_TRUE(limit.has_value());
  const uint64_t usable = 7 * kGiB / 100 * kGpuUsablePercent;
  EXPECT_EQ((usable - 4700 * kMiB - kGpuComputeReserveBytes) / 147456, *limit);
  // Only the offloaded layers' KV cache is on the device.
  GpuOffloadPlan half;
  half.gpu_layers = 16;
  half.weight_bytes = 2 * kGiB;
  const std::optional<uint32_t> half_limit = GpuContextLimit(in, half, 147456);
  ASSERT_TRUE(half_limit.has_value());
  EXPECT_EQ((usable - 2 * kGiB - kGpuComputeReserveBytes) / (147456 / 2),
            *half_limit);
  // Nothing on the device, or an unknown KV size: no limit from the GPU.
  EXPECT_EQ(std::nullopt, GpuContextLimit(in, GpuOffloadPlan(), 147456));
  EXPECT_EQ(std::nullopt, GpuContextLimit(in, plan, 0));
  // Weights that already fill the device leave no room.
  GpuOffloadPlan full;
  full.gpu_layers = -1;
  full.weight_bytes = 7 * kGiB;
  EXPECT_EQ(std::optional<uint32_t>(0u), GpuContextLimit(in, full, 147456));
}

}  // namespace
}  // namespace molt_ai
