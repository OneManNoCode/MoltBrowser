// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/letterbox.h"

#include <memory>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/renderer/core/css/media_values.h"
#include "third_party/blink/renderer/core/css/media_values_cached.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_image_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/layout/adjust_for_absolute_zoom.h"
#include "third_party/blink/renderer/core/loader/empty_clients.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "ui/display/screen_info.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_f.h"

namespace blink::molt_fingerprinting {
namespace {

// A screen of `device_scale_factor`.
class ScaledChromeClient final : public EmptyChromeClient {
 public:
  explicit ScaledChromeClient(float device_scale_factor) {
    screen_info_.device_scale_factor = device_scale_factor;
  }
  const display::ScreenInfo& GetScreenInfo(LocalFrame&) const override {
    return screen_info_;
  }

 private:
  display::ScreenInfo screen_info_;
};

// The density media queries see, live and as the preload scanner copies it.
float MediaDevicePixelRatio(LocalFrame& frame) {
  return MediaValues::CreateDynamicIfFrameExists(&frame)->DevicePixelRatio();
}
float CachedMediaDevicePixelRatio(LocalFrame& frame) {
  return MakeGarbageCollected<MediaValuesCached>(*frame.GetDocument())
      ->DevicePixelRatio();
}

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test): MoltNet's density is simulated with
// SetUniformDevicePixelRatioForTesting(), and the screen size is tested
// through UniformScreenSizeOf(), which UniformScreenRect() returns as is in
// kUniform.
class MoltLetterboxTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetUniformDevicePixelRatioForTesting(false);
  }
  void TearDown() override {
    SetUniformDevicePixelRatioForTesting(false);
    ResetForTesting();
  }

  // The main frame of a page at `url` whose frame is `size` device pixels
  // on a screen of `device_scale_factor`, zoomed to `zoom` (Blink's layout
  // zoom is then the device scale factor times the zoom, as
  // WebFrameWidgetImpl::SetZoomInternal() computes it).
  LocalFrame& MainFrame(const char* url,
                        const gfx::Size& size = gfx::Size(800, 600),
                        float device_scale_factor = 1.0f,
                        float zoom = 1.0f) {
    holders_.push_back(DummyPageHolder::CreateAndCommitNavigation(
        KURL(url), size,
        MakeGarbageCollected<ScaledChromeClient>(device_scale_factor)));
    LocalFrame& frame = holders_.back()->GetFrame();
    frame.SetLayoutZoomFactor(device_scale_factor * zoom);
    return frame;
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

// The page reports its own (letterboxed) size as the screen, so
// screen.width/height equal innerWidth/innerHeight.
TEST_F(MoltLetterboxTest, ScreenIsTheLetterboxedPageSize) {
  LocalFrame& frame = MainFrame("https://page.example/", gfx::Size(1200, 800));
  EXPECT_EQ(UniformScreenSizeOf(frame), gfx::Size(1200, 800));
  EXPECT_EQ(frame.DomWindow()->innerWidth(), 1200);
  EXPECT_EQ(frame.DomWindow()->innerHeight(), 800);

  LocalFrame& hidpi =
      MainFrame("https://page.example/", gfx::Size(2800, 1800), 2.0f);
  EXPECT_EQ(UniformScreenSizeOf(hidpi), gfx::Size(1400, 900));
  EXPECT_EQ(hidpi.DomWindow()->innerWidth(), 1400);
  EXPECT_EQ(hidpi.DomWindow()->innerHeight(), 900);

  LocalFrame& fractional =
      MainFrame("https://page.example/", gfx::Size(1250, 625), 1.25f);
  EXPECT_EQ(UniformScreenSizeOf(fractional), gfx::Size(1000, 500));
  EXPECT_EQ(fractional.DomWindow()->innerWidth(), 1000);
  EXPECT_EQ(fractional.DomWindow()->innerHeight(), 500);
}

// A page the browser hasn't letterboxed (a tiny window, DevTools device
// emulation): rounded down to the steps, capped, never below one step.
TEST_F(MoltLetterboxTest, OtherSizesAreBucketed) {
  EXPECT_EQ(UniformScreenSizeOf(
                MainFrame("https://a.example/", gfx::Size(1399, 899))),
            gfx::Size(1200, 800));
  EXPECT_EQ(UniformScreenSizeOf(
                MainFrame("https://a.example/", gfx::Size(2560, 1440))),
            gfx::Size(1400, 900));
  EXPECT_EQ(UniformScreenSizeOf(
                MainFrame("https://a.example/", gfx::Size(5120, 2880), 2.0f)),
            gfx::Size(1400, 900));
  EXPECT_EQ(
      UniformScreenSizeOf(MainFrame("https://a.example/", gfx::Size(150, 80))),
      gfx::Size(200, 100));
}

// A preset zoom factor as the renderer gets it: made a zoom level by the
// browser, a factor again and a float here (SetZoomInternal()).
float PresetZoom(double preset) {
  return static_cast<float>(
      ZoomLevelToZoomFactor(ZoomFactorToZoomLevel(preset)));
}

// The browser letterboxes a zoomed page in CSS pixels, and the screen is
// that size too: neither shows the zoom.
TEST_F(MoltLetterboxTest, ZoomedPagesReportTheirCssSizeAsTheScreen) {
  struct Case {
    gfx::Size device_pixels;
    float device_scale_factor;
    double zoom;
    gfx::Size css;
  };
  const Case cases[] = {
      {gfx::Size(1500, 1000), 1.0f, 1.25, gfx::Size(1200, 800)},
      {gfx::Size(1260, 810), 1.0f, 0.9, gfx::Size(1400, 900)},
      {gfx::Size(1400, 900), 2.0f, 0.5, gfx::Size(1400, 900)},
      {gfx::Size(2400, 1600), 2.0f, 2.0, gfx::Size(600, 400)},
      {gfx::Size(400, 300), 1.0f, 1 / 3.0, gfx::Size(1200, 900)},
      {gfx::Size(1440, 1080), 1.2f, 1.0, gfx::Size(1200, 900)},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(testing::Message() << c.device_scale_factor << " " << c.zoom);
    LocalFrame& frame = MainFrame("https://page.example/", c.device_pixels,
                                  c.device_scale_factor, PresetZoom(c.zoom));
    EXPECT_EQ(UniformScreenScale(frame),
              c.device_scale_factor * PresetZoom(c.zoom));
    EXPECT_EQ(UniformScreenSizeOf(frame), c.css);
    EXPECT_EQ(frame.DomWindow()->innerWidth(), c.css.width());
    EXPECT_EQ(frame.DomWindow()->innerHeight(), c.css.height());
  }
}

// The screen used to be the page's DIP size, so next to a zoomed page's
// innerWidth/innerHeight it gave the zoom away; now it follows the zoom,
// even before the browser has letterboxed the page again.
TEST_F(MoltLetterboxTest, TheScreenFollowsThePageZoom) {
  LocalFrame& frame =
      MainFrame("https://page.example/", gfx::Size(2800, 1800), 2.0f);
  EXPECT_EQ(UniformScreenSizeOf(frame), gfx::Size(1400, 900));
  // The user zooms to 175%; the browser hasn't letterboxed the page again
  // yet. The screen is the page's CSS size, bucketed, like innerWidth.
  frame.SetLayoutZoomFactor(2.0f * PresetZoom(1.75));
  EXPECT_EQ(frame.DomWindow()->innerWidth(), 800);
  EXPECT_EQ(UniformScreenSizeOf(frame), gfx::Size(800, 500));
  EXPECT_EQ(UniformScreenScale(frame), 3.5f);
}

// The browser predicts the page's CSS size with its own copy of this
// arithmetic, which molt_letterbox_unittest.cc checks against farbling.h's;
// farbling.h's must be gfx's and Blink's.
TEST_F(MoltLetterboxTest, PredictedArithmeticIsBlinks) {
  const float scale_factors[] = {1.0f, 1.1f,  1.15f, 1.2f,  1.25f, 1.3f, 1.4f,
                                 1.5f, 1.75f, 2.0f,  2.25f, 2.5f,  3.0f};
  for (float scale : scale_factors) {
    for (int dips = 0; dips <= 4000; ++dips) {
      ASSERT_EQ(PageDevicePixels(dips, scale),
                gfx::ScaleToCeiledSize(gfx::Size(dips, dips), scale).width())
          << scale << " " << dips;
    }
    for (double preset : kPresetBrowserZoomFactors) {
      const float zoom = scale * PresetZoom(preset);
      for (int pixels = 0; pixels <= 12000; ++pixels) {
        ASSERT_EQ(PageCssPixels(pixels, zoom),
                  AdjustForAbsoluteZoom::AdjustInt(pixels, zoom))
            << zoom << " " << pixels;
        // LocalFrameView::ViewportSizeForMediaQueries().
        gfx::SizeF media{gfx::Size(pixels, pixels)};
        media.Scale(1 / zoom);
        ASSERT_EQ(PageMediaQueryPixels(pixels, zoom), media.width())
            << zoom << " " << pixels;
      }
    }
  }
  // And through a page: innerWidth and the width media queries compare.
  const std::pair<int, float> pages[] = {{1260, PresetZoom(0.9)},
                                         {1681, 1.2f},
                                         {1313, 1.5f * PresetZoom(1.75)},
                                         {1750, PresetZoom(1.25)},
                                         {467, PresetZoom(1 / 3.0)}};
  for (const auto& [pixels, zoom] : pages) {
    LocalFrame& frame = MainFrame("https://page.example/",
                                  gfx::Size(pixels, pixels), 1.0f, zoom);
    EXPECT_EQ(frame.DomWindow()->innerWidth(), PageCssPixels(pixels, zoom));
    EXPECT_EQ(*MediaValues::CreateDynamicIfFrameExists(&frame)->Width(),
              PageMediaQueryPixels(pixels, zoom));
  }
}

TEST_F(MoltLetterboxTest, RealDensityOutsideMoltNet) {
  LocalFrame& frame =
      MainFrame("https://page.example/", gfx::Size(1600, 1200), 2.0f);
  EXPECT_EQ(DevicePixelRatioFor(&frame, 2.0), 2.0);
  EXPECT_EQ(DevicePixelRatioFor(&frame, 2.0f), 2.0f);
  EXPECT_EQ(frame.DomWindow()->devicePixelRatio(), 2.0);
  EXPECT_EQ(MediaDevicePixelRatio(frame), 2.0f);
  EXPECT_EQ(CachedMediaDevicePixelRatio(frame), 2.0f);
}

TEST_F(MoltLetterboxTest, WebPagesReportOneDensityInMoltNet) {
  LocalFrame& frame =
      MainFrame("https://page.example/", gfx::Size(1600, 1200), 2.0f);
  SetUniformDevicePixelRatioForTesting(true);
  EXPECT_EQ(DevicePixelRatioFor(&frame, 2.0), kUniformDevicePixelRatio);
  EXPECT_EQ(DevicePixelRatioFor(&frame, 2.0f), 1.0f);
  // window.devicePixelRatio, the resolution media queries and the
  // preload scanner's copy of the media values.
  EXPECT_EQ(frame.DomWindow()->devicePixelRatio(), 1.0);
  EXPECT_EQ(MediaDevicePixelRatio(frame), 1.0f);
  EXPECT_EQ(CachedMediaDevicePixelRatio(frame), 1.0f);
  // Blink itself keeps rendering at the real scale.
  EXPECT_EQ(frame.DevicePixelRatio(), 2.0);
  EXPECT_EQ(frame.GetDocument()->DevicePixelRatio(), 2.0f);
  // No frame: nothing to change.
  EXPECT_EQ(DevicePixelRatioFor(nullptr, 2.0), 2.0);

  LocalFrame& http = MainFrame("http://plain.example/", gfx::Size(), 2.0f);
  EXPECT_EQ(http.DomWindow()->devicePixelRatio(), 1.0);
}

TEST_F(MoltLetterboxTest, InternalPagesKeepTheirDensity) {
  SetUniformDevicePixelRatioForTesting(true);
  LocalFrame& frame = MainFrame("https://page.example/", gfx::Size(), 2.0f);
  // Like chrome:// pages: a top-level origin that isn't http(s) (file:// is
  // a standard scheme without registering one for the test).
  frame.DomWindow()->GetSecurityContext().SetSecurityOriginForTesting(
      SecurityOrigin::CreateFromValidTuple("file", "", 0));
  EXPECT_EQ(DevicePixelRatioFor(&frame, 2.0), 2.0);
  EXPECT_EQ(frame.DomWindow()->devicePixelRatio(), 2.0);
  EXPECT_EQ(MediaDevicePixelRatio(frame), 2.0f);
}

// srcset candidates are picked for the reported density, so the image a
// page fetches doesn't give the real one away to its server.
TEST_F(MoltLetterboxTest, SrcsetIsPickedForTheReportedDensity) {
  LocalFrame& frame =
      MainFrame("https://page.example/", gfx::Size(1600, 1200), 2.0f);
  Document& document = *frame.GetDocument();
  const AtomicString srcset("one.png 1x, two.png 2x");

  auto* real = MakeGarbageCollected<HTMLImageElement>(document);
  real->setAttribute(html_names::kSrcsetAttr, srcset);
  EXPECT_EQ(real->ImageSourceURL(), "two.png");

  SetUniformDevicePixelRatioForTesting(true);
  auto* uniform = MakeGarbageCollected<HTMLImageElement>(document);
  uniform->setAttribute(html_names::kSrcsetAttr, srcset);
  EXPECT_EQ(uniform->ImageSourceURL(), "one.png");
}

}  // namespace
}  // namespace blink::molt_fingerprinting
