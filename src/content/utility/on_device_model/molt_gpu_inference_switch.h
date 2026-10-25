// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SWITCH_H_
#define CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SWITCH_H_

namespace on_device_model {

// MoltBrowser: on the command line of an inference service the browser
// launched for the GPU (docs/design/SANDBOXED_INFERENCE.md section 11). The
// same string as molt_ai::kMoltInferenceGpuSwitch
// (chrome/browser/molt_ai/runtime/inference_process_host.h), which content
// can't include. The browser adds it only in builds with the Vulkan backend
// (molt_llama_vulkan) when it asks for the GPU; no other process of the
// on-device model sandbox type has it.
inline constexpr char kMoltInferenceGpuSwitch[] = "molt-inference-gpu";

}  // namespace on_device_model

#endif  // CONTENT_UTILITY_ON_DEVICE_MODEL_MOLT_GPU_INFERENCE_SWITCH_H_
