// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: GPU inference in the on-device model sandbox on Linux
// (llama.cpp's Vulkan backend; docs/design/SANDBOXED_INFERENCE.md section
// 11.4). Upstream's broker list for this sandbox type on desktop Linux lets
// in NVIDIA's driver, whose device nodes it already allows, but not Mesa's
// (every Intel and AMD GPU on a stock distro): libdrm lists /dev/dri, reads
// the device's sysfs entries (with readlink) and opens a DRM render node, and
// current Mesa names its ICD manifests without the architecture suffix the
// list expects. ChromeOS's GPU sandbox grants exactly these for the same
// drivers.
//
// Only for a process the browser launched for the GPU: MoltBrowser's
// inference service with kMoltInferenceGpuSwitch on its command line, which
// the browser adds only in builds with the Vulkan backend
// (molt_llama_vulkan) when it asks for the GPU. Every other process of this
// sandbox type, including the CPU path, gets exactly upstream's sandbox.

#ifndef CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_LINUX_H_
#define CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_LINUX_H_

#include <vector>

#include "sandbox/linux/syscall_broker/broker_command.h"
#include "sandbox/linux/syscall_broker/broker_file_permission.h"

namespace on_device_model {

// The same string as molt_ai::kMoltInferenceGpuSwitch
// (chrome/browser/molt_ai/runtime/inference_process_host.h), which content
// can't include.
inline constexpr char kMoltInferenceGpuSwitch[] = "molt-inference-gpu";

// With kMoltInferenceGpuSwitch on this process's command line, adds to the
// GPU broker: read-only /dev/dri; read-write /dev/dri/renderD128 to 137 and
// read-only /sys/dev/char/<major>:<minor>/ for each one present; the two
// unsuffixed Mesa ICD manifests (intel_icd.json, radeon_icd.json) in both
// ICD directories; and brokered readlink (still only for allowed paths). No
// library is added to the pre-sandbox preload list, which already has
// RADV's and ANV's. Without the switch, does nothing.
void MaybeAddMoltGpuInferencePermissions(
    std::vector<sandbox::syscall_broker::BrokerFilePermission>* permissions,
    sandbox::syscall_broker::BrokerCommandSet* command_set);

}  // namespace on_device_model

#endif  // CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_LINUX_H_
