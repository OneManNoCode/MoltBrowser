// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/letterbox.h"

#include <memory>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/renderer/core/css/media_query_list.h"
#include "third_party/blink/renderer/core/css/media_values.h"
#include "third_party/blink/renderer/core/css/media_values_cached.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/frame/dom_visual_viewport.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/geometry/dom_rect.h"
#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/html/html_image_element.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/layout/adjust_for_absolute_zoom.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/loader/empty_clients.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/scroll/scrollbar.h"
#include "third_party/blink/renderer/core/scroll/scrollbar_theme.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/core/testing/scoped_mock_overlay_scrollbars.h"
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
// locale for every later test): MoltNet's density, scrollbars and viewport
// lengths are simulated with SetUniformForTesting(), and the screen size is
// tested through UniformScreenSizeOf(), which UniformScreenRect() returns as
// is in kUniform.
class MoltLetterboxTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetUniformForTesting(false);
  }
  void TearDown() override {
    SetUniformForTesting(false);
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
        // LocalFrameView::SmallViewportSizeForViewportUnits().
        gfx::SizeF units{gfx::Size(pixels, pixels)};
        units.InvScale(zoom);
        ASSERT_EQ(PageViewportPixels(pixels, zoom), units.width())
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
    EXPECT_EQ(frame.View()->SmallViewportSizeForViewportUnits().width(),
              PageViewportPixels(pixels, zoom));
    EXPECT_EQ(frame.DomWindow()->visualViewport()->width(),
              PageViewportPixels(pixels, zoom));
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
  SetUniformForTesting(true);
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
  SetUniformForTesting(true);
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

  SetUniformForTesting(true);
  auto* uniform = MakeGarbageCollected<HTMLImageElement>(document);
  uniform->setAttribute(html_names::kSrcsetAttr, srcset);
  EXPECT_EQ(uniform->ImageSourceURL(), "one.png");
}

// --- Scrollbars
// ---------------------------------------------------------------

// Three 200 x 100 scrolling boxes (scrollbar-width auto, thin and none), a
// 20-column textarea with and without a scrollbar, and a scrolling root.
void AddScrollingBoxes(Document& document) {
  document.documentElement()->setAttribute(html_names::kStyleAttr,
                                           AtomicString("overflow: scroll"));
  document.body()->SetInnerHTMLWithoutTrustedTypes(R"HTML(
    <style>
      div { width: 200px; height: 100px; overflow: scroll; }
      textarea { padding: 0; border: 0; font: 10px monospace; }
    </style>
    <div id="auto"></div>
    <div id="thin" style="scrollbar-width: thin"></div>
    <div id="none" style="scrollbar-width: none"></div>
    <textarea id="area" cols="20"></textarea>
    <textarea id="bare" cols="20" style="scrollbar-width: none"></textarea>
  )HTML");
  document.UpdateStyleAndLayout(DocumentUpdateReason::kTest);
}

// A scrollbar's thickness in the CSS pixels of its document, as pages
// measure it.
int ScrollbarWidth(Document& document, const char* id) {
  Element* box = document.getElementById(AtomicString(id));
  return box->OffsetWidth() - box->clientWidth();
}

double TextareaWidth(Document& document, const char* id) {
  return document.getElementById(AtomicString(id))
      ->GetBoundingClientRect()
      ->width();
}

// MoltNet's classic scrollbars are 16 CSS pixels thick (12 thin) at every
// zoom level and scale factor where that is a whole number of device
// pixels, whatever the OS's theme draws.
TEST_F(MoltLetterboxTest, ClassicScrollbarsAreUniformInMoltNet) {
  USE_NON_OVERLAY_SCROLLBARS_OR_QUIT();
  SetUniformForTesting(true);
  const std::pair<float, double> scales[] = {
      {1.0f, 1.0},  {2.0f, 1.0},  {1.25f, 1.0}, {1.5f, 1.0},
      {1.75f, 1.0}, {1.0f, 1.25}, {1.0f, 2.0},  {2.0f, 0.5},
      {1.0f, 0.75}, {1.0f, 0.25}, {1.0f, 1.5},  {2.0f, 1.75}};
  for (const auto& [device_scale_factor, zoom] : scales) {
    SCOPED_TRACE(testing::Message() << device_scale_factor << " " << zoom);
    LocalFrame& frame =
        MainFrame("https://page.example/", gfx::Size(1600, 1200),
                  device_scale_factor, PresetZoom(zoom));
    Document& document = *frame.GetDocument();
    AddScrollingBoxes(document);
    EXPECT_EQ(ScrollbarWidth(document, "auto"), kUniformScrollbarThickness);
    EXPECT_EQ(ScrollbarWidth(document, "thin"), kUniformThinScrollbarThickness);
    EXPECT_EQ(ScrollbarWidth(document, "none"), 0);
    // The root scroller: innerWidth - documentElement.clientWidth.
    EXPECT_EQ(frame.DomWindow()->innerWidth() -
                  document.documentElement()->clientWidth(),
              kUniformScrollbarThickness);
    // A textarea's width counts the scrollbar it may get.
    EXPECT_NEAR(
        TextareaWidth(document, "area") - TextareaWidth(document, "bare"),
        kUniformScrollbarThickness, 0.02);
    // 100vw leaves out the scrollbar a root always has (overflow: scroll).
    EXPECT_EQ(
        document.GetLayoutView()->SmallViewportSizeForViewportUnits().width(),
        frame.View()->SmallViewportSizeForViewportUnits().width() -
            kUniformScrollbarThickness);
  }
}

