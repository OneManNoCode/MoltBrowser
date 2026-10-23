// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: ggml's x86-64 CPU kernels, one set per instruction-set level,
// chosen at run time (docs/design/SANDBOXED_INFERENCE.md section 12).
//
// Only two ggml-cpu files are built with instruction-set flags:
// ggml/src/ggml-cpu/arch/x86/quants.c and arch/x86/repack.cpp. BUILD.gn
// compiles them once per level (:ggml_cpu_x86_<level>) with this header
// force-included and GGML_CPU_X86_VARIANT_PREFIX set to that level's prefix
// (molt_x86_v2_, molt_x86_avx_, molt_x86_avx2_), which renames the functions
// they define: quantize_row_q8_0 becomes molt_x86_avx2_quantize_row_q8_0.
// chromium/ggml-cpu-x86-dispatch.cpp defines the original names as thunks
// that jump to the chosen level's function, so ggml's callers (the type
// traits table in ggml-cpu.c, ggml-cpu/repack.cpp) are unchanged.
//
// The two lists below must name exactly the functions those two files
// define; a mismatch is a link error, not a silent fallback:
//  - a function missing from the rename list is defined once per level
//    under its own name (duplicate symbol);
//  - a function renamed but missing from GGML_CPU_X86_KERNELS has no thunk,
//    so ggml's references to it are unresolved.
// Update both with every llama.cpp update (README.chromium), and check that
// the level objects still define no weak (COMDAT) symbol: the linker keeps
// one copy of such a symbol for the whole binary, which could hand AVX2 code
// to a CPU without AVX2.

#ifndef THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_CPU_X86_VARIANTS_H_
#define THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_CPU_X86_VARIANTS_H_

// X(kind, name) for each function, by its signature:
//   quantize_row  void(const float* x, void* y, int64_t k)
//   vec_dot       void(int n, float* s, size_t bs, const void* vx, size_t bx,
//                      const void* vy, size_t by, int nrc)
//   quantize_mat  void(const float* x, void* vy, int64_t k)
//   gemx          void(int n, float* s, size_t bs, const void* vx,
//                      const void* vy, int nr, int nc)
#define GGML_CPU_X86_KERNELS(X)                \
  /* arch/x86/quants.c */                      \
  X(quantize_row, quantize_row_q8_0)           \
  X(quantize_row, quantize_row_q8_1)           \
  X(quantize_row, quantize_row_q8_K)           \
  X(vec_dot, ggml_vec_dot_q1_0_q8_0)           \
  X(vec_dot, ggml_vec_dot_q2_0_q8_0)           \
  X(vec_dot, ggml_vec_dot_q4_0_q8_0)           \
  X(vec_dot, ggml_vec_dot_q4_1_q8_1)           \
  X(vec_dot, ggml_vec_dot_q5_0_q8_0)           \
  X(vec_dot, ggml_vec_dot_q5_1_q8_1)           \
  X(vec_dot, ggml_vec_dot_q8_0_q8_0)           \
  X(vec_dot, ggml_vec_dot_mxfp4_q8_0)          \
  X(vec_dot, ggml_vec_dot_nvfp4_q8_0)          \
  X(vec_dot, ggml_vec_dot_q2_K_q8_K)           \
  X(vec_dot, ggml_vec_dot_q3_K_q8_K)           \
  X(vec_dot, ggml_vec_dot_q4_K_q8_K)           \
  X(vec_dot, ggml_vec_dot_q5_K_q8_K)           \
  X(vec_dot, ggml_vec_dot_q6_K_q8_K)           \
  X(vec_dot, ggml_vec_dot_tq1_0_q8_K)          \
  X(vec_dot, ggml_vec_dot_tq2_0_q8_K)          \
  X(vec_dot, ggml_vec_dot_iq2_xxs_q8_K)        \
  X(vec_dot, ggml_vec_dot_iq2_xs_q8_K)         \
  X(vec_dot, ggml_vec_dot_iq2_s_q8_K)          \
  X(vec_dot, ggml_vec_dot_iq3_xxs_q8_K)        \
  X(vec_dot, ggml_vec_dot_iq3_s_q8_K)          \
  X(vec_dot, ggml_vec_dot_iq1_s_q8_K)          \
  X(vec_dot, ggml_vec_dot_iq1_m_q8_K)          \
  X(vec_dot, ggml_vec_dot_iq4_nl_q8_0)         \
  X(vec_dot, ggml_vec_dot_iq4_xs_q8_K)         \
  /* arch/x86/repack.cpp */                    \
  X(quantize_mat, ggml_quantize_mat_q8_0_4x8)  \
  X(quantize_mat, ggml_quantize_mat_q8_K_4x8)  \
  X(gemx, ggml_gemv_q4_0_8x8_q8_0)             \
  X(gemx, ggml_gemv_q4_K_8x8_q8_K)             \
  X(gemx, ggml_gemv_q2_K_8x8_q8_K)             \
  X(gemx, ggml_gemv_iq4_nl_8x8_q8_0)           \
  X(gemx, ggml_gemv_mxfp4_8x8_q8_0)            \
  X(gemx, ggml_gemm_q4_0_8x8_q8_0)             \
  X(gemx, ggml_gemm_q4_K_8x8_q8_K)             \
  X(gemx, ggml_gemm_q2_K_8x8_q8_K)             \
  X(gemx, ggml_gemm_iq4_nl_8x8_q8_0)           \
  X(gemx, ggml_gemm_mxfp4_8x8_q8_0)

