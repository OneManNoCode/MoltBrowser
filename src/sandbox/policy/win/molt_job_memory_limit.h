// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the memory MoltNet's renderers may commit on Windows.
//
// Chromium 156 puts each renderer in a job whose limit on committed memory is
// 1 TB (SandboxWin::GetJobMemoryLimit), so what stops a page that allocates
// ArrayBuffers or grows WebAssembly memories is the system's commit limit
// (installed RAM plus page file, less what everything else has committed): a
// RangeError it can catch shows how much memory the machine has, and how
// much of it is in use. MoltNet's renderers get a job limit of 8 GiB on every
// machine, as their data limit on Linux (sandbox/policy/linux/
// molt_data_limit.h) and the renderer limit Chromium used to give machines up
// to 16 GB: a page that asks for more is refused at the same point on every
// machine that has that much to commit. A machine that hasn't still refuses
// earlier, as it would any program.
//
// A MoltNet renderer is one the browser gave --molt-fingerprinting-mode=
// uniform (chrome/common/molt_ai/fingerprinting/
// molt_fingerprinting_switches.h; molt_ai_unittests checks that the names
// agree). Without fingerprinting protection (its kill switch) the switch is
// absent and Chromium's limit applies.

#ifndef SANDBOX_POLICY_WIN_MOLT_JOB_MEMORY_LIMIT_H_
#define SANDBOX_POLICY_WIN_MOLT_JOB_MEMORY_LIMIT_H_

#include <cstdint>

#include "base/command_line.h"

namespace sandbox::policy::molt_win {

inline constexpr char kFingerprintingModeSwitch[] = "molt-fingerprinting-mode";
inline constexpr char kUniformMode[] = "uniform";
inline constexpr uint64_t kMoltNetRendererJobMemoryLimit = uint64_t{8} << 30;

// Whether `command_line` is a MoltNet renderer's. On 64-bit Windows, where
// Chromium's renderer limit is 1 TB, the sandbox then sets the job's limit to
// kMoltNetRendererJobMemoryLimit (sandbox_win.cc,
// GenerateConfigForSandboxedProcess).
inline bool IsMoltNetRenderer(const base::CommandLine& command_line) {
  return command_line.GetSwitchValueASCII(kFingerprintingModeSwitch) ==
         kUniformMode;
}

}  // namespace sandbox::policy::molt_win

#endif  // SANDBOX_POLICY_WIN_MOLT_JOB_MEMORY_LIMIT_H_
