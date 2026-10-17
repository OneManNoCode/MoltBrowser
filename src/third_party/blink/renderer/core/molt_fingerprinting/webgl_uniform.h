// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: what WebGL says about the GPU in
// MoltNet windows, as pure data and functions.
//
// MoltNet (kUniform) wants every user to look the same. Besides its pixels
// (farbled, see farbling.h) and the unmasked GPU strings (hidden), WebGL
// describes the GPU through its implementation-dependent limits, its
// extension list and its shader precision formats. In MoltNet windows Blink
// reports, for each of them, a fixed baseline clamped to the real GPU:
//
//  * limits: min(real, baseline), or max(real, baseline) for the few where
//    the larger number is the weaker claim (an alignment, a negative offset,
//    the low end of a range);
//  * extensions: a fixed allow-list, intersected with what is supported;
//  * precision formats: the component-wise min of real and baseline.
//
// So a page is never told about a capability the GPU lacks, and every GPU
// that meets the baselines -- essentially every desktop GPU that runs WebGL
// 2, see docs/FINGERPRINTING.md for how they were picked -- reports exactly
// the same values. The reported values are not enforced: the GPU's real
// limits still apply, so a page that goes beyond them keeps working.
//
// Like farbling.cc this depends only on //base, so it is tested standalone
// (webgl_uniform_unittest.cc). The Blink side, which decides when to apply
// it, is gpu_uniformity.h.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_WEBGL_UNIFORM_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_WEBGL_UNIFORM_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "base/containers/span.h"