// The renames, only in a level's build of the two files.
#if defined(GGML_CPU_X86_VARIANT_PREFIX)

#define GGML_CPU_X86_CAT_(a, b) a##b
#define GGML_CPU_X86_CAT(a, b) GGML_CPU_X86_CAT_(a, b)
#define GGML_CPU_X86_NAME(name) GGML_CPU_X86_CAT(GGML_CPU_X86_VARIANT_PREFIX, name)

// arch/x86/quants.c
#define quantize_row_q8_0 GGML_CPU_X86_NAME(quantize_row_q8_0)
#define quantize_row_q8_1 GGML_CPU_X86_NAME(quantize_row_q8_1)
#define quantize_row_q8_K GGML_CPU_X86_NAME(quantize_row_q8_K)
#define ggml_vec_dot_q1_0_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_q1_0_q8_0)
#define ggml_vec_dot_q2_0_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_q2_0_q8_0)
#define ggml_vec_dot_q4_0_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_q4_0_q8_0)
#define ggml_vec_dot_q4_1_q8_1 GGML_CPU_X86_NAME(ggml_vec_dot_q4_1_q8_1)
#define ggml_vec_dot_q5_0_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_q5_0_q8_0)
#define ggml_vec_dot_q5_1_q8_1 GGML_CPU_X86_NAME(ggml_vec_dot_q5_1_q8_1)
#define ggml_vec_dot_q8_0_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_q8_0_q8_0)
#define ggml_vec_dot_mxfp4_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_mxfp4_q8_0)
#define ggml_vec_dot_nvfp4_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_nvfp4_q8_0)
#define ggml_vec_dot_q2_K_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_q2_K_q8_K)
#define ggml_vec_dot_q3_K_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_q3_K_q8_K)
#define ggml_vec_dot_q4_K_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_q4_K_q8_K)
#define ggml_vec_dot_q5_K_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_q5_K_q8_K)
#define ggml_vec_dot_q6_K_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_q6_K_q8_K)
#define ggml_vec_dot_tq1_0_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_tq1_0_q8_K)
#define ggml_vec_dot_tq2_0_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_tq2_0_q8_K)
#define ggml_vec_dot_iq2_xxs_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq2_xxs_q8_K)
#define ggml_vec_dot_iq2_xs_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq2_xs_q8_K)
#define ggml_vec_dot_iq2_s_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq2_s_q8_K)
#define ggml_vec_dot_iq3_xxs_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq3_xxs_q8_K)
#define ggml_vec_dot_iq3_s_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq3_s_q8_K)
#define ggml_vec_dot_iq1_s_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq1_s_q8_K)
#define ggml_vec_dot_iq1_m_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq1_m_q8_K)
#define ggml_vec_dot_iq4_nl_q8_0 GGML_CPU_X86_NAME(ggml_vec_dot_iq4_nl_q8_0)
#define ggml_vec_dot_iq4_xs_q8_K GGML_CPU_X86_NAME(ggml_vec_dot_iq4_xs_q8_K)

// arch/x86/repack.cpp
#define ggml_quantize_mat_q8_0_4x8 GGML_CPU_X86_NAME(ggml_quantize_mat_q8_0_4x8)
#define ggml_quantize_mat_q8_K_4x8 GGML_CPU_X86_NAME(ggml_quantize_mat_q8_K_4x8)
#define ggml_gemv_q4_0_8x8_q8_0 GGML_CPU_X86_NAME(ggml_gemv_q4_0_8x8_q8_0)
#define ggml_gemv_q4_K_8x8_q8_K GGML_CPU_X86_NAME(ggml_gemv_q4_K_8x8_q8_K)
#define ggml_gemv_q2_K_8x8_q8_K GGML_CPU_X86_NAME(ggml_gemv_q2_K_8x8_q8_K)
#define ggml_gemv_iq4_nl_8x8_q8_0 GGML_CPU_X86_NAME(ggml_gemv_iq4_nl_8x8_q8_0)
#define ggml_gemv_mxfp4_8x8_q8_0 GGML_CPU_X86_NAME(ggml_gemv_mxfp4_8x8_q8_0)
#define ggml_gemm_q4_0_8x8_q8_0 GGML_CPU_X86_NAME(ggml_gemm_q4_0_8x8_q8_0)
#define ggml_gemm_q4_K_8x8_q8_K GGML_CPU_X86_NAME(ggml_gemm_q4_K_8x8_q8_K)
#define ggml_gemm_q2_K_8x8_q8_K GGML_CPU_X86_NAME(ggml_gemm_q2_K_8x8_q8_K)
#define ggml_gemm_iq4_nl_8x8_q8_0 GGML_CPU_X86_NAME(ggml_gemm_iq4_nl_8x8_q8_0)
#define ggml_gemm_mxfp4_8x8_q8_0 GGML_CPU_X86_NAME(ggml_gemm_mxfp4_8x8_q8_0)

#endif  // defined(GGML_CPU_X86_VARIANT_PREFIX)

#endif  // THIRD_PARTY_LLAMA_CPP_CHROMIUM_GGML_CPU_X86_VARIANTS_H_
