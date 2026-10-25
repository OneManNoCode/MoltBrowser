// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "content/utility/on_device_model/molt_gpu_inference_sandbox_win.h"

#include <windows.h>

#include <stdint.h>

#include <vector>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/win/win_util.h"
#include "content/utility/on_device_model/molt_gpu_driver_warmup.h"
#include "content/utility/on_device_model/molt_gpu_inference_buildflags.h"
#include "content/utility/on_device_model/molt_gpu_inference_switch.h"

namespace on_device_model {

#if BUILDFLAG(MOLT_GPU_INFERENCE)

namespace {

std::vector<uintptr_t> LoadedModules() {
  std::vector<HMODULE> modules;
  std::vector<uintptr_t> out;
  if (!base::win::GetLoadedModulesSnapshot(::GetCurrentProcess(), &modules)) {
    return out;
  }
  out.reserve(modules.size());
  for (HMODULE m : modules) {
    out.push_back(reinterpret_cast<uintptr_t>(m));
  }
  return out;
}

struct PinState {
  std::vector<uintptr_t> before;
  size_t pinned = 0;
  size_t failed = 0;
};

// Runs while the warm-up's instance and devices are alive: every module
// that wasn't loaded before (the loader, the drivers and what they loaded)
// stays loaded for the life of the process.
void PinNewModules(void* context) {
  PinState* state = static_cast<PinState*>(context);
  for (uintptr_t m : NewModules(state->before, LoadedModules())) {
    HMODULE pinned = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(m), &pinned)) {
      ++state->pinned;
    } else {
      ++state->failed;
    }
  }
}

// Vulkan layers stay out of the process: overlays and capture tools that
// register implicit layers, and layers named through the environment. Only
// the loader and the drivers are loaded (and pinned) here, and the
// backend's own instance later sees the same environment. Loaders before
// 1.3.234 ignore VK_LOADER_LAYERS_DISABLE.
void KeepVulkanLayersOut() {
  for (const wchar_t* name :
       {L"VK_INSTANCE_LAYERS", L"VK_LOADER_LAYERS_ENABLE", L"VK_LAYER_PATH",
        L"VK_ADD_LAYER_PATH", L"VK_IMPLICIT_LAYER_PATH",
        L"VK_ADD_IMPLICIT_LAYER_PATH"}) {
    ::SetEnvironmentVariableW(name, nullptr);
  }
  ::SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", L"~all~");
}

}  // namespace

void MaybeWarmMoltGpuInferenceDrivers(const base::CommandLine& command_line) {
  if (!command_line.HasSwitch(kMoltInferenceGpuSwitch)) {
    return;
  }
  KeepVulkanLayersOut();
  PinState state;
  state.before = LoadedModules();

  // System32's loader only: the one GPU drivers install. Never a
  // vulkan-1.dll from the application directory, the current directory or
  // PATH.
  HMODULE loader =
      ::LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!loader) {
    LOG(WARNING) << "On-device AI: no Vulkan loader; the model runs on the "
                    "processor";
    return;
  }
  auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      ::GetProcAddress(loader, "vkGetInstanceProcAddr"));
  MoltGpuWarmupResult result;
  if (get_instance_proc_addr) {
    result = WarmUpVulkanDrivers(get_instance_proc_addr, &PinNewModules,
                                 &state);
  }
  if (!result.while_loaded_called) {
    // No instance: keep the loader itself loaded anyway (the backend looks
    // for it by name), and nothing else was loaded.
    PinNewModules(&state);
  }
  // The device checklist reads this line (docs/BUILD_MACHINE_CHECKLIST.md).
  LOG(INFO) << "On-device AI GPU warm-up: instance "
            << (result.instance ? 1 : 0) << ", GPUs " << result.gpus
            << ", devices " << result.devices << ", pipelines "
            << result.pipelines << ", modules pinned " << state.pinned
            << ", not pinned " << state.failed;
}

#else  // BUILDFLAG(MOLT_GPU_INFERENCE)

void MaybeWarmMoltGpuInferenceDrivers(const base::CommandLine&) {}

#endif  // BUILDFLAG(MOLT_GPU_INFERENCE)

}  // namespace on_device_model