// Where 16 CSS pixels aren't whole device pixels the scrollbar is the
// nearest whole number of them (90% on a 1x screen: 14 device pixels, 15.56
// CSS pixels), which a scrolling box rounds to 16.
TEST_F(MoltLetterboxTest, ScrollbarsAreTheNearestDevicePixels) {
  USE_NON_OVERLAY_SCROLLBARS_OR_QUIT();
  SetUniformForTesting(true);
  LocalFrame& frame = MainFrame("https://page.example/", gfx::Size(1260, 810),
                                1.0f, PresetZoom(0.9));
  Document& document = *frame.GetDocument();
  AddScrollingBoxes(document);
  auto* area =
      To<LayoutBox>(
          document.getElementById(AtomicString("auto"))->GetLayoutObject())
          ->GetScrollableArea();
  ASSERT_TRUE(area);
  ASSERT_TRUE(area->VerticalScrollbar());
  EXPECT_EQ(area->VerticalScrollbar()->ScrollbarThickness(), 14);
  EXPECT_EQ(ScrollbarWidth(document, "auto"), kUniformScrollbarThickness);
}

// Regular and Incognito windows keep the theme's scrollbars, in DIPs.
TEST_F(MoltLetterboxTest, ScrollbarsOutsideMoltNetAreTheThemes) {
  USE_NON_OVERLAY_SCROLLBARS_OR_QUIT();
  LocalFrame& frame =
      MainFrame("https://page.example/", gfx::Size(1600, 1200), 1.0f, 1.0f);
  Document& document = *frame.GetDocument();
  AddScrollingBoxes(document);
  // DummyPageHolder's chrome client scales DIPs by 1.
  const int native = ScrollbarTheme::GetTheme().ScrollbarThickness(
      1.0f, EScrollbarWidth::kAuto);
  EXPECT_EQ(ScrollbarWidth(document, "auto"), native);
  auto* area =
      To<LayoutBox>(
          document.getElementById(AtomicString("auto"))->GetLayoutObject())
          ->GetScrollableArea();
  EXPECT_EQ(area->ScaleFromDIP(), 1.0f);
  EXPECT_EQ(ScrollbarThicknessFor(ScrollbarTheme::GetTheme(), 1.0f,
                                  EScrollbarWidth::kAuto, &document),
            native);

  // Zoomed, a regular window's scrollbar keeps its DIPs, so its CSS width
  // shows the zoom.
  LocalFrame& zoomed = MainFrame("https://page.example/", gfx::Size(1600, 1200),
                                 1.0f, PresetZoom(2.0));
  AddScrollingBoxes(*zoomed.GetDocument());
  EXPECT_LT(ScrollbarWidth(*zoomed.GetDocument(), "auto"), native);
}

// The scale scrollbars are drawn at follows the page zoom in MoltNet, so
// their arrows and thumb keep their proportions.
TEST_F(MoltLetterboxTest, ScrollbarsAreDrawnAtThePageZoomInMoltNet) {
  USE_NON_OVERLAY_SCROLLBARS_OR_QUIT();
  LocalFrame& frame = MainFrame("https://page.example/", gfx::Size(1600, 1200),
                                2.0f, PresetZoom(1.5));
  Document& document = *frame.GetDocument();
  AddScrollingBoxes(document);
  auto* area =
      To<LayoutBox>(
          document.getElementById(AtomicString("auto"))->GetLayoutObject())
          ->GetScrollableArea();
  EXPECT_EQ(ScrollbarScaleFromDIP(*area, 2.0f), 2.0f);
  SetUniformForTesting(true);
  EXPECT_EQ(ScrollbarScaleFromDIP(*area, 2.0f), frame.LayoutZoomFactor());
  EXPECT_EQ(area->ScaleFromDIP(), frame.LayoutZoomFactor());
}

