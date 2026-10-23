// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the x86-64 CPU kernel dispatch (see ggml-cpu-x86-dispatch.h
// and ggml-cpu-x86-variants.h; docs/design/SANDBOXED_INFERENCE.md section
// 12). Built with Chromium's baseline flags, like the rest of ggml-cpu.
//
// Each level's build of arch/x86/quants.c and arch/x86/repack.cpp defines
// molt_x86_<level>_<name> for the functions GGML_CPU_X86_KERNELS lists.
// This file defines <name> itself: a thunk that jumps to the function of
// the level in use. ggml-cpu.c's type traits table and ggml-cpu/repack.cpp
// call these names, so they are unchanged.

#include "chromium/ggml-cpu-x86-dispatch.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// ggml's own declarations of the functions, so a signature here that
// doesn't match upstream's is a compile error. Same preamble as
// ggml-cpu/repack.cpp.
#define GGML_COMMON_DECL_CPP
#include "ggml-common.h"
#include "ggml-cpu/quants.h"
#include "ggml-cpu/repack.h"

#include "chromium/ggml-cpu-x86-variants.h"

// Each level's renamed functions, with upstream's types.
extern "C" {
#define MOLT_X86_DECLARE(kind, name)   \
  decltype(::name) molt_x86_v2_##name; \
  decltype(::name) molt_x86_avx_##name; \
  decltype(::name) molt_x86_avx2_##name;
GGML_CPU_X86_KERNELS(MOLT_X86_DECLARE)
#undef MOLT_X86_DECLARE
}

namespace molt_llama {
namespace {

struct Kernels {
#define MOLT_X86_FIELD(kind, name) decltype(&::name) name;
  GGML_CPU_X86_KERNELS(MOLT_X86_FIELD)
#undef MOLT_X86_FIELD
};

#define MOLT_X86_V2(kind, name) &molt_x86_v2_##name,
#define MOLT_X86_AVX(kind, name) &molt_x86_avx_##name,
#define MOLT_X86_AVX2(kind, name) &molt_x86_avx2_##name,
constexpr Kernels kX86_64_V2Kernels = {GGML_CPU_X86_KERNELS(MOLT_X86_V2)};
constexpr Kernels kAvxKernels = {GGML_CPU_X86_KERNELS(MOLT_X86_AVX)};
constexpr Kernels kAvx2Kernels = {GGML_CPU_X86_KERNELS(MOLT_X86_AVX2)};
#undef MOLT_X86_V2
#undef MOLT_X86_AVX
#undef MOLT_X86_AVX2

// Until the service chooses: the set every accepted CPU can run. Constant-
// initialized (no static initializer).
constinit std::atomic<const Kernels*> g_kernels{&kX86_64_V2Kernels};

const Kernels* TableFor(X86CpuKernels kernels) {
  switch (kernels) {
    case X86CpuKernels::kX86_64_V2:
      return &kX86_64_V2Kernels;
    case X86CpuKernels::kAvx:
      return &kAvxKernels;
    case X86CpuKernels::kAvx2:
      return &kAvx2Kernels;
  }
  return &kX86_64_V2Kernels;
}

}  // namespace

void UseX86CpuKernels(X86CpuKernels kernels) {
  g_kernels.store(TableFor(kernels), std::memory_order_release);
}

X86CpuKernels CurrentX86CpuKernels() {
  const Kernels* const k = g_kernels.load(std::memory_order_acquire);
  if (k == &kAvx2Kernels) return X86CpuKernels::kAvx2;
  if (k == &kAvxKernels) return X86CpuKernels::kAvx;
  return X86CpuKernels::kX86_64_V2;
}

}  // namespace molt_llama

// The thunks, under ggml's names. One load and an indirect jump (or call)
// per kernel call; a kernel call computes a whole row.
#define MOLT_X86_PARAMS_quantize_row \
  (const float* GGML_RESTRICT x, void* GGML_RESTRICT y, int64_t k)
#define MOLT_X86_ARGS_quantize_row (x, y, k)
#define MOLT_X86_PARAMS_vec_dot                                             \
  (int n, float* GGML_RESTRICT s, size_t bs, const void* GGML_RESTRICT vx, \
   size_t bx, const void* GGML_RESTRICT vy, size_t by, int nrc)
#define MOLT_X86_ARGS_vec_dot (n, s, bs, vx, bx, vy, by, nrc)
#define MOLT_X86_PARAMS_quantize_mat \
  (const float* GGML_RESTRICT x, void* GGML_RESTRICT vy, int64_t k)
#define MOLT_X86_ARGS_quantize_mat (x, vy, k)
#define MOLT_X86_PARAMS_gemx                                                \
  (int n, float* GGML_RESTRICT s, size_t bs, const void* GGML_RESTRICT vx, \
   const void* GGML_RESTRICT vy, int nr, int nc)
#define MOLT_X86_ARGS_gemx (n, s, bs, vx, vy, nr, nc)

extern "C" {
#define MOLT_X86_THUNK(kind, name)                                       \
  void name MOLT_X86_PARAMS_##kind {                                     \
    molt_llama::g_kernels.load(std::memory_order_acquire)->name          \
        MOLT_X86_ARGS_##kind;                                            \
  }
GGML_CPU_X86_KERNELS(MOLT_X86_THUNK)
#undef MOLT_X86_THUNK
}
