// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "content/utility/on_device_model/molt_gpu_inference_sandbox_policy_linux.h"

#include <set>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace on_device_model {
namespace {

using Access = MoltGpuBrokerGrant::Access;

std::vector<MoltGpuBrokerGrant> Below(
    const std::vector<MoltGpuBrokerGrant>& grants,
    const std::string& prefix) {
  std::vector<MoltGpuBrokerGrant> out;
  for (const MoltGpuBrokerGrant& grant : grants) {
    if (grant.path.starts_with(prefix)) {
      out.push_back(grant);
    }
  }
  return out;
}

// What BrokerFilePermission accepts for a non-recursive path: absolute, no
// trailing slash, no "." or ".." components. A trailing slash would make it
// a recursive grant.
bool IsExactBrokerPath(const std::string& path) {
  return path.size() > 1 && path.front() == '/' && path.back() != '/' &&
         path.find("/./") == std::string::npos &&
         path.find("/../") == std::string::npos && !path.ends_with("/.") &&
         !path.ends_with("/..");
}

TEST(MoltGpuInferenceSandboxPolicyTest, NoRenderNode) {
  const std::vector<MoltGpuBrokerGrant> grants =
      MoltGpuInferenceBrokerGrants({});
  EXPECT_EQ(grants, (std::vector<MoltGpuBrokerGrant>{
                        {"/dev/dri", Access::kReadOnly},
                        {"/usr/share/vulkan/icd.d/intel_icd.json",
                         Access::kReadOnly},
                        {"/usr/share/vulkan/icd.d/radeon_icd.json",
                         Access::kReadOnly},
                        {"/etc/vulkan/icd.d/intel_icd.json", Access::kReadOnly},
                        {"/etc/vulkan/icd.d/radeon_icd.json",
                         Access::kReadOnly},
                    }));
  EXPECT_TRUE(Below(grants, "/sys/").empty());
}

TEST(MoltGpuInferenceSandboxPolicyTest, OneRenderNodeGetsExactlyLibdrmsFiles) {
  const std::vector<MoltGpuBrokerGrant> grants =
      MoltGpuInferenceBrokerGrants({{"/dev/dri/renderD128", 226, 128}});
  EXPECT_EQ(Below(grants, "/dev/dri/"),
            (std::vector<MoltGpuBrokerGrant>{
                {"/dev/dri/renderD128", Access::kReadWrite}}));
  const std::string device = "/sys/dev/char/226:128/device/";
  EXPECT_EQ(Below(grants, "/sys/"),
            (std::vector<MoltGpuBrokerGrant>{
                {device + "drm", Access::kReadOnly},
                {device + "subsystem", Access::kReadOnly},
                {device + "uevent", Access::kReadOnly},
                {device + "vendor", Access::kReadOnly},
                {device + "device", Access::kReadOnly},
                {device + "subsystem_vendor", Access::kReadOnly},
                {device + "subsystem_device", Access::kReadOnly},
                {device + "revision", Access::kReadOnly},
                {device + "config", Access::kReadOnly},
            }));
}

// The device's directory links to its bus, and through the bus to every
// other device; and the node's directory to the device. None of it, and
// nothing recursive, may be granted.
TEST(MoltGpuInferenceSandboxPolicyTest, NothingRecursiveOrBeyondTheDevice) {
  const std::vector<MoltGpuBrokerGrant> grants = MoltGpuInferenceBrokerGrants(
      {{"/dev/dri/renderD128", 226, 128}, {"/dev/dri/renderD129", 226, 129}});
  for (const MoltGpuBrokerGrant& grant : grants) {
    SCOPED_TRACE(grant.path);
    EXPECT_TRUE(IsExactBrokerPath(grant.path));
    if (grant.path.starts_with("/sys/")) {
      EXPECT_EQ(grant.access, Access::kReadOnly);
      // /sys/dev/char/<M>:<m>/device/<entry>, one level below the device.
      const std::string rest =
          grant.path.substr(grant.path.find("/device/") + 8);
      EXPECT_EQ(rest.find('/'), std::string::npos);
      EXPECT_NE(rest, "");
    }
  }
  for (const char* forbidden :
       {"/sys/dev/char/226:128", "/sys/dev/char/226:128/device",
        "/sys/dev/char/226:128/uevent", "/sys/dev/char/226:128/device/driver",
        "/sys/dev/char/226:128/device/subsystem/devices",
        "/sys/dev/char/226:128/device/drm/card0", "/sys", "/sys/bus/pci"}) {
    for (const MoltGpuBrokerGrant& grant : grants) {
      EXPECT_NE(grant.path, forbidden);
    }
  }
}

TEST(MoltGpuInferenceSandboxPolicyTest, EachNodeItsOwnFiles) {
  const std::vector<MoltGpuBrokerGrant> grants = MoltGpuInferenceBrokerGrants(
      {{"/dev/dri/renderD128", 226, 128}, {"/dev/dri/renderD129", 226, 129}});
  EXPECT_EQ(Below(grants, "/sys/dev/char/226:128/").size(),
            kMoltDrmSysfsDeviceEntries.size());
  EXPECT_EQ(Below(grants, "/sys/dev/char/226:129/").size(),
            kMoltDrmSysfsDeviceEntries.size());
  EXPECT_EQ(Below(grants, "/sys/").size(),
            2 * kMoltDrmSysfsDeviceEntries.size());
  std::set<std::string> paths;
  for (const MoltGpuBrokerGrant& grant : grants) {
    EXPECT_TRUE(paths.insert(grant.path).second) << grant.path;
  }
  // A node whose device numbers aren't the usual ones (another major).
  const std::vector<MoltGpuBrokerGrant> other =
      MoltGpuInferenceBrokerGrants({{"/dev/dri/renderD130", 511, 2}});
  EXPECT_EQ(Below(other, "/sys/dev/char/511:2/device/").size(),
            kMoltDrmSysfsDeviceEntries.size());
}

}  // namespace
}  // namespace on_device_model