namespace blink::molt_fingerprinting::webgl_uniform {

// The GL enums used below, with their values from the Khronos GLES 2 / GLES 3
// headers (gpu_uniformity.cc checks every one against them), so that this
// file needs no GL headers.
namespace gl {
// WebGL 1 and 2.
inline constexpr uint32_t kSubpixelBits = 0x0D50;
inline constexpr uint32_t kMaxTextureSize = 0x0D33;
inline constexpr uint32_t kMaxViewportDims = 0x0D3A;
inline constexpr uint32_t kAliasedPointSizeRange = 0x846D;
inline constexpr uint32_t kAliasedLineWidthRange = 0x846E;
inline constexpr uint32_t kMaxRenderbufferSize = 0x84E8;
inline constexpr uint32_t kMaxCubeMapTextureSize = 0x851C;
inline constexpr uint32_t kMaxVertexAttribs = 0x8869;
inline constexpr uint32_t kMaxTextureImageUnits = 0x8872;
inline constexpr uint32_t kMaxVertexTextureImageUnits = 0x8B4C;
inline constexpr uint32_t kMaxCombinedTextureImageUnits = 0x8B4D;
inline constexpr uint32_t kMaxVertexUniformVectors = 0x8DFB;
inline constexpr uint32_t kMaxVaryingVectors = 0x8DFC;
inline constexpr uint32_t kMaxFragmentUniformVectors = 0x8DFD;
// EXT_texture_filter_anisotropic.
inline constexpr uint32_t kMaxTextureMaxAnisotropy = 0x84FF;
// WebGL 2, and WEBGL_draw_buffers in WebGL 1 (same values).
inline constexpr uint32_t kMaxDrawBuffers = 0x8824;
inline constexpr uint32_t kMaxColorAttachments = 0x8CDF;
// WebGL 2.
inline constexpr uint32_t kMax3DTextureSize = 0x8073;
inline constexpr uint32_t kMaxElementsVertices = 0x80E8;
inline constexpr uint32_t kMaxElementsIndices = 0x80E9;
inline constexpr uint32_t kMaxTextureLodBias = 0x84FD;
inline constexpr uint32_t kMaxArrayTextureLayers = 0x88FF;
inline constexpr uint32_t kMinProgramTexelOffset = 0x8904;
inline constexpr uint32_t kMaxProgramTexelOffset = 0x8905;
inline constexpr uint32_t kMaxVertexUniformBlocks = 0x8A2B;
inline constexpr uint32_t kMaxFragmentUniformBlocks = 0x8A2D;
inline constexpr uint32_t kMaxCombinedUniformBlocks = 0x8A2E;
inline constexpr uint32_t kMaxUniformBufferBindings = 0x8A2F;
inline constexpr uint32_t kMaxUniformBlockSize = 0x8A30;
inline constexpr uint32_t kMaxCombinedVertexUniformComponents = 0x8A31;
inline constexpr uint32_t kMaxCombinedFragmentUniformComponents = 0x8A33;
inline constexpr uint32_t kUniformBufferOffsetAlignment = 0x8A34;
inline constexpr uint32_t kMaxFragmentUniformComponents = 0x8B49;
inline constexpr uint32_t kMaxVertexUniformComponents = 0x8B4A;
inline constexpr uint32_t kMaxVaryingComponents = 0x8B4B;
inline constexpr uint32_t kMaxTransformFeedbackSeparateComponents = 0x8C80;
inline constexpr uint32_t kMaxTransformFeedbackInterleavedComponents = 0x8C8A;
inline constexpr uint32_t kMaxTransformFeedbackSeparateAttribs = 0x8C8B;
inline constexpr uint32_t kMaxSamples = 0x8D57;
inline constexpr uint32_t kMaxElementIndex = 0x8D6B;
inline constexpr uint32_t kMaxServerWaitTimeout = 0x9111;
inline constexpr uint32_t kMaxVertexOutputComponents = 0x9122;
inline constexpr uint32_t kMaxFragmentInputComponents = 0x9125;
// getShaderPrecisionFormat precision types.
inline constexpr uint32_t kLowFloat = 0x8DF0;
inline constexpr uint32_t kMediumFloat = 0x8DF1;
inline constexpr uint32_t kHighFloat = 0x8DF2;
inline constexpr uint32_t kLowInt = 0x8DF3;
inline constexpr uint32_t kMediumInt = 0x8DF4;
inline constexpr uint32_t kHighInt = 0x8DF5;
}  // namespace gl

// Which way a limit is conservative.
enum class Bound {
  // A maximum (sizes, counts): reported as min(real, baseline).
  kMax,
  // A minimum or an alignment: reported as max(real, baseline).
  kMin,
};

struct IntLimit {
  uint32_t pname;
  int64_t baseline;
  Bound bound;
};

struct FloatLimit {
  uint32_t pname;
  float baseline;
};

// ALIASED_POINT_SIZE_RANGE / ALIASED_LINE_WIDTH_RANGE.
struct FloatRange {
  uint32_t pname;
  float low;
  float high;
};

struct PrecisionFormat {
  int32_t range_min;
  int32_t range_max;
  int32_t precision;
  friend bool operator==(const PrecisionFormat&,
                         const PrecisionFormat&) = default;
};

// The baselines (the tables are exposed for tests).
base::span<const IntLimit> IntLimits();
base::span<const FloatLimit> FloatLimits();
base::span<const FloatRange> FloatRanges();
// MAX_VIEWPORT_DIMS, both dimensions.
inline constexpr int32_t kMaxViewportDimsBaseline = 8192;
// The canonical names of the extensions MoltNet pages may see, sorted.
base::span<const std::string_view> AllowedExtensions();

// What a MoltNet page is told for getParameter(`pname`) when the GPU says
// `real`, or nullopt when `pname` is not an implementation-dependent limit
// (state such as VIEWPORT is never changed).
std::optional<int64_t> ClampInt(uint32_t pname, int64_t real);
std::optional<float> ClampFloat(uint32_t pname, float real);
// Array parameters, in place: the ends of ALIASED_POINT_SIZE_RANGE and
// ALIASED_LINE_WIDTH_RANGE become [max(low, baseline low), min(high,
// baseline high)], both MAX_VIEWPORT_DIMS become min(real, baseline). Returns
// whether `pname` is one of them; nothing changes for other parameters, for
// fewer than two values, or when a clamped range would be empty.
bool ClampFloatArray(uint32_t pname, base::span<float> values);
bool ClampIntArray(uint32_t pname, base::span<int32_t> values);

// getShaderPrecisionFormat: the component-wise min of `real` and the
// baseline for `precision_type` (the same for vertex and fragment shaders),
// or nullopt for an unknown precision type.
std::optional<PrecisionFormat> ClampPrecisionFormat(
    uint32_t precision_type,
    const PrecisionFormat& real);

// getInternalformatParameter(RENDERBUFFER, format, SAMPLES): moves the
// sample counts that don't exceed the MAX_SAMPLES baseline to the front of
// `counts`, keeping their order, and returns how many there are.
size_t ClampSampleCounts(base::span<int32_t> counts);

// Whether a MoltNet page may see the extension called `name` (its canonical
// spelling, e.g. "OES_texture_float"; the comparison is exact).
bool IsExtensionAllowed(std::string_view name);

}  // namespace blink::molt_fingerprinting::webgl_uniform

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_WEBGL_UNIFORM_H_
