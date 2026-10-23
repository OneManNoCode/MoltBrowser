// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "content/utility/on_device_model/molt_gpu_inference_sandbox_linux.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <string>

#include "base/command_line.h"
#include "base/strings/stringprintf.h"

namespace on_device_model {

namespace {

using sandbox::syscall_broker::BrokerFilePermission;

// DRM render nodes are /dev/dri/renderD128 and up, one per GPU; ten is what
// ChromeOS's list covers too.
constexpr int kFirstRenderNode = 128;
constexpr int kRenderNodes = 10;

}  // namespace

void MaybeAddMoltGpuInferencePermissions(
    std::vector<BrokerFilePermission>* permissions,
    sandbox::syscall_broker::BrokerCommandSet* command_set) {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          kMoltInferenceGpuSwitch)) {
    return;
  }
  // libdrm's drmGetDevices2() lists the directory, then stats each node.
  permissions->push_back(BrokerFilePermission::ReadOnly("/dev/dri"));
  for (int i = 0; i < kRenderNodes; ++i) {
    const std::string node =
        base::StringPrintf("/dev/dri/renderD%d", kFirstRenderNode + i);
    struct stat st;
    if (stat(node.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) continue;
    // The unprivileged DRM interface: no mode setting. This sandbox already
    // allows the primary nodes /dev/dri/card0-9 read-write.
    permissions->push_back(BrokerFilePermission::ReadWrite(node));
    // The device's PCI ids and bus info (and the subsystem link libdrm
    // reads), as ChromeOS's AddDrmGpuDevPermissions grants them.
    permissions->push_back(BrokerFilePermission::ReadOnlyRecursive(
        base::StringPrintf("/sys/dev/char/%u:%u/", major(st.st_rdev),
                           minor(st.st_rdev))));
  }
  // Upstream lists only the arch-suffixed Mesa names (intel_icd.x86_64.json,
  // radeon_icd.x86_64.json); Mesa now ships these. Their libraries are the
  // ones the pre-sandbox hook loads already (libvulkan_intel.so,
  // libvulkan_radeon.so).
  for (const char* dir : {"/usr/share/vulkan/icd.d/", "/etc/vulkan/icd.d/"}) {
    for (const char* manifest : {"intel_icd.json", "radeon_icd.json"}) {
      permissions->push_back(
          BrokerFilePermission::ReadOnly(std::string(dir) + manifest));
    }
  }
  command_set->set(sandbox::syscall_broker::COMMAND_READLINK);
}

}  // namespace on_device_model
