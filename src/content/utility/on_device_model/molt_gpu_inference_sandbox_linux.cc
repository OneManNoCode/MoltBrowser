// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "content/utility/on_device_model/molt_gpu_inference_sandbox_linux.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <string>

#include "base/command_line.h"
#include "base/strings/stringprintf.h"
#include "content/utility/on_device_model/molt_gpu_inference_sandbox_policy_linux.h"

namespace on_device_model {

using sandbox::syscall_broker::BrokerFilePermission;

void MaybeAddMoltGpuInferencePermissions(
    std::vector<BrokerFilePermission>* permissions,
    sandbox::syscall_broker::BrokerCommandSet* command_set) {
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          kMoltInferenceGpuSwitch)) {
    return;
  }
  std::vector<MoltDrmRenderNode> render_nodes;
  for (int i = 0; i < kMoltRenderNodes; ++i) {
    std::string node =
        base::StringPrintf("/dev/dri/renderD%d", kMoltFirstRenderNode + i);
    struct stat st;
    if (stat(node.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) {
      continue;
    }
    render_nodes.push_back({.path = std::move(node),
                            .dev_major = major(st.st_rdev),
                            .dev_minor = minor(st.st_rdev)});
  }
  for (const MoltGpuBrokerGrant& grant :
       MoltGpuInferenceBrokerGrants(render_nodes)) {
    permissions->push_back(
        grant.access == MoltGpuBrokerGrant::Access::kReadWrite
            ? BrokerFilePermission::ReadWrite(grant.path)
            : BrokerFilePermission::ReadOnly(grant.path));
  }
  // libdrm reads the device's "subsystem" link for its bus type.
  command_set->set(sandbox::syscall_broker::COMMAND_READLINK);
}

}  // namespace on_device_model
