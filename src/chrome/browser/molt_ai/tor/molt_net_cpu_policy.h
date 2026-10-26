// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The processors MoltNet opens on.
//
// MoltNet pages get the same answers on every computer. Most answers that
// could follow the processor's features are computed one way in MoltNet's
// renderers (docs/FINGERPRINTING.md, "MoltNet: CPU features" and "MoltNet:
// what pages can learn about the processor"). One can't be: V8 compiles
// WebAssembly SIMD on x86-64 only with SSE4.1 (CpuFeatures::SupportsSimd128()
// at Chromium 156; SSE4.1 counts only with SSSE3 and SSE3), and without it a
// module that uses SIMD fails validation (WebAssembly.validate() is false).
// Chromium itself runs on x86-64 processors with SSE3 only, so on those few
// machines (Intel before about 2008, AMD before about 2011) a page could
// tell them apart. Turning SIMD off for every MoltNet page would slow all of
// them for machines that are already rare and slow enough to stand out.
// So MoltNet doesn't open on such a processor; the rest of the browser is
// unchanged.
//
// MoltNet's requirement, checked once per browser run with base::CPU in the
// browser process (molt_net_cpu.h):
//   * x86-64: SSE3, SSSE3 and SSE4.1 (CPUID leaf 1, ECX bits 0, 9 and 19,
//     the bits V8 reads). Every other x86-64 feature is either made
//     uniform in MoltNet or changes no answer.
//   * arm64: none beyond the ARMv8-A baseline Chromium already needs.
//   * Any other processor architecture: refused. MoltNet's uniform answers
//     were checked on x86-64 and arm64 only.
//
// Every way into MoltNet checks it before anything starts: no Tor process
// (TorManager::Launch()), no MoltNet profile
// (MoltNetRouting::GetOrCreateMoltNetProfile()) and no MoltNet window
// (MoltNetWindow::Open(), NewIdentity()), and MoltNet's pages say why up
// front.
//
// Header-only and std-only, so molt_ai_unittests tests it.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_CPU_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_CPU_POLICY_H_

#include <string>

namespace molt_ai::tor {

enum class MoltNetCpuArch {
  kX86_64,
  kArm64,
  // Anything else (32-bit builds, other architectures).
  kOther,
};

// What base::CPU reports. Every field defaults to the refusing value, so a
// field nobody filled in refuses.
struct MoltNetCpuFeatures {
  MoltNetCpuArch arch = MoltNetCpuArch::kOther;
  // x86-64 only; ignored elsewhere.
  bool sse3 = false;
  bool ssse3 = false;
  bool sse41 = false;
};

enum class MoltNetCpuCheck {
  kOk,
  // An x86-64 processor without SSE4.1 (or without SSSE3 or SSE3, without
  // which V8 doesn't use SSE4.1).
  kNoSse41,
  // Not an x86-64 or arm64 processor.
  kUnsupportedArch,
};

constexpr MoltNetCpuCheck CheckMoltNetCpu(const MoltNetCpuFeatures& cpu) {
  switch (cpu.arch) {
    case MoltNetCpuArch::kX86_64:
      return cpu.sse3 && cpu.ssse3 && cpu.sse41 ? MoltNetCpuCheck::kOk
                                                : MoltNetCpuCheck::kNoSse41;
    case MoltNetCpuArch::kArm64:
      return MoltNetCpuCheck::kOk;
    case MoltNetCpuArch::kOther:
      break;
  }
  return MoltNetCpuCheck::kUnsupportedArch;
}

// The one sentence every way into MoltNet shows instead of opening it.
// Empty for kOk.
inline std::string MoltNetCpuMessage(MoltNetCpuCheck check) {
  switch (check) {
    case MoltNetCpuCheck::kOk:
      return std::string();
    case MoltNetCpuCheck::kNoSse41:
      return "MoltNet can't open on this computer: its processor lacks "
             "SSE4.1 (Intel processors from before about 2008, AMD ones from "
             "before about 2011), which MoltNet needs to give pages the same "
             "answers as on other computers.";
    case MoltNetCpuCheck::kUnsupportedArch:
      break;
  }
  return "MoltNet can't open on this computer: it gives pages the same "
         "answers only on x86-64 and arm64 processors.";
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_CPU_POLICY_H_
