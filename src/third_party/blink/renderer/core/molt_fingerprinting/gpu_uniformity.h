// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: WebGL and WebGPU in MoltNet windows.
//
// MoltNet (kUniform) wants every user to look the same. WebGL's pixels are
// farbled like everywhere else and its unmasked GPU strings are hidden
// (molt_fingerprinting.h); this file covers what is left:
//
//   * WebGL 1 and 2 describe the GPU through their limits, extension list
//     and shader precision formats. MoltNet pages get the fixed baselines of
//     webgl_uniform.h instead, clamped so that no capability the GPU lacks is
//     ever claimed.
//   * WebGPU (GPUAdapter.info, limits, features) has no adapter at all:
//     navigator.gpu.requestAdapter() resolves to null, which the spec allows
//     and which sites already handle by falling back to WebGL.
//
// Upstream hooks (each marked "MoltBrowser:"):
//
//   webgl  WebGLRenderingContextBase::ExtensionSupportedAndAllowed
//            (getSupportedExtensions, getExtension)  -> WebGLExtensionAllowed
//          WebGLRenderingContextBase::GetIntParameter, getParameter
//            (WEBGL_draw_buffers limits)              -> UniformWebGLInt
//          WebGL2RenderingContextBase::GetInt64Parameter -> UniformWebGLInt64
//          WebGLRenderingContextBase::GetFloatParameter  -> UniformWebGLFloat
//          WebGLRenderingContextBase::GetWebGLFloatArrayParameter
//                                                     -> UniformWebGLFloats
//          WebGLRenderingContextBase::GetWebGLIntArrayParameter
//                                                     -> UniformWebGLInts
//          WebGLRenderingContextBase::getShaderPrecisionFormat
//                                             -> UniformShaderPrecisionFormat
//          WebGL2RenderingContextBase::getInternalformatParameter (SAMPLES)
//                                                     -> UniformSampleCounts
//   webgpu GPU::RequestAdapterImpl                    -> HidesWebGPUAdapter
//
// Normal and Incognito windows (kFarble) are left alone: there the goal is
// per-site unlinkability without breaking sites, and fixed limits would
// break WebGL apps without making a user look like anybody in particular
// (docs/FINGERPRINTING.md).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_GPU_UNIFORMITY_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_GPU_UNIFORMITY_H_

#include <cstddef>
#include <cstdint>

#include "base/containers/span.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace blink {

class ExecutionContext;

namespace molt_fingerprinting {

// Whether WebGL and WebGPU in `context` must look the same for every MoltNet
// user: the process is in kUniform mode and `context` is web content (its
// top-level site is http(s) or opaque; chrome:// pages and the like keep
// the real GPU). A null context counts as web content.
CORE_EXPORT bool UsesUniformGpu(ExecutionContext* context);

// --- WebGL ------------------------------------------------------------------
// `context` is the WebGL context's execution context (the canvas's document
// or worker). Outside MoltNet every function returns / leaves the real
// values.

// getParameter(`pname`) when the GPU says `real`. Parameters that aren't
// implementation-dependent limits are returned unchanged.
CORE_EXPORT int32_t UniformWebGLInt(ExecutionContext* context,
                                    uint32_t pname,
                                    int32_t real);
CORE_EXPORT int64_t UniformWebGLInt64(ExecutionContext* context,
                                      uint32_t pname,
                                      int64_t real);
CORE_EXPORT float UniformWebGLFloat(ExecutionContext* context,
                                    uint32_t pname,
                                    float real);
// Array parameters, in place: ALIASED_POINT_SIZE_RANGE and
// ALIASED_LINE_WIDTH_RANGE (floats), MAX_VIEWPORT_DIMS (ints).
CORE_EXPORT void UniformWebGLFloats(ExecutionContext* context,
                                    uint32_t pname,
                                    base::span<float> values);
CORE_EXPORT void UniformWebGLInts(ExecutionContext* context,
                                  uint32_t pname,
                                  base::span<int32_t> values);

struct ShaderPrecisionFormat {
  int32_t range_min;
  int32_t range_max;
  int32_t precision;
};
// getShaderPrecisionFormat(shader type, `precision_type`).
CORE_EXPORT ShaderPrecisionFormat
UniformShaderPrecisionFormat(ExecutionContext* context,
                             uint32_t precision_type,
                             const ShaderPrecisionFormat& real);

// getInternalformatParameter(RENDERBUFFER, format, SAMPLES): moves the
// sample counts the page may see to the front of `counts` and returns how
// many there are (all of them outside MoltNet).
CORE_EXPORT size_t UniformSampleCounts(ExecutionContext* context,
                                       base::span<int32_t> counts);

// Whether getSupportedExtensions() / getExtension() may expose the
// extension whose canonical name is `name` (one the GPU supports).
CORE_EXPORT bool WebGLExtensionAllowed(ExecutionContext* context,
                                       const char* name);

// --- WebGPU -----------------------------------------------------------------

// Whether navigator.gpu.requestAdapter() in `context` resolves to null
// (windows and workers alike).
CORE_EXPORT bool HidesWebGPUAdapter(ExecutionContext* context);

// --- Tests ------------------------------------------------------------------

// Makes UsesUniformGpu() behave as in kUniform mode without configuring the
// process for it (which would pin the time zone and locale for every later
// test). The context must still be farbled, so tests configure kFarble.
CORE_EXPORT void SetUniformGpuForTesting(bool uniform);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_GPU_UNIFORMITY_H_
