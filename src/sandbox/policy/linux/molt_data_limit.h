// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the data limit (RLIMIT_DATA) of MoltNet's renderers on Linux.
//
// Chromium 156 limits a renderer's data segment by the machine's memory
// (GetProcessDataSizeLimit, sandbox_linux.cc): 8 GiB up to 16 GB, 16 GiB up
// to 32 GB, 32 GiB above (twice that with kHigherRendererMemoryLimit). The
// limit counts memory a process has mapped writable, whether it touched it
// or not, so a page that allocates ArrayBuffers or grows WebAssembly
// memories until it is refused (a RangeError it can catch) learns the tier
// in a moment, without using the memory. MoltNet's renderers get the
// smallest tier on every machine: 8 GiB, which every machine up to 16 GB
// already has, so it is no less than what the smallest one gives them.
//
// A MoltNet renderer is one the browser gave --molt-fingerprinting-mode=
// uniform (chrome/common/molt_ai/fingerprinting/
// molt_fingerprinting_switches.h; molt_ai_unittests checks that the names
// agree). Without fingerprinting protection (its kill switch) the switch is
// absent and Chromium's limit applies.

#ifndef SANDBOX_POLICY_LINUX_MOLT_DATA_LIMIT_H_
#define SANDBOX_POLICY_LINUX_MOLT_DATA_LIMIT_H_

#include <algorithm>
#include <cstdint>

#include "base/command_line.h"

namespace sandbox::policy::molt {

inline constexpr char kFingerprintingModeSwitch[] = "molt-fingerprinting-mode";
inline constexpr char kUniformMode[] = "uniform";
inline constexpr uint64_t kMoltNetRendererDataLimit = uint64_t{8} << 30;

// The data limit for a process with `command_line`, whose sandbox
// (`is_renderer`) Chromium gives `chromium_limit`.
inline uint64_t ProcessDataSizeLimit(const base::CommandLine& command_line,
                                     bool is_renderer,
                                     uint64_t chromium_limit) {
  if (!is_renderer || command_line.GetSwitchValueASCII(
                          kFingerprintingModeSwitch) != kUniformMode) {
    return chromium_limit;
  }
  return std::min(chromium_limit, kMoltNetRendererDataLimit);
}

}  // namespace sandbox::policy::molt

#endif  // SANDBOX_POLICY_LINUX_MOLT_DATA_LIMIT_H_
