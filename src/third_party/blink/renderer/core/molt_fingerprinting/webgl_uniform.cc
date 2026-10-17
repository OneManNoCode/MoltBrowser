// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/webgl_uniform.h"

#include <algorithm>
#include <array>

namespace blink::molt_fingerprinting::webgl_uniform {

namespace {

// How the baselines were picked (docs/FINGERPRINTING.md has the full
// reasoning): every one is at least the WebGL 2 (OpenGL ES 3.0) minimum, so
// a clamped report is still a valid one, and at most what every desktop GPU
// that runs WebGL 2 in current Chromium reports through ANGLE (D3D11 feature
// level 10_0 and up, every Metal Mac, current Mesa and NVIDIA drivers), so
// that nearly everybody reports the baseline itself. Where apps size their
// work from a limit (texture size, uniforms, varyings, texture units,
// attributes, draw buffers), the baseline is the realistic desktop floor,
// well above the spec minimum; where they don't (hints, LOD bias, server
// wait timeout), it is the spec minimum or a deliberately low value.
constexpr int32_t kMaxSamplesBaseline = 4;
constexpr int64_t kMaxUniformBlockSizeBaseline = 16384;
constexpr int64_t kMaxVertexUniformBlocksBaseline = 12;
constexpr int64_t kMaxFragmentUniformBlocksBaseline = 12;
constexpr int64_t kMaxUniformVectorsBaseline = 1024;

constexpr auto kIntLimits = std::to_array<IntLimit>({
    // Sizes: ~95% of WebGL 2 devices and every desktop one have 8192
    // (D3D10-class GPUs stop there; D3D11, Metal and Mesa have 16384, which
    // only ~80% have). 4096 would halve the largest texture (images over
    // 4K); 16384 would single out older and mobile GPUs.
    {gl::kMaxTextureSize, 8192, Bound::kMax},
    {gl::kMaxCubeMapTextureSize, 8192, Bound::kMax},
    {gl::kMaxRenderbufferSize, 8192, Bound::kMax},
    // ANGLE caps vertex attributes at 16.
    {gl::kMaxVertexAttribs, 16, Bound::kMax},
    // The desktop backends report 1024 (Metal) or more; the spec minimums
    // (128/16 in WebGL 1, 256/224 in WebGL 2) push engines onto slower
    // paths (e.g. fewer skinning bones in uniforms).
    {gl::kMaxVertexUniformVectors, kMaxUniformVectorsBaseline, Bound::kMax},
    {gl::kMaxFragmentUniformVectors, kMaxUniformVectorsBaseline, Bound::kMax},
    {gl::kMaxVertexUniformComponents, 4 * kMaxUniformVectorsBaseline,
     Bound::kMax},
    {gl::kMaxFragmentUniformComponents, 4 * kMaxUniformVectorsBaseline,
     Bound::kMax},
    // 30 vec4 varyings: ~95% of WebGL 2 devices (all desktop ones) have at
    // least 30, about a third more; the minimum (15) is tight for big
    // material shaders.
    {gl::kMaxVaryingVectors, 30, Bound::kMax},
    {gl::kMaxVaryingComponents, 4 * 30, Bound::kMax},
    {gl::kMaxVertexOutputComponents, 4 * 30, Bound::kMax},
    {gl::kMaxFragmentInputComponents, 4 * 30, Bound::kMax},
    // Texture units: the WebGL 2 minimums are what almost every GPU has.
    {gl::kMaxTextureImageUnits, 16, Bound::kMax},
    {gl::kMaxVertexTextureImageUnits, 16, Bound::kMax},
    {gl::kMaxCombinedTextureImageUnits, 32, Bound::kMax},
    {gl::kSubpixelBits, 4, Bound::kMax},
    // MRT: 8 on every desktop GPU (4 on some mobile ones).
    {gl::kMaxDrawBuffers, 8, Bound::kMax},
    {gl::kMaxColorAttachments, 8, Bound::kMax},
    // WebGL 2. 3D textures and array layers: 2048 on D3D11 and Metal.
    {gl::kMax3DTextureSize, 2048, Bound::kMax},
    {gl::kMaxArrayTextureLayers, 2048, Bound::kMax},
    {gl::kMaxSamples, kMaxSamplesBaseline, Bound::kMax},
    // Uniform blocks: the spec minimums, which is also what a fifth of
    // Chromium's WebGL 2 users report for the block size.
    {gl::kMaxVertexUniformBlocks, kMaxVertexUniformBlocksBaseline,
     Bound::kMax},
    {gl::kMaxFragmentUniformBlocks, kMaxFragmentUniformBlocksBaseline,
     Bound::kMax},
    {gl::kMaxCombinedUniformBlocks, 24, Bound::kMax},
    {gl::kMaxUniformBufferBindings, 24, Bound::kMax},
    {gl::kMaxUniformBlockSize, kMaxUniformBlockSizeBaseline, Bound::kMax},
    // The spec defines these from the values above.
    {gl::kMaxCombinedVertexUniformComponents,
     kMaxVertexUniformBlocksBaseline * kMaxUniformBlockSizeBaseline / 4 +
         4 * kMaxUniformVectorsBaseline,
     Bound::kMax},
    {gl::kMaxCombinedFragmentUniformComponents,
     kMaxFragmentUniformBlocksBaseline * kMaxUniformBlockSizeBaseline / 4 +
         4 * kMaxUniformVectorsBaseline,
     Bound::kMax},
    // The spec's bound (an alignment of at most 256): any real alignment
    // divides it, so buffers laid out for it work everywhere.
    {gl::kUniformBufferOffsetAlignment, 256, Bound::kMin},
    {gl::kMinProgramTexelOffset, -8, Bound::kMin},
    {gl::kMaxProgramTexelOffset, 7, Bound::kMax},
    {gl::kMaxTransformFeedbackInterleavedComponents, 64, Bound::kMax},
    {gl::kMaxTransformFeedbackSeparateAttribs, 4, Bound::kMax},
    {gl::kMaxTransformFeedbackSeparateComponents, 4, Bound::kMax},
    // 32-bit indices on every desktop GPU.
    {gl::kMaxElementIndex, 4294967295LL, Bound::kMax},
    // Performance hints with no minimum (backends report anything from a
    // few thousand to INT_MAX) that WebGL never enforces: a low value costs
    // nothing.
    {gl::kMaxElementsVertices, 2048, Bound::kMax},
    {gl::kMaxElementsIndices, 2048, Bound::kMax},
    // WebGL 2 only allows waitSync() with TIMEOUT_IGNORED anyway.
    {gl::kMaxServerWaitTimeout, 0, Bound::kMax},
});

constexpr auto kFloatLimits = std::to_array<FloatLimit>({
    // 16x anisotropic filtering is universal on desktop GPUs.
    {gl::kMaxTextureMaxAnisotropy, 16.0f},
    // Only bounds a shader's texture LOD bias; the spec minimum.
    {gl::kMaxTextureLodBias, 2.0f},
});

constexpr auto kFloatRanges = std::to_array<FloatRange>({
    // Points up to 255 px: about Intel's limit, the lowest on desktop
    // (Metal and D3D11 allow more).
    {gl::kAliasedPointSizeRange, 1.0f, 255.0f},
    // D3D11, Metal and Vulkan only draw 1-pixel lines.
    {gl::kAliasedLineWidthRange, 1.0f, 1.0f},
});

struct PrecisionBaseline {
  uint32_t precision_type;
  PrecisionFormat format;
};

// highp is IEEE single precision and 32-bit integers everywhere. lowp and
// mediump are given as half floats and 16-bit integers, what GPUs with
// native low precision (mobile, Apple) run them at; desktop GPUs, which run
// every precision at 32 bits, are clamped down to the same report.
constexpr auto kPrecisionBaselines = std::to_array<PrecisionBaseline>({
    {gl::kLowFloat, {15, 15, 10}},
    {gl::kMediumFloat, {15, 15, 10}},
    {gl::kHighFloat, {127, 127, 23}},
    {gl::kLowInt, {15, 14, 0}},
    {gl::kMediumInt, {15, 14, 0}},
    {gl::kHighInt, {31, 30, 0}},
});

// Common extensions every desktop GPU has in Chromium. Left out: anything
// that differs between GPUs of one platform (EXT_float_blend: not on Apple
// GPUs; BPTC/RGTC/ASTC/ETC/PVRTC texture compression), GPU timers
// (EXT_disjoint_timer_query*), the unmasked GPU strings
// (WEBGL_debug_renderer_info) and translated shader source, which shows the
// backend and its driver workarounds (WEBGL_debug_shaders), plus the newer
// extensions few sites use. ASCII order.
constexpr auto kAllowedExtensions = std::to_array<std::string_view>({
    "ANGLE_instanced_arrays",
    "EXT_blend_minmax",
    "EXT_color_buffer_float",
    "EXT_color_buffer_half_float",
    "EXT_frag_depth",
    "EXT_sRGB",
    "EXT_shader_texture_lod",
    "EXT_texture_filter_anisotropic",
    "KHR_parallel_shader_compile",
    "OES_element_index_uint",
    "OES_standard_derivatives",
    "OES_texture_float",
    "OES_texture_float_linear",
    "OES_texture_half_float",
    "OES_texture_half_float_linear",
    "OES_vertex_array_object",
    "WEBGL_color_buffer_float",
    "WEBGL_compressed_texture_s3tc",
    "WEBGL_compressed_texture_s3tc_srgb",
    "WEBGL_depth_texture",
    "WEBGL_draw_buffers",
    "WEBGL_lose_context",
    "WEBGL_multi_draw",
});

}  // namespace

base::span<const IntLimit> IntLimits() {
  return kIntLimits;
}

base::span<const FloatLimit> FloatLimits() {
  return kFloatLimits;
}

base::span<const FloatRange> FloatRanges() {
  return kFloatRanges;
}

base::span<const std::string_view> AllowedExtensions() {
  return kAllowedExtensions;
}

std::optional<int64_t> ClampInt(uint32_t pname, int64_t real) {
  for (const IntLimit& limit : kIntLimits) {
    if (limit.pname == pname) {
      return limit.bound == Bound::kMax ? std::min(real, limit.baseline)
                                        : std::max(real, limit.baseline);
    }
  }
  return std::nullopt;
}

std::optional<float> ClampFloat(uint32_t pname, float real) {
  for (const FloatLimit& limit : kFloatLimits) {
    if (limit.pname == pname) {
      // A NaN (never seen from a GPU) reports the baseline.
      return real < limit.baseline ? real : limit.baseline;
    }
  }
  return std::nullopt;
}

bool ClampFloatArray(uint32_t pname, base::span<float> values) {
  for (const FloatRange& range : kFloatRanges) {
    if (range.pname != pname) {
      continue;
    }
    if (values.size() >= 2) {
      const float low = values[0] > range.low ? values[0] : range.low;
      const float high = values[1] < range.high ? values[1] : range.high;
      // An empty range (e.g. the zeros of a lost context) stays as it was.
      if (low <= high) {
        values[0] = low;
        values[1] = high;
      }
    }
    return true;
  }
  return false;
}

bool ClampIntArray(uint32_t pname, base::span<int32_t> values) {
  if (pname != gl::kMaxViewportDims) {
    return false;
  }
  if (values.size() >= 2) {
    values[0] = std::min(values[0], kMaxViewportDimsBaseline);
    values[1] = std::min(values[1], kMaxViewportDimsBaseline);
  }
  return true;
}

std::optional<PrecisionFormat> ClampPrecisionFormat(
    uint32_t precision_type,
    const PrecisionFormat& real) {
  for (const PrecisionBaseline& baseline : kPrecisionBaselines) {
    if (baseline.precision_type == precision_type) {
      return PrecisionFormat{
          std::min(real.range_min, baseline.format.range_min),
          std::min(real.range_max, baseline.format.range_max),
          std::min(real.precision, baseline.format.precision)};
    }
  }
  return std::nullopt;
}

size_t ClampSampleCounts(base::span<int32_t> counts) {
  size_t kept = 0;
  for (size_t i = 0; i < counts.size(); ++i) {
    if (counts[i] <= kMaxSamplesBaseline) {
      counts[kept++] = counts[i];
    }
  }
  return kept;
}

bool IsExtensionAllowed(std::string_view name) {
  return std::find(kAllowedExtensions.begin(), kAllowedExtensions.end(),
                   name) != kAllowedExtensions.end();
}

}  // namespace blink::molt_fingerprinting::webgl_uniform
