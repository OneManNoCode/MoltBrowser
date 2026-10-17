// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/gpu_uniformity.h"

#include <atomic>
#include <optional>
#include <string_view>

#include "base/numerics/safe_conversions.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/webgl_uniform.h"

namespace blink::molt_fingerprinting {

namespace gl = webgl_uniform::gl;

// webgl_uniform.h spells out the GL enums it needs; they must be the real
// ones (the GL headers come in through molt_fingerprinting.h).
static_assert(gl::kSubpixelBits == GL_SUBPIXEL_BITS);
static_assert(gl::kMaxTextureSize == GL_MAX_TEXTURE_SIZE);
static_assert(gl::kMaxViewportDims == GL_MAX_VIEWPORT_DIMS);
static_assert(gl::kAliasedPointSizeRange == GL_ALIASED_POINT_SIZE_RANGE);
static_assert(gl::kAliasedLineWidthRange == GL_ALIASED_LINE_WIDTH_RANGE);
static_assert(gl::kMaxRenderbufferSize == GL_MAX_RENDERBUFFER_SIZE);
static_assert(gl::kMaxCubeMapTextureSize == GL_MAX_CUBE_MAP_TEXTURE_SIZE);
static_assert(gl::kMaxVertexAttribs == GL_MAX_VERTEX_ATTRIBS);
static_assert(gl::kMaxTextureImageUnits == GL_MAX_TEXTURE_IMAGE_UNITS);
static_assert(gl::kMaxVertexTextureImageUnits ==
              GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS);
static_assert(gl::kMaxCombinedTextureImageUnits ==
              GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
static_assert(gl::kMaxVertexUniformVectors == GL_MAX_VERTEX_UNIFORM_VECTORS);
static_assert(gl::kMaxVaryingVectors == GL_MAX_VARYING_VECTORS);
static_assert(gl::kMaxFragmentUniformVectors ==
              GL_MAX_FRAGMENT_UNIFORM_VECTORS);
static_assert(gl::kMaxTextureMaxAnisotropy ==
              GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT);
static_assert(gl::kMaxDrawBuffers == GL_MAX_DRAW_BUFFERS);
static_assert(gl::kMaxDrawBuffers == GL_MAX_DRAW_BUFFERS_EXT);
static_assert(gl::kMaxColorAttachments == GL_MAX_COLOR_ATTACHMENTS);
static_assert(gl::kMaxColorAttachments == GL_MAX_COLOR_ATTACHMENTS_EXT);
static_assert(gl::kMax3DTextureSize == GL_MAX_3D_TEXTURE_SIZE);
static_assert(gl::kMaxElementsVertices == GL_MAX_ELEMENTS_VERTICES);
static_assert(gl::kMaxElementsIndices == GL_MAX_ELEMENTS_INDICES);
static_assert(gl::kMaxTextureLodBias == GL_MAX_TEXTURE_LOD_BIAS);
static_assert(gl::kMaxArrayTextureLayers == GL_MAX_ARRAY_TEXTURE_LAYERS);
static_assert(gl::kMinProgramTexelOffset == GL_MIN_PROGRAM_TEXEL_OFFSET);
static_assert(gl::kMaxProgramTexelOffset == GL_MAX_PROGRAM_TEXEL_OFFSET);
static_assert(gl::kMaxVertexUniformBlocks == GL_MAX_VERTEX_UNIFORM_BLOCKS);
static_assert(gl::kMaxFragmentUniformBlocks ==
              GL_MAX_FRAGMENT_UNIFORM_BLOCKS);
static_assert(gl::kMaxCombinedUniformBlocks ==
              GL_MAX_COMBINED_UNIFORM_BLOCKS);
static_assert(gl::kMaxUniformBufferBindings ==
              GL_MAX_UNIFORM_BUFFER_BINDINGS);
static_assert(gl::kMaxUniformBlockSize == GL_MAX_UNIFORM_BLOCK_SIZE);
static_assert(gl::kMaxCombinedVertexUniformComponents ==
              GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS);
static_assert(gl::kMaxCombinedFragmentUniformComponents ==
              GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS);
static_assert(gl::kUniformBufferOffsetAlignment ==
              GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT);
static_assert(gl::kMaxFragmentUniformComponents ==
              GL_MAX_FRAGMENT_UNIFORM_COMPONENTS);
static_assert(gl::kMaxVertexUniformComponents ==
              GL_MAX_VERTEX_UNIFORM_COMPONENTS);
static_assert(gl::kMaxVaryingComponents == GL_MAX_VARYING_COMPONENTS);
static_assert(gl::kMaxTransformFeedbackSeparateComponents ==
              GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS);
static_assert(gl::kMaxTransformFeedbackInterleavedComponents ==
              GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS);
static_assert(gl::kMaxTransformFeedbackSeparateAttribs ==
              GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS);
static_assert(gl::kMaxSamples == GL_MAX_SAMPLES);
static_assert(gl::kMaxElementIndex == GL_MAX_ELEMENT_INDEX);
static_assert(gl::kMaxServerWaitTimeout == GL_MAX_SERVER_WAIT_TIMEOUT);
static_assert(gl::kMaxVertexOutputComponents ==
              GL_MAX_VERTEX_OUTPUT_COMPONENTS);
static_assert(gl::kMaxFragmentInputComponents ==
              GL_MAX_FRAGMENT_INPUT_COMPONENTS);
static_assert(gl::kLowFloat == GL_LOW_FLOAT);
static_assert(gl::kMediumFloat == GL_MEDIUM_FLOAT);
static_assert(gl::kHighFloat == GL_HIGH_FLOAT);
static_assert(gl::kLowInt == GL_LOW_INT);
static_assert(gl::kMediumInt == GL_MEDIUM_INT);
static_assert(gl::kHighInt == GL_HIGH_INT);

namespace {

std::atomic<bool> g_uniform_gpu_for_testing{false};

bool UniformMode() {
  return IsUniform() ||
         g_uniform_gpu_for_testing.load(std::memory_order_relaxed);
}

}  // namespace

bool UsesUniformGpu(ExecutionContext* context) {
  if (!UniformMode()) {
    return false;
  }
  // IsFarbled() is false exactly for contexts whose top-level site isn't
  // web content (in kUniform, Shields-off sites are farbled too).
  return !context || IsFarbled(context);
}

// Each function below checks the (cheap) table first and resolves the
// context only for the parameters it would change.

int32_t UniformWebGLInt(ExecutionContext* context,
                        uint32_t pname,
                        int32_t real) {
  if (!UniformMode()) {
    return real;
  }
  const std::optional<int64_t> shown = webgl_uniform::ClampInt(pname, real);
  if (!shown || !UsesUniformGpu(context)) {
    return real;
  }
  // Between `real` and a baseline that fits, so it fits too.
  return base::saturated_cast<int32_t>(*shown);
}

int64_t UniformWebGLInt64(ExecutionContext* context,
                          uint32_t pname,
                          int64_t real) {
  if (!UniformMode()) {
    return real;
  }
  const std::optional<int64_t> shown = webgl_uniform::ClampInt(pname, real);
  if (!shown || !UsesUniformGpu(context)) {
    return real;
  }
  return *shown;
}

float UniformWebGLFloat(ExecutionContext* context, uint32_t pname, float real) {
  if (!UniformMode()) {
    return real;
  }
  const std::optional<float> shown = webgl_uniform::ClampFloat(pname, real);
  if (!shown || !UsesUniformGpu(context)) {
    return real;
  }
  return *shown;
}

void UniformWebGLFloats(ExecutionContext* context,
                        uint32_t pname,
                        base::span<float> values) {
  if (!UniformMode() ||
      (pname != gl::kAliasedPointSizeRange &&
       pname != gl::kAliasedLineWidthRange) ||
      !UsesUniformGpu(context)) {
    return;
  }
  webgl_uniform::ClampFloatArray(pname, values);
}

void UniformWebGLInts(ExecutionContext* context,
                      uint32_t pname,
                      base::span<int32_t> values) {
  if (!UniformMode() || pname != gl::kMaxViewportDims ||
      !UsesUniformGpu(context)) {
    return;
  }
  webgl_uniform::ClampIntArray(pname, values);
}

ShaderPrecisionFormat UniformShaderPrecisionFormat(
    ExecutionContext* context,
    uint32_t precision_type,
    const ShaderPrecisionFormat& real) {
  if (!UniformMode() || !UsesUniformGpu(context)) {
    return real;
  }
  const std::optional<webgl_uniform::PrecisionFormat> shown =
      webgl_uniform::ClampPrecisionFormat(
          precision_type, {real.range_min, real.range_max, real.precision});
  if (!shown) {
    return real;
  }
  return {shown->range_min, shown->range_max, shown->precision};
}

size_t UniformSampleCounts(ExecutionContext* context,
                           base::span<int32_t> counts) {
  if (!UniformMode() || !UsesUniformGpu(context)) {
    return counts.size();
  }
  return webgl_uniform::ClampSampleCounts(counts);
}

bool WebGLExtensionAllowed(ExecutionContext* context, const char* name) {
  if (!UniformMode()) {
    return true;
  }
  if (name && webgl_uniform::IsExtensionAllowed(std::string_view(name))) {
    return true;
  }
  return !UsesUniformGpu(context);
}

bool HidesWebGPUAdapter(ExecutionContext* context) {
  return UsesUniformGpu(context);
}

void SetUniformGpuForTesting(bool uniform) {
  g_uniform_gpu_for_testing.store(uniform, std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
