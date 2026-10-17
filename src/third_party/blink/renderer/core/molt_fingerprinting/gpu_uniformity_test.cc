// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/gpu_uniformity.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace blink::molt_fingerprinting {
namespace {

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet is simulated with
// SetUniformGpuForTesting() on top of kFarble.
class MoltGpuUniformityTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetUniformGpuForTesting(false);
  }
  void TearDown() override {
    SetUniformGpuForTesting(false);
    ResetForTesting();
  }

  // A window whose top-level site is `top_level_url`'s site.
  LocalDOMWindow& Window(const char* top_level_url) {
    holders_.push_back(DummyPageHolder::CreateAndCommitNavigation(
        KURL("https://page.example/")));
    LocalDOMWindow& window = *holders_.back()->GetFrame().DomWindow();
    window.SetStorageKey(BlinkStorageKey::CreateFirstParty(
        SecurityOrigin::Create(KURL(top_level_url))));
    return window;
  }

  void SimulateMoltNet() {
    Configure(Mode::kFarble);
    SetUniformGpuForTesting(true);
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

// A desktop GPU above every baseline.
void ExpectRealValues(ExecutionContext* context) {
  EXPECT_EQ(UniformWebGLInt(context, GL_MAX_TEXTURE_SIZE, 16384), 16384);
  EXPECT_EQ(UniformWebGLInt(context, GL_MAX_VARYING_VECTORS, 31), 31);
  EXPECT_EQ(UniformWebGLInt64(context, GL_MAX_UNIFORM_BLOCK_SIZE, 65536),
            65536);
  EXPECT_EQ(
      UniformWebGLFloat(context, GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, 32.0f),
      32.0f);
  std::array<float, 4> points = {1.0f, 1024.0f, 0, 0};
  UniformWebGLFloats(context, GL_ALIASED_POINT_SIZE_RANGE, points);
  EXPECT_EQ(points[1], 1024.0f);
  std::array<int32_t, 4> dims = {32767, 32767, 0, 0};
  UniformWebGLInts(context, GL_MAX_VIEWPORT_DIMS, dims);
  EXPECT_EQ(dims[0], 32767);
  const ShaderPrecisionFormat mediump =
      UniformShaderPrecisionFormat(context, GL_MEDIUM_FLOAT, {127, 127, 23});
  EXPECT_EQ(mediump.precision, 23);
  std::array<int32_t, 3> samples = {8, 4, 2};
  EXPECT_EQ(UniformSampleCounts(context, samples), 3u);
  EXPECT_EQ(samples[0], 8);
  EXPECT_TRUE(WebGLExtensionAllowed(context, "WEBGL_debug_shaders"));
  EXPECT_TRUE(WebGLExtensionAllowed(context, "EXT_float_blend"));
  EXPECT_FALSE(HidesWebGPUAdapter(context));
}

TEST_F(MoltGpuUniformityTest, OffLeavesEverythingReal) {
  LocalDOMWindow& window = Window("https://a.example/");
  EXPECT_FALSE(UsesUniformGpu(&window));
  EXPECT_FALSE(UsesUniformGpu(nullptr));
  ExpectRealValues(&window);
  ExpectRealValues(nullptr);
}

TEST_F(MoltGpuUniformityTest, NormalWindowsLeaveEverythingReal) {
  Configure(Mode::kFarble);
  LocalDOMWindow& window = Window("https://a.example/");
  ASSERT_TRUE(IsFarbled(&window));
  EXPECT_FALSE(UsesUniformGpu(&window));
  ExpectRealValues(&window);
}

TEST_F(MoltGpuUniformityTest, MoltNetReportsTheBaselines) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://a.example/");
  ASSERT_TRUE(UsesUniformGpu(&window));

  EXPECT_EQ(UniformWebGLInt(&window, GL_MAX_TEXTURE_SIZE, 16384), 8192);
  EXPECT_EQ(UniformWebGLInt(&window, GL_MAX_TEXTURE_SIZE, 4096), 4096);
  EXPECT_EQ(UniformWebGLInt(&window, GL_MAX_VARYING_VECTORS, 31), 30);
  EXPECT_EQ(UniformWebGLInt(&window, GL_MAX_DRAW_BUFFERS_EXT, 8), 8);
  EXPECT_EQ(UniformWebGLInt(&window, GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, 4),
            256);
  EXPECT_EQ(UniformWebGLInt64(&window, GL_MAX_UNIFORM_BLOCK_SIZE, 65536),
            16384);
  EXPECT_EQ(
      UniformWebGLFloat(&window, GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, 32.0f),
      16.0f);

  std::array<float, 4> points = {1.0f, 1024.0f, 0, 0};
  UniformWebGLFloats(&window, GL_ALIASED_POINT_SIZE_RANGE, points);
  EXPECT_EQ(points[0], 1.0f);
  EXPECT_EQ(points[1], 255.0f);
  std::array<int32_t, 4> dims = {32767, 32767, 0, 0};
  UniformWebGLInts(&window, GL_MAX_VIEWPORT_DIMS, dims);
  EXPECT_EQ(dims[0], 8192);
  EXPECT_EQ(dims[1], 8192);

  const ShaderPrecisionFormat mediump =
      UniformShaderPrecisionFormat(&window, GL_MEDIUM_FLOAT, {127, 127, 23});
  EXPECT_EQ(mediump.range_min, 15);
  EXPECT_EQ(mediump.range_max, 15);
  EXPECT_EQ(mediump.precision, 10);
  const ShaderPrecisionFormat highp =
      UniformShaderPrecisionFormat(&window, GL_HIGH_FLOAT, {127, 127, 23});
  EXPECT_EQ(highp.precision, 23);

  std::array<int32_t, 3> samples = {8, 4, 2};
  ASSERT_EQ(UniformSampleCounts(&window, samples), 2u);
  EXPECT_EQ(samples[0], 4);
  EXPECT_EQ(samples[1], 2);

  EXPECT_TRUE(WebGLExtensionAllowed(&window, "OES_texture_float"));
  EXPECT_TRUE(WebGLExtensionAllowed(&window, "WEBGL_lose_context"));
  EXPECT_FALSE(WebGLExtensionAllowed(&window, "WEBGL_debug_renderer_info"));
  EXPECT_FALSE(WebGLExtensionAllowed(&window, "WEBGL_debug_shaders"));
  EXPECT_FALSE(WebGLExtensionAllowed(&window, "EXT_float_blend"));
  EXPECT_FALSE(WebGLExtensionAllowed(&window, nullptr));

  EXPECT_TRUE(HidesWebGPUAdapter(&window));
}

