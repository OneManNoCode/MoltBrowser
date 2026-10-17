// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/webgl_uniform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting::webgl_uniform {
namespace {

// Parameters that are state, not limits (never changed).
constexpr uint32_t kViewport = 0x0BA2;
constexpr uint32_t kScissorBox = 0x0C10;
constexpr uint32_t kPackAlignment = 0x0D05;
constexpr uint32_t kDepthRange = 0x0B70;
constexpr uint32_t kSamples = 0x80A9;

struct SpecMinimum {
  uint32_t pname;
  // The OpenGL ES 3.0 / WebGL 2 requirement: the smallest allowed value of
  // a maximum, or the largest allowed value of a minimum or an alignment.
  int64_t value;
};

// OpenGL ES 3.0.6 tables 6.31-6.36 (WebGL 2 adds nothing stricter here).
constexpr auto kSpecMinimums = std::to_array<SpecMinimum>({
    {gl::kMaxTextureSize, 2048},
    {gl::kMaxCubeMapTextureSize, 2048},
    {gl::kMaxRenderbufferSize, 2048},
    {gl::kMaxVertexAttribs, 16},
    {gl::kMaxVertexUniformVectors, 256},
    {gl::kMaxFragmentUniformVectors, 224},
    {gl::kMaxVertexUniformComponents, 1024},
    {gl::kMaxFragmentUniformComponents, 896},
    {gl::kMaxVaryingVectors, 15},
    {gl::kMaxVaryingComponents, 60},
    {gl::kMaxVertexOutputComponents, 64},
    {gl::kMaxFragmentInputComponents, 60},
    {gl::kMaxTextureImageUnits, 16},
    {gl::kMaxVertexTextureImageUnits, 16},
    {gl::kMaxCombinedTextureImageUnits, 32},
    {gl::kSubpixelBits, 4},
    {gl::kMaxDrawBuffers, 4},
    {gl::kMaxColorAttachments, 4},
    {gl::kMax3DTextureSize, 256},
    {gl::kMaxArrayTextureLayers, 256},
    {gl::kMaxSamples, 4},
    {gl::kMaxVertexUniformBlocks, 12},
    {gl::kMaxFragmentUniformBlocks, 12},
    {gl::kMaxCombinedUniformBlocks, 24},
    {gl::kMaxUniformBufferBindings, 24},
    {gl::kMaxUniformBlockSize, 16384},
    {gl::kMaxCombinedVertexUniformComponents, 12 * 16384 / 4 + 1024},
    {gl::kMaxCombinedFragmentUniformComponents, 12 * 16384 / 4 + 896},
    {gl::kUniformBufferOffsetAlignment, 256},
    {gl::kMinProgramTexelOffset, -8},
    {gl::kMaxProgramTexelOffset, 7},
    {gl::kMaxTransformFeedbackInterleavedComponents, 64},
    {gl::kMaxTransformFeedbackSeparateAttribs, 4},
    {gl::kMaxTransformFeedbackSeparateComponents, 4},
    {gl::kMaxElementIndex, (int64_t{1} << 24) - 1},
    {gl::kMaxElementsVertices, 0},
    {gl::kMaxElementsIndices, 0},
    {gl::kMaxServerWaitTimeout, 0},
});

int64_t Baseline(uint32_t pname) {
  for (const IntLimit& limit : IntLimits()) {
    if (limit.pname == pname) {
      return limit.baseline;
    }
  }
  ADD_FAILURE() << "no baseline for 0x" << std::hex << pname;
  return 0;
}

TEST(MoltWebGLUniformTest, EveryIntLimitHasASpecMinimumAndMeetsIt) {
  EXPECT_EQ(IntLimits().size(), kSpecMinimums.size());
  for (const IntLimit& limit : IntLimits()) {
    const auto it = std::find_if(
        kSpecMinimums.begin(), kSpecMinimums.end(),
        [&](const SpecMinimum& spec) { return spec.pname == limit.pname; });
    ASSERT_NE(it, kSpecMinimums.end()) << std::hex << limit.pname;
    if (limit.bound == Bound::kMax) {
      EXPECT_GE(limit.baseline, it->value) << std::hex << limit.pname;
    } else {
      EXPECT_LE(limit.baseline, it->value) << std::hex << limit.pname;
    }
  }
}

TEST(MoltWebGLUniformTest, OnlyAlignmentAndNegativeOffsetAreLowerBounds) {
  for (const IntLimit& limit : IntLimits()) {
    const bool lower = limit.pname == gl::kUniformBufferOffsetAlignment ||
                       limit.pname == gl::kMinProgramTexelOffset;
    EXPECT_EQ(limit.bound, lower ? Bound::kMin : Bound::kMax)
        << std::hex << limit.pname;
  }
}

TEST(MoltWebGLUniformTest, NoParameterIsListedTwice) {
  std::set<uint32_t> seen;
  for (const IntLimit& limit : IntLimits()) {
    EXPECT_TRUE(seen.insert(limit.pname).second) << std::hex << limit.pname;
  }
  for (const FloatLimit& limit : FloatLimits()) {
    EXPECT_TRUE(seen.insert(limit.pname).second) << std::hex << limit.pname;
  }
  for (const FloatRange& range : FloatRanges()) {
    EXPECT_TRUE(seen.insert(range.pname).second) << std::hex << range.pname;
  }
  EXPECT_TRUE(seen.insert(gl::kMaxViewportDims).second);
}

TEST(MoltWebGLUniformTest, BaselinesAreConsistentWithEachOther) {
  // Vectors and components describe the same storage.
  EXPECT_EQ(Baseline(gl::kMaxVertexUniformComponents),
            4 * Baseline(gl::kMaxVertexUniformVectors));
  EXPECT_EQ(Baseline(gl::kMaxFragmentUniformComponents),
            4 * Baseline(gl::kMaxFragmentUniformVectors));
  EXPECT_EQ(Baseline(gl::kMaxVaryingComponents),
            4 * Baseline(gl::kMaxVaryingVectors));
  // ES 3.0 defines the combined counts from the others.
  EXPECT_EQ(Baseline(gl::kMaxCombinedVertexUniformComponents),
            Baseline(gl::kMaxVertexUniformBlocks) *
                    Baseline(gl::kMaxUniformBlockSize) / 4 +
                Baseline(gl::kMaxVertexUniformComponents));
  EXPECT_EQ(Baseline(gl::kMaxCombinedFragmentUniformComponents),
            Baseline(gl::kMaxFragmentUniformBlocks) *
                    Baseline(gl::kMaxUniformBlockSize) / 4 +
                Baseline(gl::kMaxFragmentUniformComponents));
  EXPECT_GE(Baseline(gl::kMaxCombinedUniformBlocks),
            Baseline(gl::kMaxVertexUniformBlocks) +
                Baseline(gl::kMaxFragmentUniformBlocks));
  EXPECT_GE(Baseline(gl::kMaxCombinedTextureImageUnits),
            Baseline(gl::kMaxTextureImageUnits) +
                Baseline(gl::kMaxVertexTextureImageUnits));
  EXPECT_GE(Baseline(gl::kMaxColorAttachments),
            Baseline(gl::kMaxDrawBuffers));
  // The viewport must cover the largest renderbuffer (ES 3.0 2.12.1).
  EXPECT_GE(kMaxViewportDimsBaseline, Baseline(gl::kMaxRenderbufferSize));
  EXPECT_EQ(Baseline(gl::kMaxCubeMapTextureSize),
            Baseline(gl::kMaxTextureSize));
}

TEST(MoltWebGLUniformTest, ClampIntNeverClaimsMoreThanTheGpu) {
  const std::vector<int64_t> reals = {-64, -32, -8, -1, 0,   1,     4,
                                      7,   15,  16, 30, 31, 256,   1024,
                                      2048, 4096, 8192, 16384, 32768,
                                      int64_t{1} << 32};
  for (const IntLimit& limit : IntLimits()) {
    for (int64_t real : reals) {
      const std::optional<int64_t> shown = ClampInt(limit.pname, real);
      ASSERT_TRUE(shown.has_value());
      if (limit.bound == Bound::kMax) {
        // Never more than the GPU has; the baseline whenever it has that.
        EXPECT_LE(*shown, real);
        EXPECT_EQ(*shown, real >= limit.baseline ? limit.baseline : real);
      } else {
        EXPECT_GE(*shown, real);
        EXPECT_EQ(*shown, real <= limit.baseline ? limit.baseline : real);
      }
    }
  }
}

TEST(MoltWebGLUniformTest, ClampIntExamples) {
  EXPECT_EQ(ClampInt(gl::kMaxTextureSize, 16384), 8192);
  EXPECT_EQ(ClampInt(gl::kMaxTextureSize, 8192), 8192);
  EXPECT_EQ(ClampInt(gl::kMaxTextureSize, 4096), 4096);
  EXPECT_EQ(ClampInt(gl::kMaxVertexUniformVectors, 4095), 1024);
  EXPECT_EQ(ClampInt(gl::kMaxVaryingVectors, 31), 30);
  EXPECT_EQ(ClampInt(gl::kMaxSamples, 8), 4);
  EXPECT_EQ(ClampInt(gl::kMaxUniformBlockSize, 65536), 16384);
  EXPECT_EQ(ClampInt(gl::kMaxElementIndex, 4294967295LL), 4294967295LL);
  EXPECT_EQ(ClampInt(gl::kMaxServerWaitTimeout, 1'000'000'000), 0);
  // Lower bounds go the other way.
  EXPECT_EQ(ClampInt(gl::kUniformBufferOffsetAlignment, 4), 256);
  EXPECT_EQ(ClampInt(gl::kUniformBufferOffsetAlignment, 256), 256);
  EXPECT_EQ(ClampInt(gl::kMinProgramTexelOffset, -32), -8);
  EXPECT_EQ(ClampInt(gl::kMaxProgramTexelOffset, 31), 7);
}

TEST(MoltWebGLUniformTest, StateIsNotALimit) {
  for (uint32_t pname : {kViewport, kScissorBox, kPackAlignment, kDepthRange,
                         kSamples, gl::kMaxViewportDims,
                         gl::kAliasedPointSizeRange, gl::kHighFloat}) {
    EXPECT_FALSE(ClampInt(pname, 5).has_value()) << std::hex << pname;
    EXPECT_FALSE(ClampFloat(pname, 5.0f).has_value()) << std::hex << pname;
  }
}

// Three plausible GPUs above every baseline (D3D11-, Metal- and GL-like
// numbers) look exactly the same.
TEST(MoltWebGLUniformTest, DifferentGpusReportTheSameLimits) {
  struct Gpu {
    int64_t big;     // texture sizes
    int64_t middle;  // uniform vectors
    int64_t small;   // varyings, units, buffers
    int64_t alignment;
    int64_t texel_offset;
  };
  const std::array<Gpu, 3> gpus = {{{16384, 4096, 32, 256, -8},
                                    {16384, 1024, 31, 16, -8},
                                    {32768, 4096, 64, 4, -32}}};
  for (const IntLimit& limit : IntLimits()) {
    std::set<int64_t> reports;
    for (const Gpu& gpu : gpus) {
      int64_t real;
      if (limit.pname == gl::kUniformBufferOffsetAlignment) {
        real = gpu.alignment;
      } else if (limit.pname == gl::kMinProgramTexelOffset) {
        real = gpu.texel_offset;
      } else {
        real = std::max({limit.baseline, gpu.small,
                         limit.baseline >= 1024 ? gpu.middle : 0,
                         limit.baseline >= 8192 ? gpu.big : 0});
      }
      reports.insert(*ClampInt(limit.pname, real));
    }
    EXPECT_EQ(reports.size(), 1u) << std::hex << limit.pname;
    EXPECT_EQ(*reports.begin(), limit.baseline) << std::hex << limit.pname;
  }
}

TEST(MoltWebGLUniformTest, ClampFloat) {
  EXPECT_EQ(ClampFloat(gl::kMaxTextureMaxAnisotropy, 16.0f), 16.0f);
  EXPECT_EQ(ClampFloat(gl::kMaxTextureMaxAnisotropy, 32.0f), 16.0f);
  EXPECT_EQ(ClampFloat(gl::kMaxTextureMaxAnisotropy, 8.0f), 8.0f);
  EXPECT_EQ(ClampFloat(gl::kMaxTextureLodBias, 15.99f), 2.0f);
  EXPECT_EQ(ClampFloat(gl::kMaxTextureLodBias, 16.0f), 2.0f);
  EXPECT_EQ(ClampFloat(gl::kMaxTextureMaxAnisotropy,
                       std::numeric_limits<float>::quiet_NaN()),
            16.0f);
  // Both float baselines meet their minimums (EXT_texture_filter_anisotropic
  // requires 2, ES 3.0 a LOD bias of 2).
  for (const FloatLimit& limit : FloatLimits()) {
    EXPECT_GE(limit.baseline, 2.0f);
  }
}

TEST(MoltWebGLUniformTest, ClampFloatArray) {
  std::array<float, 4> point = {1.0f, 1024.0f, 9.0f, 9.0f};
  EXPECT_TRUE(ClampFloatArray(gl::kAliasedPointSizeRange, point));
  EXPECT_EQ(point, (std::array<float, 4>{1.0f, 255.0f, 9.0f, 9.0f}));

  // A GPU allowing smaller and fewer: low end raised, high end kept.
  point = {0.125f, 64.0f, 0, 0};
  EXPECT_TRUE(ClampFloatArray(gl::kAliasedPointSizeRange, point));
  EXPECT_EQ(point[0], 1.0f);
  EXPECT_EQ(point[1], 64.0f);

  std::array<float, 4> line = {1.0f, 7.375f, 0, 0};
  EXPECT_TRUE(ClampFloatArray(gl::kAliasedLineWidthRange, line));
  EXPECT_EQ(line[0], 1.0f);
  EXPECT_EQ(line[1], 1.0f);

  // A lost context reads zeros: an empty range stays as it was.
  std::array<float, 4> lost = {0, 0, 0, 0};
  EXPECT_TRUE(ClampFloatArray(gl::kAliasedPointSizeRange, lost));
  EXPECT_EQ(lost, (std::array<float, 4>{0, 0, 0, 0}));

  // Too short: untouched.
  std::array<float, 1> one = {500.0f};
  EXPECT_TRUE(ClampFloatArray(gl::kAliasedPointSizeRange, one));
  EXPECT_EQ(one[0], 500.0f);

  // State arrays are not ranges.
  std::array<float, 4> depth = {0.0f, 1.0f, 0, 0};
  EXPECT_FALSE(ClampFloatArray(kDepthRange, depth));
  EXPECT_EQ(depth, (std::array<float, 4>{0.0f, 1.0f, 0, 0}));

  for (const FloatRange& range : FloatRanges()) {
    EXPECT_LE(range.low, 1.0f);
    EXPECT_GE(range.high, 1.0f);
  }
}

TEST(MoltWebGLUniformTest, ClampIntArray) {
  std::array<int32_t, 4> dims = {32767, 32767, 0, 0};
  EXPECT_TRUE(ClampIntArray(gl::kMaxViewportDims, dims));
  EXPECT_EQ(dims, (std::array<int32_t, 4>{8192, 8192, 0, 0}));

  dims = {4096, 16384, 0, 0};
  EXPECT_TRUE(ClampIntArray(gl::kMaxViewportDims, dims));
  EXPECT_EQ(dims[0], 4096);
  EXPECT_EQ(dims[1], 8192);

  std::array<int32_t, 4> viewport = {0, 0, 30000, 30000};
  EXPECT_FALSE(ClampIntArray(kViewport, viewport));
  EXPECT_FALSE(ClampIntArray(kScissorBox, viewport));
  EXPECT_EQ(viewport, (std::array<int32_t, 4>{0, 0, 30000, 30000}));

  std::array<int32_t, 1> one = {30000};
  EXPECT_TRUE(ClampIntArray(gl::kMaxViewportDims, one));
  EXPECT_EQ(one[0], 30000);
}

TEST(MoltWebGLUniformTest, PrecisionFormats) {
  const PrecisionFormat fp32 = {127, 127, 23};
  const PrecisionFormat fp16 = {15, 15, 10};
  const PrecisionFormat int32 = {31, 30, 0};
  const PrecisionFormat int16 = {15, 14, 0};

  // A desktop GPU reports 32 bits for every precision...
  EXPECT_EQ(ClampPrecisionFormat(gl::kLowFloat, fp32), fp16);
  EXPECT_EQ(ClampPrecisionFormat(gl::kMediumFloat, fp32), fp16);
  EXPECT_EQ(ClampPrecisionFormat(gl::kHighFloat, fp32), fp32);
  EXPECT_EQ(ClampPrecisionFormat(gl::kLowInt, int32), int16);
  EXPECT_EQ(ClampPrecisionFormat(gl::kMediumInt, int32), int16);
  EXPECT_EQ(ClampPrecisionFormat(gl::kHighInt, int32), int32);
  // ... a GPU with native half floats the same.
  EXPECT_EQ(ClampPrecisionFormat(gl::kMediumFloat, fp16), fp16);
  EXPECT_EQ(ClampPrecisionFormat(gl::kMediumInt, int16), int16);

  // Lower real precision is never raised.
  const PrecisionFormat tiny = {1, 1, 8};
  EXPECT_EQ(ClampPrecisionFormat(gl::kLowFloat, tiny), tiny);
  const PrecisionFormat odd = {62, 200, 16};
  EXPECT_EQ(ClampPrecisionFormat(gl::kHighFloat, odd),
            (PrecisionFormat{62, 127, 16}));

  EXPECT_FALSE(ClampPrecisionFormat(0x1406 /* GL_FLOAT */, fp32).has_value());
}

TEST(MoltWebGLUniformTest, SampleCounts) {
  std::array<int32_t, 4> counts = {16, 8, 4, 2};
  ASSERT_EQ(ClampSampleCounts(counts), 2u);
  EXPECT_EQ(counts[0], 4);
  EXPECT_EQ(counts[1], 2);

  std::array<int32_t, 2> metal = {4, 1};
  ASSERT_EQ(ClampSampleCounts(metal), 2u);
  EXPECT_EQ(metal[0], 4);
  EXPECT_EQ(metal[1], 1);

  std::array<int32_t, 1> eight = {8};
  EXPECT_EQ(ClampSampleCounts(eight), 0u);
  EXPECT_EQ(ClampSampleCounts(base::span<int32_t>()), 0u);

  // Never above the reported MAX_SAMPLES.
  EXPECT_EQ(Baseline(gl::kMaxSamples), 4);
}

TEST(MoltWebGLUniformTest, ExtensionAllowList) {
  const base::span<const std::string_view> allowed = AllowedExtensions();
  EXPECT_TRUE(std::is_sorted(allowed.begin(), allowed.end()));
  EXPECT_EQ(std::adjacent_find(allowed.begin(), allowed.end()),
            allowed.end());
  for (std::string_view name : allowed) {
    EXPECT_TRUE(IsExtensionAllowed(name)) << name;
  }

  for (std::string_view name :
       {"ANGLE_instanced_arrays", "OES_texture_float", "OES_vertex_array_object",
        "WEBGL_depth_texture", "EXT_color_buffer_float",
        "EXT_texture_filter_anisotropic", "WEBGL_compressed_texture_s3tc",
        "WEBGL_lose_context", "WEBGL_multi_draw"}) {
    EXPECT_TRUE(IsExtensionAllowed(name)) << name;
  }
  for (std::string_view name :
       {"WEBGL_debug_renderer_info", "WEBGL_debug_shaders",
        "EXT_disjoint_timer_query", "EXT_disjoint_timer_query_webgl2",
        "EXT_float_blend", "EXT_texture_compression_bptc",
        "EXT_texture_compression_rgtc", "WEBGL_compressed_texture_astc",
        "WEBGL_compressed_texture_etc", "WEBGL_compressed_texture_etc1",
        "WEBGL_compressed_texture_pvrtc", "EXT_texture_norm16",
        "WEBGL_clip_cull_distance", "OVR_multiview2",
        "WEBGL_shader_pixel_local_storage", "WEBGL_polygon_mode",
        "WEBGL_blend_func_extended", "EXT_depth_clamp", ""}) {
    EXPECT_FALSE(IsExtensionAllowed(name)) << name;
  }
  // Blink matches tracker names, which are canonical: exact comparison.
  EXPECT_FALSE(IsExtensionAllowed("oes_texture_float"));
  EXPECT_FALSE(IsExtensionAllowed("OES_texture_float "));
}

}  // namespace
}  // namespace blink::molt_fingerprinting::webgl_uniform
