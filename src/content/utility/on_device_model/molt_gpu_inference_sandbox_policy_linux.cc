// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "content/utility/on_device_model/molt_gpu_inference_sandbox_policy_linux.h"

#include <string>
#include <vector>

namespace on_device_model {

std::vector<MoltGpuBrokerGrant> MoltGpuInferenceBrokerGrants(
    const std::vector<MoltDrmRenderNode>& render_nodes) {
  using Access = MoltGpuBrokerGrant::Access;
  std::vector<MoltGpuBrokerGrant> grants;
  // libdrm's drmGetDevices2() lists the directory, then stats each node.
  grants.push_back({"/dev/dri", Access::kReadOnly});
  for (const MoltDrmRenderNode& node : render_nodes) {
    // The unprivileged DRM interface: no mode setting. This sandbox already
    // allows the primary nodes /dev/dri/card0-9 read-write.
    grants.push_back({node.path, Access::kReadWrite});
    const std::string device = "/sys/dev/char/" +
                               std::to_string(node.dev_major) + ":" +
                               std::to_string(node.dev_minor) + "/device/";
    for (const char* entry : kMoltDrmSysfsDeviceEntries) {
      grants.push_back({device + entry, Access::kReadOnly});
    }
  }
  // Upstream lists only the arch-suffixed Mesa names (intel_icd.x86_64.json,
  // radeon_icd.x86_64.json); Mesa now ships these. Their libraries are the
  // ones the pre-sandbox hook loads already (libvulkan_intel.so,
  // libvulkan_radeon.so).
  for (const char* dir : {"/usr/share/vulkan/icd.d/", "/etc/vulkan/icd.d/"}) {
    for (const char* manifest : {"intel_icd.json", "radeon_icd.json"}) {
      grants.push_back({std::string(dir) + manifest, Access::kReadOnly});
    }
  }
  return grants;
}

}  // namespace on_device_model
