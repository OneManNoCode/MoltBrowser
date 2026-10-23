// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_text.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_canvas_text_align.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_canvas_text_baseline.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_text_cluster_options.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/geometry/dom_rect_read_only.h"
#include "third_party/blink/renderer/core/html/canvas/text_cluster.h"
#include "third_party/blink/renderer/core/html/canvas/text_metrics.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_text_values.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/fonts/font.h"
#include "third_party/blink/renderer/platform/fonts/font_platform_data.h"
#include "third_party/blink/renderer/platform/fonts/plain_text_painter.h"
#include "third_party/blink/renderer/platform/fonts/simple_font_data.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/testing/font_test_helpers.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/skia/include/core/SkFont.h"

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include "third_party/blink/public/platform/linux/web_sandbox_support.h"
#include "third_party/blink/public/platform/web_font_render_style.h"
#endif

namespace blink::molt_fingerprinting {
namespace {

using Align = V8CanvasTextAlign::Enum;
using Baseline = V8CanvasTextBaseline::Enum;

constexpr std::array<Align, 5> kAligns = {Align::kStart, Align::kEnd,
                                          Align::kLeft, Align::kRight,
                                          Align::kCenter};
constexpr std::array<Baseline, 6> kBaselines = {
    Baseline::kTop,        Baseline::kHanging,     Baseline::kMiddle,
    Baseline::kAlphabetic, Baseline::kIdeographic, Baseline::kBottom};

moltnet::TextMetricsValues ValuesOf(const TextMetrics& metrics) {
  moltnet::TextMetricsValues v;
  v.width = metrics.width();
  v.actual_bounding_box_left = metrics.actualBoundingBoxLeft();
  v.actual_bounding_box_right = metrics.actualBoundingBoxRight();
  v.font_bounding_box_ascent = metrics.fontBoundingBoxAscent();
  v.font_bounding_box_descent = metrics.fontBoundingBoxDescent();
  v.actual_bounding_box_ascent = metrics.actualBoundingBoxAscent();
  v.actual_bounding_box_descent = metrics.actualBoundingBoxDescent();
  v.em_height_ascent = metrics.emHeightAscent();
  v.em_height_descent = metrics.emHeightDescent();
  v.alphabetic_baseline = metrics.alphabeticBaseline();
  v.hanging_baseline = metrics.hangingBaseline();
  v.ideographic_baseline = metrics.ideographicBaseline();
  return v;
}

std::array<double, 12> Fields(const moltnet::TextMetricsValues& v) {
  return {v.width,
          v.actual_bounding_box_left,
          v.actual_bounding_box_right,
          v.font_bounding_box_ascent,
          v.font_bounding_box_descent,
          v.actual_bounding_box_ascent,
          v.actual_bounding_box_descent,
          v.em_height_ascent,
          v.em_height_descent,
          v.alphabetic_baseline,
          v.hanging_baseline,
          v.ideographic_baseline};
}

// The frame TextMetrics::Update() measured left-to-right text in, from
// public API: its alignment point and textBaseline offset.
moltnet::TextMetricsFrame FrameOf(const TextMetrics& metrics,
                                  Align align,
                                  Baseline baseline) {
  moltnet::TextMetricsFrame frame;
  const float width = static_cast<float>(metrics.width());
  if (align == Align::kCenter) {
    frame.align = moltnet::TextAlignPoint::kCenter;
    frame.align_dx = width / 2.0f;
  } else if (align == Align::kRight || align == Align::kEnd) {
    frame.align = moltnet::TextAlignPoint::kRight;
    frame.align_dx = width;
  }
  frame.baseline =
      TextMetrics::GetFontBaseline(baseline, *metrics.GetFont()->PrimaryFont());
  return frame;
}

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet is simulated with
// SetUniformTextMetricsForTesting() on top of kFarble.
class MoltNetTextTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetUniformTextMetricsForTesting(false);
    holder_ = MakeGarbageCollected<FontHolder>();
  }
  void TearDown() override {
    SetUniformTextMetricsForTesting(false);
    FontPlatformData::SetUniformTextRendering({});
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
    SetUniformTextMetricsForTesting(true);
  }

  const Font* Roboto(float size) {
    holder_->fonts.push_back(test::CreateTestFont(
        AtomicString("Roboto"),
        test::PlatformTestDataPath("third_party/Roboto/roboto-regular.woff2"),
        size));
    return holder_->fonts.back();
  }

  TextMetrics* Measure(const Font* font,
                       const String& text,
                       Align align = Align::kLeft,
                       Baseline baseline = Baseline::kAlphabetic) {
    return MakeGarbageCollected<TextMetrics>(
        font, TextDirection::kLtr, baseline, align, text,
        *MakeGarbageCollected<PlainTextPainter>(PlainTextPainter::kCanvas));
  }