TEST_F(MoltGpuUniformityTest, MoltNetLeavesStateAlone) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://a.example/");
  EXPECT_EQ(UniformWebGLInt(&window, GL_PACK_ALIGNMENT, 8), 8);
  EXPECT_EQ(UniformWebGLInt(&window, GL_SAMPLES, 16), 16);
  EXPECT_EQ(UniformWebGLFloat(&window, GL_LINE_WIDTH, 3.0f), 3.0f);
  std::array<int32_t, 4> viewport = {0, 0, 30000, 20000};
  UniformWebGLInts(&window, GL_VIEWPORT, viewport);
  EXPECT_EQ(viewport[2], 30000);
  std::array<float, 4> depth = {0.25f, 0.75f, 0, 0};
  UniformWebGLFloats(&window, GL_DEPTH_RANGE, depth);
  EXPECT_EQ(depth[0], 0.25f);
  EXPECT_EQ(depth[1], 0.75f);
}

TEST_F(MoltGpuUniformityTest, MoltNetOpaqueAndNullContextsAreUniform) {
  SimulateMoltNet();
  // A sandboxed top-level document has an opaque site: still web content.
  LocalDOMWindow& opaque = Window("data:text/html,x");
  EXPECT_TRUE(UsesUniformGpu(&opaque));
  EXPECT_EQ(UniformWebGLInt(&opaque, GL_MAX_TEXTURE_SIZE, 16384), 8192);
  // No context: fail closed.
  EXPECT_TRUE(UsesUniformGpu(nullptr));
  EXPECT_TRUE(HidesWebGPUAdapter(nullptr));
  EXPECT_EQ(UniformWebGLInt(nullptr, GL_MAX_TEXTURE_SIZE, 16384), 8192);
}

TEST_F(MoltGpuUniformityTest, MoltNetLeavesNonWebPagesReal) {
  SimulateMoltNet();
  // Like chrome:// pages: a top-level site that isn't http(s).
  LocalDOMWindow& local = Window("file:///home/user/page.html");
  ASSERT_FALSE(IsFarbled(&local));
  EXPECT_FALSE(UsesUniformGpu(&local));
  ExpectRealValues(&local);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
