// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Local State prefs of GPU inference (docs/design/SANDBOXED_INFERENCE.md
// sections 11.10 and 11.11). Browser-wide, like BrowserAIRuntime, and never
// in a profile: they are about the machine's GPU, not browsing.
//
//   molt_ai.gpu_inference.mode            int, GpuInferenceMode
//   molt_ai.gpu_inference.driver_records  list of GpuDriverRecords entries:
//     {"key": GpuDriverKeyToString(), "name": string,
//      "failures": [ms, ...], "last_event": ms, "off": bool,
//      "off_since": ms, "off_version": string, "probation": bool}
//
// What is read back is validated like the service's input: an entry with a
// bad key or a field of the wrong type is dropped, and
// GpuDriverRecords::Restore() bounds the rest.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_INFERENCE_PREFS_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_INFERENCE_PREFS_H_

#include <vector>

#include "base/values.h"
#include "chrome/browser/molt_ai/runtime/gpu_driver_record.h"

class PrefRegistrySimple;

namespace molt_ai {

inline constexpr char kGpuInferenceModePref[] = "molt_ai.gpu_inference.mode";
inline constexpr char kGpuDriverRecordsPref[] =
    "molt_ai.gpu_inference.driver_records";

// "Use the GPU for on-device AI" on molt://ai-settings.
enum class GpuInferenceMode {
  // The GPU when its driver works (the record), else the CPU.
  kAutomatic = 0,
  // Never the GPU.
  kOff = 1,
};

// Called from ChromeContentBrowserClient::RegisterLocalStatePrefs().
void RegisterGpuInferenceLocalStatePrefs(PrefRegistrySimple* registry);

// An unknown value is kAutomatic.
GpuInferenceMode GpuInferenceModeFromPref(int value);

base::ListValue GpuDriverEntriesToPref(
    const std::vector<GpuDriverEntry>& entries);
std::vector<GpuDriverEntry> GpuDriverEntriesFromPref(
    const base::ListValue& list);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_INFERENCE_PREFS_H_