  class FontHolder : public GarbageCollected<FontHolder> {
   public:
    void Trace(Visitor* visitor) const { visitor->Trace(fonts); }
    HeapVector<Member<const Font>> fonts;
  };

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
  Persistent<FontHolder> holder_;
};

constexpr char kText[] = "MoltNet fjord Wave 0123 éß";

// In MoltNet web content measureText() reports exactly what the rounding
// model gives for the exact metrics, whatever textAlign and textBaseline.
TEST_F(MoltNetTextTest, MeasureTextIsRoundedInMoltNet) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://site.example/");
  const Font* font = Roboto(16);
  for (Align align : kAligns) {
    for (Baseline baseline : kBaselines) {
      TextMetrics* metrics = Measure(font, String::FromUtf8(kText), align,
                                     baseline);
      moltnet::TextMetricsValues expected = ValuesOf(*metrics);
      moltnet::TextMetricsFrame frame = FrameOf(*metrics, align, baseline);
      moltnet::RoundTextMetrics(1, expected, frame);

      UniformTextMetrics(&window, *metrics);
      EXPECT_EQ(TextMetricsAccess::Grid(*metrics), 1);
      EXPECT_EQ(Fields(ValuesOf(*metrics)), Fields(expected))
          << static_cast<int>(align) << " " << static_cast<int>(baseline);
      for (double value : Fields(ValuesOf(*metrics))) {
        // Whole pixels, or halves around a centred alignment point.
        EXPECT_EQ(std::fmod(value * 2, 1.0), 0.0) << value;
      }
    }
  }
}

TEST_F(MoltNetTextTest, LeftAlignedAlphabeticMetricsAreWholePixels) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://site.example/");
  TextMetrics* metrics = Measure(Roboto(16), String::FromUtf8(kText));
  UniformTextMetrics(&window, *metrics);
  for (double value : Fields(ValuesOf(*metrics))) {
    EXPECT_EQ(std::fmod(value, 1.0), 0.0) << value;
  }
}

// Large text is rounded to a 64th to a 128th of its size.
TEST_F(MoltNetTextTest, GridFollowsTheFontSize) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://site.example/");
  TextMetrics* metrics = Measure(Roboto(300), String::FromUtf8(kText));
  EXPECT_EQ(TextMetricsAccess::FontSize(*metrics), 300);
  UniformTextMetrics(&window, *metrics);
  EXPECT_EQ(TextMetricsAccess::Grid(*metrics), 4);
  for (double value : Fields(ValuesOf(*metrics))) {
    EXPECT_EQ(std::fmod(value, 4.0), 0.0) << value;
  }
}

// Regular and Incognito windows, internal pages and unprotected processes
// keep the exact metrics.
TEST_F(MoltNetTextTest, OtherContextsKeepExactMetrics) {
  const Font* font = Roboto(16);
  auto expect_exact = [&](ExecutionContext* context) {
    TextMetrics* metrics = Measure(font, String::FromUtf8(kText),
                                   Align::kCenter, Baseline::kMiddle);
    const auto exact = Fields(ValuesOf(*metrics));
    UniformTextMetrics(context, *metrics);
    EXPECT_EQ(Fields(ValuesOf(*metrics)), exact);
    EXPECT_EQ(TextMetricsAccess::Grid(*metrics), 0);
  };
  // Protection off.
  expect_exact(&Window("https://site.example/"));
  // Normal and Incognito windows.
  Configure(Mode::kFarble);
  expect_exact(&Window("https://site.example/"));
  // MoltNet, but not web content.
  SetUniformTextMetricsForTesting(true);
  expect_exact(&Window("chrome://settings/"));
  // ... while web content there is rounded.
  TextMetrics* metrics = Measure(font, String::FromUtf8(kText),
                                 Align::kCenter, Baseline::kMiddle);
  UniformTextMetrics(&Window("https://site.example/"), *metrics);
  EXPECT_EQ(TextMetricsAccess::Grid(*metrics), 1);
}

// A measurement without a context (fail closed) is rounded; one without a
// font (a frameless document's empty TextMetrics) is left alone.
TEST_F(MoltNetTextTest, NoContextIsRoundedAndNoFontIsLeftAlone) {
  SimulateMoltNet();
  TextMetrics* metrics = Measure(Roboto(16), String::FromUtf8(kText));
  UniformTextMetrics(nullptr, *metrics);
  EXPECT_EQ(TextMetricsAccess::Grid(*metrics), 1);

  TextMetrics* empty = MakeGarbageCollected<TextMetrics>();
  UniformTextMetrics(nullptr, *empty);
  EXPECT_EQ(TextMetricsAccess::Grid(*empty), 0);
  EXPECT_EQ(empty->width(), 0);
}

