// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what the GPU broker of MoltBrowser's inference service may
// open when it runs on the GPU on Linux (docs/design/SANDBOXED_INFERENCE.md
// section 11.4), as a plain list, so molt_ai_unittests can check it.
// molt_gpu_inference_sandbox_linux.cc turns it into broker permissions.
//
// Standard library only.

#ifndef CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_POLICY_LINUX_H_
#define CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_POLICY_LINUX_H_

#include <array>
#include <string>
#include <vector>

namespace on_device_model {

// DRM render nodes are /dev/dri/renderD128 and up, one per GPU; ten is what
// ChromeOS's list covers too.
inline constexpr int kMoltFirstRenderNode = 128;
inline constexpr int kMoltRenderNodes = 10;

// What libdrm reads about the GPU behind a render node, below
// /sys/dev/char/<major>:<minor>/device/ (the GPU's own PCI device):
// drmGetDevices2() and drmGetDevice2(), which Mesa's Vulkan drivers (RADV,
// ANV) use to find the GPU and its PCI ids, and the device's DRM minors,
// which libdrm_amdgpu lists to tell two file descriptors of one GPU apart.
//  - "drm": stat()ed to tell a DRM device from another character device,
//    and listed (only its own entries: card<N>, renderD<N>).
//  - "subsystem": readlink()ed for the bus type (".../bus/pci").
//  - "uevent": PCI_SLOT_NAME, the bus address.
//  - "vendor", "device", "subsystem_vendor", "subsystem_device": the ids.
//  - "revision", and "config" (the first 64 bytes of the PCI configuration
//    space) where a kernel has no "revision" file: the revision ANV asks
//    for.
// libdrm reads them through this path: it can't resolve it to the
// /sys/devices/... path (realpath() needs readlink() of "/sys", which the
// broker refuses) and falls back to it. Nothing else of the device, and
// nothing recursive: the device directory links to the bus, and through it
// to every other device's sysfs files (network addresses, disk and USB
// serial numbers, monitor EDIDs).
inline constexpr std::array<const char*, 9> kMoltDrmSysfsDeviceEntries = {
    "drm",
    "subsystem",
    "uevent",
    "vendor",
    "device",
    "subsystem_vendor",
    "subsystem_device",
    "revision",
    "config",
};

struct MoltDrmRenderNode {
  std::string path;  // "/dev/dri/renderD128"
  unsigned dev_major = 0;
  unsigned dev_minor = 0;
};

struct MoltGpuBrokerGrant {
  enum class Access { kReadOnly, kReadWrite };
  std::string path;
  Access access = Access::kReadOnly;

  friend bool operator==(const MoltGpuBrokerGrant&,
                         const MoltGpuBrokerGrant&) = default;
};

// The broker grants for the GPU, given the render nodes present (character
// devices, as stat() reports them): read-only /dev/dri (libdrm lists it);
// for each node, read-write access to the node (the unprivileged DRM
// interface: no mode setting) and read-only access to exactly
// kMoltDrmSysfsDeviceEntries below /sys/dev/char/<major>:<minor>/device/;
// and the two unsuffixed Mesa ICD manifests (intel_icd.json,
// radeon_icd.json) in both ICD directories. Every path is absolute and
// exact (none recursive).
std::vector<MoltGpuBrokerGrant> MoltGpuInferenceBrokerGrants(
    const std::vector<MoltDrmRenderNode>& render_nodes);

}  // namespace on_device_model

#endif  // CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_POLICY_LINUX_H_
