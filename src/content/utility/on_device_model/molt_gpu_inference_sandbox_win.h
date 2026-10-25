// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: GPU inference in the on-device model sandbox on Windows
// (llama.cpp's Vulkan backend; docs/design/SANDBOXED_INFERENCE.md section
// 11.15). This sandbox type gets MITIGATION_FORCE_MS_SIGNED_BINS as a
// delayed mitigation, applied at LowerToken(): from then on the process can
// map only Microsoft-signed images, so neither the Vulkan loader nor a GPU
// vendor's driver could be loaded when the service brings the backend up.
// Upstream's own on-device model service has the same problem with its
// Direct3D drivers and solves it the same way: it brings its GPUs up before
// lockdown (PreSandboxInit), so the drivers are mapped while they still can
// be, and a later LoadLibrary of an already-mapped module maps nothing new.
//
// Only for a process the browser launched for the GPU (kMoltInferenceGpuSwitch
// on its command line, which the browser adds only in builds with
// molt_llama_vulkan when it asks for the GPU). Every other process of this
// sandbox type, including the CPU path, starts exactly as upstream's.

#ifndef CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_WIN_H_
#define CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_WIN_H_

namespace base {
class CommandLine;
}

namespace on_device_model {

// Called by content/utility/utility_main.cc (MoltBrowser hook) for the
// on-device model sandbox type, before LowerToken() and before any service
// starts. With kMoltInferenceGpuSwitch on |command_line| (and the GN switch
// molt_llama_vulkan; a no-op otherwise):
//  - loads the Vulkan loader from System32 (vulkan-1.dll, which GPU drivers
//    install; never one from the application or current directory);
//  - creates an instance, and on each integrated or discrete GPU a device
//    with one compute queue and a trivial compute pipeline, which makes the
//    driver load its user-mode modules and its shader compiler
//    (molt_gpu_driver_warmup.h);
//  - pins every module loaded meanwhile (GET_MODULE_HANDLE_EX_FLAG_PIN), so
//    destroying the device and instance doesn't unload them;
//  - destroys the device and instance.
// The backend later finds the loader by name and the loader finds the
// drivers already mapped. Any failure leaves the process as it would have
// been: the model then loads on the CPU (kNoDevice). A driver that crashes
// here crashes the process before it reported a device, which the browser
// counts against the unidentified GPU entry (section 11.10).
void MaybeWarmMoltGpuInferenceDrivers(const base::CommandLine& command_line);

}  // namespace on_device_model

#endif  // CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SANDBOX_WIN_H_
