// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: which of ggml's x86-64 CPU kernel sets the CPU backend uses
// (docs/design/SANDBOXED_INFERENCE.md section 12). x64 builds only: BUILD.gn
// defines GGML_MOLTBROWSER_X86_CPU_KERNELS for code that may include this.
//
// The inference service picks the set
// (//chrome/services/molt_ai/inference/engine/cpu_kernels.h) and calls
// UseX86CpuKernels() before any ggml computation. Until then the
// x86-64-v2 set is used, which every CPU the service accepts can run.

#ifndef THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_CPU_X86_DISPATCH_H_
#define THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_CPU_X86_DISPATCH_H_

namespace molt_llama {

// One per level BUILD.gn compiles ggml's arch/x86 files for.
enum class X86CpuKernels {
  // x86-64-v2: SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT.
  kX86_64_V2,
  // The above and AVX (Sandy Bridge, Bulldozer).
  kAvx,
  // The above and AVX2, FMA, F16C (Haswell, Excavator, Zen): the flags
  // //third_party/llama_cpp used for every x64 CPU before.
  kAvx2,
};

// From now on, ggml's CPU backend runs |kernels|. The caller has checked
// that this CPU (and its OS) supports them: running a set the CPU lacks
// ends the process with an illegal-instruction fault. Thread-safe, but call
// it before ggml computes anything: a computation already running may mix
// the old and the new set (both correct, with slightly different
// rounding).
void UseX86CpuKernels(X86CpuKernels kernels);

// The set in use.
X86CpuKernels CurrentX86CpuKernels();

}  // namespace molt_llama

#endif  // THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_CPU_X86_DISPATCH_H_