// Overlay scrollbars take no room in the layout: left to the theme. So are
// internal pages' scrollbars.
TEST_F(MoltLetterboxTest, OverlayScrollbarsAndInternalPagesAreTheThemes) {
  SetUniformForTesting(true);
  LocalFrame& frame = MainFrame("https://page.example/", gfx::Size(1600, 1200),
                                1.0f, PresetZoom(2.0));
  Document& document = *frame.GetDocument();
  {
    ScopedMockOverlayScrollbars mock_overlay_scrollbars;
    const ScrollbarTheme& overlay = ScrollbarTheme::GetTheme();
    ASSERT_TRUE(overlay.UsesOverlayScrollbars());
    EXPECT_EQ(
        ScrollbarThicknessFor(overlay, 1.0f, EScrollbarWidth::kAuto, &document),
        overlay.ScrollbarThickness(1.0f, EScrollbarWidth::kAuto));
    EXPECT_EQ(ScrollbarThicknessForViewportUnits(
                  overlay, EScrollbarWidth::kAuto, &document),
              overlay.ScrollbarThickness(1.0f, EScrollbarWidth::kAuto));
    AddScrollingBoxes(document);
    EXPECT_EQ(ScrollbarWidth(document, "auto"), 0);
  }
  USE_NON_OVERLAY_SCROLLBARS_OR_QUIT();
  const ScrollbarTheme& classic = ScrollbarTheme::GetTheme();
  EXPECT_EQ(
      ScrollbarThicknessFor(classic, 1.0f, EScrollbarWidth::kAuto, &document),
      2 * kUniformScrollbarThickness);
  EXPECT_EQ(
      ScrollbarThicknessFor(classic, 1.0f, EScrollbarWidth::kThin, &document),
      2 * kUniformThinScrollbarThickness);
  EXPECT_EQ(
      ScrollbarThicknessFor(classic, 1.0f, EScrollbarWidth::kNone, &document),
      0);
  // Viewport units take it in CSS pixels, whatever the zoom.
  EXPECT_EQ(ScrollbarThicknessForViewportUnits(classic, EScrollbarWidth::kAuto,
                                               &document),
            kUniformScrollbarThickness);
  EXPECT_EQ(ScrollbarThicknessForViewportUnits(classic, EScrollbarWidth::kThin,
                                               &document),
            kUniformThinScrollbarThickness);
  EXPECT_EQ(
      ScrollbarThicknessFor(classic, 1.0f, EScrollbarWidth::kAuto, nullptr),
      classic.ScrollbarThickness(1.0f, EScrollbarWidth::kAuto));
  EXPECT_EQ(ScrollbarThicknessForViewportUnits(classic, EScrollbarWidth::kAuto,
                                               nullptr),
            classic.ScrollbarThickness(1.0f, EScrollbarWidth::kAuto));
  // Like chrome:// pages: a top-level origin that isn't http(s).
  frame.DomWindow()->GetSecurityContext().SetSecurityOriginForTesting(
      SecurityOrigin::CreateFromValidTuple("file", "", 0));
  EXPECT_EQ(
      ScrollbarThicknessFor(classic, 1.0f, EScrollbarWidth::kAuto, &document),
      classic.ScrollbarThickness(1.0f, EScrollbarWidth::kAuto));
  EXPECT_EQ(ScrollbarThicknessForViewportUnits(classic, EScrollbarWidth::kAuto,
                                               &document),
            classic.ScrollbarThickness(1.0f, EScrollbarWidth::kAuto));
}

// --- Viewport lengths
// ----------------------------------------------------------