// Rounding twice (two hooks, or a page measuring the same object twice)
// changes nothing more.
TEST_F(MoltNetTextTest, RoundingIsIdempotent) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://site.example/");
  TextMetrics* metrics = Measure(Roboto(16), String::FromUtf8(kText),
                                 Align::kCenter, Baseline::kHanging);
  UniformTextMetrics(&window, *metrics);
  const auto once = Fields(ValuesOf(*metrics));
  UniformTextMetrics(&window, *metrics);
  EXPECT_EQ(Fields(ValuesOf(*metrics)), once);
}

// The ExtendedTextMetrics methods agree with the rounded metrics.
TEST_F(MoltNetTextTest, ExtendedTextMetricsAreRounded) {
  SimulateMoltNet();
  LocalDOMWindow& window = Window("https://site.example/");
  const String text = String::FromUtf8(kText);
  for (Align align : {Align::kLeft, Align::kCenter, Align::kRight}) {
    TextMetrics* metrics = Measure(Roboto(16), text, align,
                                   Baseline::kMiddle);
    UniformTextMetrics(&window, *metrics);
    NonThrowableExceptionState exception_state;

    // The whole text's selection is the rounded width, from the rounded
    // alignment point.
    const HeapVector<Member<DOMRectReadOnly>> rects =
        metrics->getSelectionRects(0, text.length(), exception_state);
    ASSERT_EQ(rects.size(), 1u);
    EXPECT_EQ(rects[0]->width(), metrics->width());
    const double share =
        align == Align::kCenter ? 0.5 : align == Align::kRight ? 1.0 : 0.0;
    EXPECT_EQ(rects[0]->x(), -share * metrics->width());
    EXPECT_EQ(rects[0]->y(), -metrics->fontBoundingBoxAscent());
    EXPECT_EQ(rects[0]->height(), metrics->fontBoundingBoxAscent() +
                                      metrics->fontBoundingBoxDescent());
    // Partial selections have rounded ends too.
    for (unsigned end = 1; end < text.length(); ++end) {
      for (const auto& rect :
           metrics->getSelectionRects(0, end, exception_state)) {
        EXPECT_EQ(std::fmod(rect->x() * 2, 1.0), 0.0);
        EXPECT_EQ(std::fmod(rect->width(), 1.0), 0.0);
      }
    }

    // The ink box of the whole text is on the grid and holds what
    // measureText() reports.
    DOMRectReadOnly* box =
        metrics->getActualBoundingBox(0, text.length(), exception_state);
    EXPECT_EQ(std::fmod(box->width(), 1.0), 0.0);
    EXPECT_EQ(std::fmod(box->height(), 1.0), 0.0);
    EXPECT_LE(box->x(), -metrics->actualBoundingBoxLeft() + 1);
    EXPECT_GE(box->x() + box->width(), metrics->actualBoundingBoxRight() - 1);

    // Offsets in one grid cell give one answer.
    for (double x = -metrics->actualBoundingBoxLeft();
         x < metrics->width(); x += 1) {
      EXPECT_EQ(metrics->getIndexFromOffset(x),
                metrics->getIndexFromOffset(x + 0.3))
          << x;
    }

    // Clusters sit at grid points (halves around a centred alignment).
    for (const auto& cluster : metrics->getTextClusters(nullptr)) {
      EXPECT_EQ(std::fmod(cluster->x() * 2, 1.0), 0.0) << cluster->x();
      EXPECT_EQ(std::fmod(cluster->y(), 1.0), 0.0) << cluster->y();
    }
  }
}

