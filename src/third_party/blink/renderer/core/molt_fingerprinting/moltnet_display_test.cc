// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_display.h"

#include <memory>
#include <vector>

#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/css/media_query_list.h"
#include "third_party/blink/renderer/core/css/media_values.h"
#include "third_party/blink/renderer/core/css/media_values_cached.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/loader/empty_clients.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/graphics/color_space_gamut.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "ui/display/screen_info.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/display_color_spaces.h"
#include "ui/gfx/geometry/size.h"

namespace blink::molt_fingerprinting {
namespace {

// A wide-gamut HDR display with 10 bits per component, as on a recent
// MacBook Pro or an HDR monitor.
class WideGamutChromeClient final : public EmptyChromeClient {
 public:
  WideGamutChromeClient() {
    screen_info_.display_color_spaces =
        gfx::DisplayColorSpaces(gfx::ColorSpace::CreateDisplayP3D65());
    screen_info_.display_color_spaces.SetHDRMaxLuminanceRelative(4.0f);
    screen_info_.depth = 30;
    screen_info_.depth_per_component = 10;
  }
  const display::ScreenInfo& GetScreenInfo(LocalFrame&) const override {
    return screen_info_;
  }

 private:
  display::ScreenInfo screen_info_;
};

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet is simulated with
// SetMoltNetDisplayForTesting().
class MoltNetDisplayTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetDisplayForTesting(false);
  }
  void TearDown() override {
    SetMoltNetDisplayForTesting(false);
    ResetForTesting();
  }

  LocalFrame& FrameOnAWideGamutDisplay() {
    holders_.push_back(DummyPageHolder::CreateAndCommitNavigation(
        KURL("https://page.example/"), gfx::Size(800, 600),
        MakeGarbageCollected<WideGamutChromeClient>()));
    return holders_.back()->GetFrame();
  }

  static bool Matches(LocalFrame& frame, const char* query) {
    return frame.DomWindow()->matchMedia(query)->matches();
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

TEST_F(MoltNetDisplayTest, NormalWindowsSeeTheRealDisplay) {
  LocalFrame& frame = FrameOnAWideGamutDisplay();
  const MediaValues* values = MediaValues::CreateDynamicIfFrameExists(&frame);
  EXPECT_EQ(values->ColorGamut(), ColorSpaceGamut::P3);
  EXPECT_TRUE(values->DeviceSupportsHDR());
  EXPECT_EQ(values->ColorBitsPerComponent(), 10);
  EXPECT_TRUE(Matches(frame, "(color-gamut: p3)"));
  EXPECT_TRUE(Matches(frame, "(dynamic-range: high)"));
  EXPECT_TRUE(Matches(frame, "(video-dynamic-range: high)"));
  EXPECT_TRUE(Matches(frame, "(color: 10)"));
  EXPECT_FALSE(UniformDisplayColorGamut());
  EXPECT_FALSE(UniformDisplaySupportsHDR());
  EXPECT_FALSE(UniformDisplayColorBitsPerComponent());
  EXPECT_FALSE(UniformDisplayMonochromeBitsPerComponent());
}

TEST_F(MoltNetDisplayTest, MoltNetPagesSeeAPlainSrgbDisplay) {
  Configure(Mode::kFarble);
  SetMoltNetDisplayForTesting(true);
  LocalFrame& frame = FrameOnAWideGamutDisplay();

  const MediaValues* values = MediaValues::CreateDynamicIfFrameExists(&frame);
  EXPECT_EQ(values->ColorGamut(), ColorSpaceGamut::SRGB);
  EXPECT_FALSE(values->DeviceSupportsHDR());
  EXPECT_EQ(values->ColorBitsPerComponent(), 8);
  EXPECT_EQ(values->MonochromeBitsPerComponent(), 0);

  // What the preload scanner copies agrees.
  const MediaValues* cached =
      MakeGarbageCollected<MediaValuesCached>(*frame.GetDocument());
  EXPECT_EQ(cached->ColorGamut(), ColorSpaceGamut::SRGB);
  EXPECT_FALSE(cached->DeviceSupportsHDR());
  EXPECT_EQ(cached->ColorBitsPerComponent(), 8);

  EXPECT_TRUE(Matches(frame, "(color-gamut: srgb)"));
  EXPECT_FALSE(Matches(frame, "(color-gamut: p3)"));
  EXPECT_FALSE(Matches(frame, "(color-gamut: rec2020)"));
  EXPECT_TRUE(Matches(frame, "(dynamic-range: standard)"));
  EXPECT_FALSE(Matches(frame, "(dynamic-range: high)"));
  EXPECT_FALSE(Matches(frame, "(video-dynamic-range: high)"));
  EXPECT_TRUE(Matches(frame, "(color: 8)"));
  EXPECT_FALSE(Matches(frame, "(min-color: 9)"));
  EXPECT_FALSE(Matches(frame, "(monochrome)"));
}

TEST_F(MoltNetDisplayTest, ScrollbarStyleIsThePlatformDefaultInMoltNet) {
  // Outside MoltNet the browser's (OS) setting is applied as it comes.
  EXPECT_TRUE(UniformOverlayScrollbars(true));
  EXPECT_FALSE(UniformOverlayScrollbars(false));

  Configure(Mode::kFarble);
  SetMoltNetDisplayForTesting(true);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_ANDROID)
  constexpr bool kPlatformDefault = true;
#else
  constexpr bool kPlatformDefault = false;
#endif
  EXPECT_EQ(UniformOverlayScrollbars(true), kPlatformDefault);
  EXPECT_EQ(UniformOverlayScrollbars(false), kPlatformDefault);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