// At 90% on a 1x screen a 1400 x 900 CSS-pixel page is 1260 x 810 device
// pixels; upstream's media queries see it 1400.0001 wide (they multiply by
// 1 / 0.9f), so (width > 1400px) matched. In MoltNet every length the page
// reads is the whole 1400 x 900, like innerWidth / innerHeight.
TEST_F(MoltLetterboxTest, ViewportLengthsAreWholeInMoltNet) {
  const float zoom = PresetZoom(0.9);
  LocalFrame& frame =
      MainFrame("https://page.example/", gfx::Size(1260, 810), 1.0f, zoom);
  LocalDOMWindow& window = *frame.DomWindow();
  ASSERT_EQ(window.innerWidth(), 1400);
  ASSERT_EQ(window.innerHeight(), 900);
  // Upstream.
  EXPECT_EQ(*MediaValues::CreateDynamicIfFrameExists(&frame)->Width(),
            PageMediaQueryPixels(1260, zoom));
  EXPECT_NE(*MediaValues::CreateDynamicIfFrameExists(&frame)->Width(), 1400.0);
  EXPECT_TRUE(window.matchMedia("(width > 1400px)")->matches());
  // The viewport units divide, which happens to be exact here, so the media
  // queries contradicted them too.
  EXPECT_EQ(frame.View()->SmallViewportSizeForViewportUnits().width(), 1400);
  EXPECT_TRUE(window.matchMedia("(width > 100vw)")->matches());

  SetUniformForTesting(true);
  EXPECT_EQ(*MediaValues::CreateDynamicIfFrameExists(&frame)->Width(), 1400.0);
  EXPECT_EQ(*MediaValues::CreateDynamicIfFrameExists(&frame)->Height(), 900.0);
  EXPECT_EQ(
      *MakeGarbageCollected<MediaValuesCached>(*frame.GetDocument())->Width(),
      1400.0);
  EXPECT_FALSE(window.matchMedia("(width > 1400px)")->matches());
  EXPECT_FALSE(window.matchMedia("(height < 900px)")->matches());
  EXPECT_TRUE(
      window.matchMedia("(width: 1400px) and (height: 900px)")->matches());
  EXPECT_FALSE(window.matchMedia("(width > 100vw)")->matches());
  EXPECT_EQ(frame.View()->SmallViewportSizeForViewportUnits(),
            gfx::SizeF(1400, 900));
  EXPECT_EQ(window.visualViewport()->width(), 1400.0);
  EXPECT_EQ(window.visualViewport()->height(), 900.0);

  // 270 device pixels at 90% on a 1.5 screen are 200 CSS pixels, but the
  // division gives 200.0000153 too: vw and visualViewport.
  const float scaled_zoom = 1.5f * zoom;
  LocalFrame& scaled =
      MainFrame("https://page.example/", gfx::Size(270, 270), 1.5f, zoom);
  ASSERT_EQ(scaled.DomWindow()->innerWidth(), 200);
  SetUniformForTesting(false);
  EXPECT_EQ(scaled.View()->SmallViewportSizeForViewportUnits().width(),
            PageViewportPixels(270, scaled_zoom));
  EXPECT_NE(PageViewportPixels(270, scaled_zoom), 200.0f);
  EXPECT_EQ(scaled.DomWindow()->visualViewport()->width(),
            PageViewportPixels(270, scaled_zoom));
  SetUniformForTesting(true);
  EXPECT_EQ(scaled.View()->SmallViewportSizeForViewportUnits(),
            gfx::SizeF(200, 200));
  EXPECT_EQ(scaled.DomWindow()->visualViewport()->width(), 200.0);
  EXPECT_EQ(*MediaValues::CreateDynamicIfFrameExists(&scaled)->Width(), 200.0);
}

// Lengths further off than float rounding (a page the browser couldn't
// letterbox exactly), other modes and internal pages are left alone.
TEST_F(MoltLetterboxTest, OnlyNearlyWholeWebViewportLengthsSnap) {
  LocalFrame& frame =
      MainFrame("https://page.example/", gfx::Size(1260, 810), 1.0f, 1.0f);
  EXPECT_EQ(UniformViewportLength(frame, 1400.0001220703125),
            1400.0001220703125);
  SetUniformForTesting(true);
  EXPECT_EQ(UniformViewportLength(frame, 1400.0001220703125), 1400.0);
  EXPECT_EQ(UniformViewportLength(frame, 500.1875), 500.1875);
  EXPECT_EQ(UniformViewportSize(frame, gfx::SizeF(1399.9999f, 500.25f)),
            gfx::SizeF(1400, 500.25f));
  // A page 1313 device pixels high on a 1.5 screen at 175%: 500.19 CSS
  // pixels for the media queries, not snapped.
  LocalFrame& inexact = MainFrame(
      "https://page.example/", gfx::Size(1000, 1313), 1.5f, PresetZoom(1.75));
  EXPECT_EQ(*MediaValues::CreateDynamicIfFrameExists(&inexact)->Height(),
            PageMediaQueryPixels(1313, 1.5f * PresetZoom(1.75)));

  frame.DomWindow()->GetSecurityContext().SetSecurityOriginForTesting(
      SecurityOrigin::CreateFromValidTuple("file", "", 0));
  EXPECT_EQ(UniformViewportLength(frame, 1400.0001220703125),
            1400.0001220703125);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