// Without the rounding the extended methods are exact as upstream.
TEST_F(MoltNetTextTest, ExtendedTextMetricsOutsideMoltNetAreExact) {
  Configure(Mode::kFarble);
  LocalDOMWindow& window = Window("https://site.example/");
  const String text = String::FromUtf8(kText);
  TextMetrics* metrics =
      Measure(Roboto(16), text, Align::kCenter, Baseline::kMiddle);
  TextMetrics* reference =
      Measure(Roboto(16), text, Align::kCenter, Baseline::kMiddle);
  UniformTextMetrics(&window, *metrics);
  NonThrowableExceptionState exception_state;
  EXPECT_EQ(
      metrics->getActualBoundingBox(0, text.length(), exception_state)->x(),
      reference->getActualBoundingBox(0, text.length(), exception_state)
          ->x());
  EXPECT_EQ(metrics->getIndexFromOffset(3.3),
            reference->getIndexFromOffset(3.3));
}

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
// MoltNet renderers lay every font out as for text-rendering:
// geometricPrecision: no hinting, subpixel positioning, linear metrics,
// whatever the machine's settings and scale factor.
TEST_F(MoltNetTextTest, MoltNetRenderersMeasureUnhinted) {
  const SimpleFontData* before = Roboto(16)->PrimaryFont();
  ASSERT_TRUE(before);
  const WebFontRenderStyle& upstream =
      before->PlatformData().GetFontRenderStyle();

  PinUniformTextRendering();
  EXPECT_TRUE(FontPlatformData::GetUniformTextRendering().geometric_precision);
  const SimpleFontData* pinned = Roboto(16)->PrimaryFont();
  ASSERT_TRUE(pinned);
  const WebFontRenderStyle& style = pinned->PlatformData().GetFontRenderStyle();
  EXPECT_EQ(style.use_anti_alias, 1);
  EXPECT_EQ(style.use_subpixel_positioning, 1);
  EXPECT_EQ(style.use_hinting, 0);
  EXPECT_EQ(style.hint_style, 0);
  const SkFont sk_font = pinned->PlatformData().CreateSkFont();
  EXPECT_TRUE(sk_font.isLinearMetrics());
  EXPECT_TRUE(sk_font.isSubpixel());
  EXPECT_EQ(sk_font.getHinting(), SkFontHinting::kNone);

  FontPlatformData::SetUniformTextRendering({});
  const SimpleFontData* after = Roboto(16)->PrimaryFont();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->PlatformData().GetFontRenderStyle(), upstream);
}

// A desktop whose fontconfig turns anti-aliasing off and hints fully.
class AliasedDesktop : public WebSandboxSupport {
 public:
  bool GetFallbackFontForCharacter(WebUChar32,
                                   const char*,
                                   gfx::FallbackFontData*) override {
    return false;
  }
  bool MatchFontByPostscriptNameOrFullFontName(const char*,
                                               gfx::FallbackFontData*) override {
    return false;
  }
  void GetWebFontRenderStyleForStrike(const char*,
                                      int,
                                      bool,
                                      bool,
                                      float,
                                      WebFontRenderStyle* style) override {
    style->use_anti_alias = 0;
    style->use_hinting = 1;
    style->hint_style = 3;  // full
    style->use_subpixel_positioning = 0;
  }
};

class AliasedDesktopPlatform : public TestingPlatformSupport {
 public:
  WebSandboxSupport* GetSandboxSupport() override { return &desktop_; }

 private:
  AliasedDesktop desktop_;
};

// The uniform style doesn't depend on the desktop's anti-aliasing: with it
// off, MoltNet renderers still measure (and draw) unhinted, anti-aliased.
TEST_F(MoltNetTextTest, MoltNetRenderersIgnoreTheDesktopsAntiAliasing) {
  const SimpleFontData* roboto = Roboto(16)->PrimaryFont();
  ASSERT_TRUE(roboto);
  const sk_sp<SkTypeface> typeface = roboto->PlatformData().TypefaceSp();
  ScopedTestingPlatformSupport<AliasedDesktopPlatform> platform;
  // A system font (one with a family name) asks the desktop.
  auto style_now = [&typeface] {
    return MakeGarbageCollected<FontPlatformData>(
               typeface, "Roboto", 16, /*synthetic_bold=*/false,
               /*synthetic_italic=*/false,
               TextRenderingMode::kAutoTextRendering, ResolvedFontFeatures())
        ->GetFontRenderStyle();
  };

  const WebFontRenderStyle desktop = style_now();
  EXPECT_EQ(desktop.use_anti_alias, 0);
  EXPECT_EQ(desktop.use_hinting, 1);

  PinUniformTextRendering();
  const WebFontRenderStyle style = style_now();
  EXPECT_EQ(style.use_anti_alias, 1);
  EXPECT_EQ(style.use_hinting, 0);
  EXPECT_EQ(style.hint_style, 0);
  EXPECT_EQ(style.use_subpixel_positioning, 1);
}

TEST_F(MoltNetTextTest, EmojiFamilyOnlyWhenInstalled) {
  FontDescription description;
  description.SetComputedSize(16);
  FontPlatformData::UniformTextRendering rendering;
  rendering.emoji_family = "MoltBrowser No Such Emoji Font";
  FontPlatformData::SetUniformTextRendering(rendering);
  EXPECT_EQ(FontPlatformData::UniformEmojiFontData(description), nullptr);

  PinUniformTextRendering();
  ASSERT_NE(FontPlatformData::GetUniformTextRendering().emoji_family,
            nullptr);
  if (const SimpleFontData* emoji =
          FontPlatformData::UniformEmojiFontData(description)) {
    EXPECT_EQ(emoji->PlatformData().FontFamilyName(), "Noto Color Emoji");
  }

  FontPlatformData::SetUniformTextRendering({});
  EXPECT_EQ(FontPlatformData::UniformEmojiFontData(description), nullptr);
}
#endif

}  // namespace
}  // namespace blink::molt_fingerprinting
