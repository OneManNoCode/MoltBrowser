// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_vision.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace agent_vision {
namespace {

using agent_actions::Observation;
using agent_actions::ObservedElement;
using agent_safety::FieldKind;

// ---------------------------------------------------------------------------
// DecideCapture
// ---------------------------------------------------------------------------

CaptureFacts Allowed() {
  CaptureFacts f;
  f.switched_on = true;
  f.run_allows_images = true;
  f.model_sees_images = true;
  f.on_device_model = true;
  f.in_moltnet = false;
  f.tab_visible = true;
  f.same_document = true;
  f.error_document = false;
  f.url_scheme = "https";
  f.committed_origin = "https://shop.example";
  f.observed_origin = "https://shop.example";
  f.origin_allowed = true;
  f.cross_origin_frames = 0;
  f.frames_settled = true;
  return f;
}

TEST(AgentVisionCaptureTest, CapturesTheObservedWebPage) {
  EXPECT_EQ(DecideCapture(Allowed()), CaptureDecision::kCapture);
  CaptureFacts http = Allowed();
  http.url_scheme = "http";
  http.committed_origin = http.observed_origin = "http://shop.example:8080";
  EXPECT_EQ(DecideCapture(http), CaptureDecision::kCapture);
}

TEST(AgentVisionCaptureTest, EachConditionRefusesOnItsOwn) {
  struct Case {
    void (*change)(CaptureFacts&);
    CaptureDecision expected;
  } cases[] = {
      {[](CaptureFacts& f) { f.switched_on = false; },
       CaptureDecision::kSwitchedOff},
      {[](CaptureFacts& f) { f.run_allows_images = false; },
       CaptureDecision::kRunOff},
      {[](CaptureFacts& f) { f.model_sees_images = false; },
       CaptureDecision::kNoVision},
      {[](CaptureFacts& f) { f.tab_visible = false; },
       CaptureDecision::kTabHidden},
      {[](CaptureFacts& f) { f.same_document = false; },
       CaptureDecision::kDocumentChanged},
      {[](CaptureFacts& f) { f.error_document = true; },
       CaptureDecision::kErrorPage},
      {[](CaptureFacts& f) { f.observed_origin = "https://evil.example"; },
       CaptureDecision::kOriginChanged},
      {[](CaptureFacts& f) { f.observed_origin = ""; },
       CaptureDecision::kOriginChanged},
      {[](CaptureFacts& f) { f.origin_allowed = false; },
       CaptureDecision::kOriginNotAllowed},
      {[](CaptureFacts& f) { f.cross_origin_frames = 1; },
       CaptureDecision::kCrossOriginFrame},
      {[](CaptureFacts& f) { f.frames_settled = false; },
       CaptureDecision::kFramesChanging},
  };
  for (const Case& c : cases) {
    CaptureFacts f = Allowed();
    c.change(f);
    EXPECT_EQ(DecideCapture(f), c.expected) << CaptureDecisionName(c.expected);
  }
}

// A mask covers only a frame's box, and the page decides how the frame is
// painted: a frame of another origin anywhere in the page (the browser's
// count) means no screenshot, wherever it is and whatever else holds; so
// does a frame that navigated or went away just before or during the
// capture.
TEST(AgentVisionCaptureTest, FramesOfAnotherOriginMeanNoScreenshot) {
  for (size_t frames : {size_t{1}, size_t{2}, size_t{1000}}) {
    CaptureFacts f = Allowed();
    f.cross_origin_frames = frames;
    EXPECT_EQ(DecideCapture(f), CaptureDecision::kCrossOriginFrame) << frames;
    f.in_moltnet = true;
    EXPECT_EQ(DecideCapture(f), CaptureDecision::kCrossOriginFrame) << frames;
  }
  // The defaults refuse: facts nobody filled in never capture.
  CaptureFacts unfilled = Allowed();
  unfilled.frames_settled = CaptureFacts().frames_settled;
  EXPECT_NE(DecideCapture(unfilled), CaptureDecision::kCapture);
  // Every decision has a name for the logs.
  for (CaptureDecision d :
       {CaptureDecision::kCrossOriginFrame, CaptureDecision::kFramesChanging}) {
    EXPECT_FALSE(CaptureDecisionName(d).empty());
  }
}

TEST(AgentVisionCaptureTest, OnlyHttpAndHttpsPages) {
  // molt:// and chrome:// pages (settings, the vault, the agent studio),
  // extensions, local files and every other scheme, whatever the origin.
  for (const char* scheme :
       {"chrome", "molt", "chrome-untrusted", "chrome-extension", "devtools",
        "file", "data", "blob", "filesystem", "about", "javascript",
        "view-source", "chrome-error", "ftp", "ws", "wss", "HTTPS", ""}) {
    CaptureFacts f = Allowed();
    f.url_scheme = scheme;
    EXPECT_EQ(DecideCapture(f), CaptureDecision::kNotWebPage) << scheme;
  }
  // An http(s) URL whose document has another origin: opaque (a sandboxed
  // or data: document), or a non-web one.
  for (const char* origin :
       {"null", "", "chrome://settings", "molt://ai-agent",
        "chrome-extension://abcdefghijklmnop", "file://", "http://", "https://",
        "xhttps://shop.example", " https://shop.example"}) {
    CaptureFacts f = Allowed();
    f.committed_origin = f.observed_origin = origin;
    EXPECT_EQ(DecideCapture(f), CaptureDecision::kNotWebPage) << origin;
  }
}

TEST(AgentVisionCaptureTest, MoltNetCapturesOnlyForTheOnDeviceModel) {
  // The agent runs in MoltNet windows with the on-device model; the pixels
  // stay on the device.
  CaptureFacts moltnet = Allowed();
  moltnet.in_moltnet = true;
  EXPECT_EQ(DecideCapture(moltnet), CaptureDecision::kCapture);
  // Anything else would leave the device: cloud AI is off in MoltNet.
  moltnet.on_device_model = false;
  EXPECT_EQ(DecideCapture(moltnet), CaptureDecision::kMoltNetCloud);
  // Elsewhere the screenshot would go where the page text goes.
  CaptureFacts regular = Allowed();
  regular.on_device_model = false;
  EXPECT_EQ(DecideCapture(regular), CaptureDecision::kCapture);
  // MoltNet doesn't loosen any other check.
  moltnet = Allowed();
  moltnet.in_moltnet = true;
  moltnet.url_scheme = "chrome";
  EXPECT_EQ(DecideCapture(moltnet), CaptureDecision::kNotWebPage);
  moltnet = Allowed();
  moltnet.in_moltnet = true;
  moltnet.same_document = false;
  EXPECT_EQ(DecideCapture(moltnet), CaptureDecision::kDocumentChanged);
}

TEST(AgentVisionCaptureTest, TheSwitchWinsOverEverything) {
  CaptureFacts f;  // everything false or empty
  EXPECT_EQ(DecideCapture(f), CaptureDecision::kSwitchedOff);
  f = Allowed();
  f.switched_on = false;
  f.model_sees_images = false;
  f.url_scheme = "chrome";
  EXPECT_EQ(DecideCapture(f), CaptureDecision::kSwitchedOff);
  for (int d = 0; d <= static_cast<int>(CaptureDecision::kOriginNotAllowed);
       ++d) {
    EXPECT_FALSE(CaptureDecisionName(static_cast<CaptureDecision>(d)).empty());
  }
}

// ---------------------------------------------------------------------------
// The geometry script and its parser
// ---------------------------------------------------------------------------

ObservedElement Element(std::string ref,
                        std::string selector,
                        FieldKind kind = FieldKind::kOrdinary) {
  ObservedElement e;
  e.ref = std::move(ref);
  e.selector = std::move(selector);
  e.kind = "button";
  e.field_kind = kind;
  return e;
}

std::optional<ScreenshotGeometry> Parse(std::string_view json,
                                        size_t elements) {
  std::optional<base::Value> v = base::JSONReader::Read(json);
  EXPECT_TRUE(v.has_value()) << json;
  if (!v)
    return std::nullopt;
  return ParseGeometry(*v, elements);
}

// A result with two boxes (the second element out of view), one mask and
// two frame elements (one of them out of view).
constexpr char kGeometry[] =
    R"({"origin":"https://shop.example",
        "view":{"w":1280,"h":720,"sx":0,"sy":1500,"zoomed":false},
        "boxes":[[100,50,200,40],null],
        "masks":[[10,600,300,100]],
        "frames":2,
        "over":false})";

TEST(AgentVisionGeometryTest, ScriptCarriesSelectorsAsJson) {
  Observation o;
  o.elements.push_back(Element("e1", "#a"));
  o.elements.push_back(
      Element("e2", "#x\\\"]'); alert(1); //</script>\xE2\x80\xA8"));
  const std::string script = BuildGeometryScript(o);
  EXPECT_NE(script.find("const sels = [\"#a\",\"#x\\\\\\\"]'); alert(1); "
                        "//\\u003C/script>\\u2028\"];"),
            std::string::npos)
      << script;
  EXPECT_NE(script.find("MAX_MASKS = 32"), std::string::npos);
  EXPECT_EQ(script.find("%s"), std::string::npos);
  // No elements: an empty list, still a complete script.
  const std::string empty = BuildGeometryScript(Observation());
  EXPECT_NE(empty.find("const sels = [];"), std::string::npos);
  EXPECT_EQ(empty.rfind("(() => {", 0), 0u);
}

TEST(AgentVisionGeometryTest, ParsesAResult) {
  std::optional<ScreenshotGeometry> g = Parse(kGeometry, 2);
  ASSERT_TRUE(g);
  EXPECT_EQ(g->origin, "https://shop.example");
  EXPECT_EQ(g->viewport_width, 1280);
  EXPECT_EQ(g->viewport_height, 720);
  EXPECT_EQ(g->scroll_x, 0);
  EXPECT_EQ(g->scroll_y, 1500);
  ASSERT_EQ(g->boxes.size(), 2u);
  EXPECT_EQ(g->boxes[0], (Box{100, 50, 200, 40}));
  EXPECT_FALSE(g->boxes[1]);
  ASSERT_EQ(g->masks.size(), 1u);
  EXPECT_EQ(g->masks[0], (Box{10, 600, 300, 100}));
  EXPECT_EQ(g->frame_elements, 2u);
  // Whole numbers written as doubles are fine; negative positions too
  // (partly above or left of the viewport).
  g = Parse(R"({"origin":"https://a.example",
                "view":{"w":1280.0,"h":720,"sx":-3,"sy":0,"zoomed":false},
                "boxes":[[-20.0,-5,40,10]],"masks":[],"frames":0.0,
                "over":false})",
            1);
  ASSERT_TRUE(g);
  EXPECT_EQ(g->viewport_width, 1280);
  EXPECT_EQ(g->boxes[0], (Box{-20, -5, 40, 10}));
  EXPECT_EQ(g->frame_elements, 0u);
  // The cap and the coordinate range, at the limit.
  std::string masks;
  for (size_t i = 0; i < kMaxMasks; ++i)
    masks += std::string(i ? "," : "") + "[0,0,1000000,1]";
  g = Parse(R"({"origin":"https://a.example",
                "view":{"w":16384,"h":1,"sx":1000000,"sy":-1000000,
                        "zoomed":false},
                "boxes":[],"masks":[)" +
                masks + R"(],"frames":50000,"over":false})",
            0);
  ASSERT_TRUE(g);
  EXPECT_EQ(g->masks.size(), kMaxMasks);
  EXPECT_EQ(g->frame_elements, static_cast<size_t>(kMaxGeometryWalk));
}

// The browser compares the frames the document owns with the frame
// elements the script found: one it didn't find (inside a closed shadow
// root) would be drawn unmasked.
TEST(AgentVisionGeometryTest, EveryFrameMustBeFound) {
  const ScreenshotGeometry g = *Parse(kGeometry, 2);
  EXPECT_TRUE(FoundEveryFrame(g, 0));
  EXPECT_TRUE(FoundEveryFrame(g, 1));  // an <object> showing an image
  EXPECT_TRUE(FoundEveryFrame(g, 2));
  EXPECT_FALSE(FoundEveryFrame(g, 3));
  ScreenshotGeometry none = g;
  none.frame_elements = 0;
  EXPECT_TRUE(FoundEveryFrame(none, 0));
  EXPECT_FALSE(FoundEveryFrame(none, 1));

  // The script counts frames wherever it walks, open shadow roots
  // included, in view or not.
  const std::string script = BuildGeometryScript(Observation());
  EXPECT_NE(script.find("if (FRAMES.includes(el.localName)) frames++;"),
            std::string::npos);
  EXPECT_NE(script.find("frames: frames"), std::string::npos);
}

TEST(AgentVisionGeometryTest, RefusesAnythingOff) {
  const std::string base = kGeometry;
  auto with = [&](std::string_view from, std::string_view to) {
    std::string s = base;
    const size_t pos = s.find(from);
    EXPECT_NE(pos, std::string::npos) << from;
    if (pos != std::string::npos)
      s.replace(pos, from.size(), to);
    return s;
  };
  const std::string bad[] = {
      // The script gave up (more to mask than the cap, or an exception).
      with(R"("over":false)", R"("over":true)"),
      with(R"("over":false)", R"("overflow":false)"),
      with(R"("over":false)", R"("over":0)"),
      // Origin.
      with(R"("origin":"https://shop.example",)", ""),
      with(R"("https://shop.example")", R"("")"),
      with(R"("https://shop.example")", R"("https://a\u0000.example")"),
      with(R"("https://shop.example")",
           "\"https://" + std::string(510, 'a') + "\""),
      // Pinch zoom, and its flag missing.
      with(R"("zoomed":false)", R"("zoomed":true)"),
      with(R"(,"zoomed":false)", ""),
      // The viewport.
      with(R"("w":1280)", R"("w":0)"),
      with(R"("w":1280)", R"("w":16385)"),
      with(R"("w":1280)", R"("w":1280.5)"),
      with(R"("w":1280)", R"("w":"1280")"),
      with(R"("h":720)", R"("h":-720)"),
      with(R"("sy":1500)", R"("sy":1000001)"),
      with(R"(,"sx":0)", ""),
      with(R"("view":{"w":1280,"h":720,"sx":0,"sy":1500,"zoomed":false},)", ""),
      // Boxes: one per element, each null or four whole numbers in range.
      with(R"([[100,50,200,40],null])", R"([[100,50,200,40]])"),
      with(R"([[100,50,200,40],null])", R"([[100,50,200,40],null,null])"),
      with(R"([100,50,200,40])", R"([100,50,200])"),
      with(R"([100,50,200,40])", R"([100,50,200,40,1])"),
      with(R"([100,50,200,40])", R"([100,50,0,40])"),
      with(R"([100,50,200,40])", R"([100,50,200,-40])"),
      with(R"([100,50,200,40])", R"([100.5,50,200,40])"),
      with(R"([100,50,200,40])", R"([1000001,50,200,40])"),
      with(R"([100,50,200,40])", R"(["100",50,200,40])"),
      with(R"([100,50,200,40])", R"({"x":100})"),
      with(R"("boxes":[[100,50,200,40],null],)", ""),
      // Masks: every one readable (one that isn't can't be applied).
      with(R"([[10,600,300,100]])", R"([[10,600,300,100],null])"),
      with(R"([[10,600,300,100]])", R"([[10,600,300]])"),
      with(R"([[10,600,300,100]])", R"([[10,600,300,0]])"),
      with(R"("masks":[[10,600,300,100]],)", ""),
      // The frame count: required, a whole number within the walk.
      with(R"("frames":2,)", ""),
      with(R"("frames":2)", R"("frames":-1)"),
      with(R"("frames":2)", R"("frames":2.5)"),
      with(R"("frames":2)", R"("frames":"2")"),
      with(R"("frames":2)", R"("frames":null)"),
      with(R"("frames":2)", R"("frames":50001)"),
  };
  for (const std::string& json : bad)
    EXPECT_FALSE(Parse(json, 2)) << json;

  std::string masks;
  for (size_t i = 0; i <= kMaxMasks; ++i)
    masks += std::string(i ? "," : "") + "[0,0,1,1]";
  EXPECT_FALSE(Parse(with(R"([[10,600,300,100]])", "[" + masks + "]"), 2));

  EXPECT_FALSE(ParseGeometry(base::Value(), 0));
  EXPECT_FALSE(ParseGeometry(base::Value("geometry"), 0));
  base::ListValue list;
  EXPECT_FALSE(ParseGeometry(base::Value(std::move(list)), 0));
}

TEST(AgentVisionGeometryTest, SameGeometryAllowsOnePixelOfRounding) {
  const ScreenshotGeometry before = *Parse(kGeometry, 2);
  EXPECT_TRUE(SameGeometry(before, before));

  ScreenshotGeometry after = before;
  after.boxes[0]->x += 1;
  after.boxes[0]->height -= 1;
  after.masks[0].y -= 1;
  EXPECT_TRUE(SameGeometry(before, after));

  auto expect_differs = [&](void (*change)(ScreenshotGeometry&)) {
    ScreenshotGeometry g = before;
    change(g);
    EXPECT_FALSE(SameGeometry(before, g));
    EXPECT_FALSE(SameGeometry(g, before));
  };
  expect_differs([](ScreenshotGeometry& g) { g.boxes[0]->x += 2; });
  expect_differs([](ScreenshotGeometry& g) { g.boxes[0]->width -= 2; });
  expect_differs([](ScreenshotGeometry& g) { g.boxes[0] = std::nullopt; });
  expect_differs([](ScreenshotGeometry& g) { g.boxes[1] = Box{0, 0, 5, 5}; });
  expect_differs([](ScreenshotGeometry& g) { g.boxes.pop_back(); });
  expect_differs([](ScreenshotGeometry& g) { g.masks[0].height += 2; });
  expect_differs([](ScreenshotGeometry& g) { g.masks.clear(); });
  expect_differs(
      [](ScreenshotGeometry& g) { g.masks.push_back(Box{0, 0, 1, 1}); });
  expect_differs([](ScreenshotGeometry& g) { g.scroll_y += 1; });
  expect_differs([](ScreenshotGeometry& g) { g.scroll_x -= 1; });
  expect_differs([](ScreenshotGeometry& g) { g.viewport_width += 1; });
  expect_differs([](ScreenshotGeometry& g) { g.viewport_height -= 1; });
  expect_differs([](ScreenshotGeometry& g) { g.frame_elements += 1; });
  expect_differs(
      [](ScreenshotGeometry& g) { g.origin = "https://evil.example"; });
}

// ---------------------------------------------------------------------------
// From the viewport to the model image
// ---------------------------------------------------------------------------

TEST(AgentVisionMappingTest, CaptureSize) {
  struct Case {
    int w, h, out_w, out_h;
  } cases[] = {
      {1280, 720, 1280, 720},     {1536, 864, 1536, 864},
      {3840, 2160, 1536, 864},    {1000, 3000, 512, 1536},
      {1, 5000, 1, 1536},         {5000, 1, 1536, 1},
      {16384, 16384, 1536, 1536},
  };
  for (const Case& c : cases) {
    std::optional<CaptureSize> s = CaptureSizeFor(c.w, c.h);
    ASSERT_TRUE(s) << c.w << "x" << c.h;
    EXPECT_EQ(s->width, c.out_w) << c.w << "x" << c.h;
    EXPECT_EQ(s->height, c.out_h) << c.w << "x" << c.h;
  }
  for (auto [w, h] : {std::pair{0, 720}, std::pair{1280, 0}, std::pair{-1, 720},
                      std::pair{16385, 10}, std::pair{10, 16385}}) {
    EXPECT_FALSE(CaptureSizeFor(w, h)) << w << "x" << h;
  }
}

ImagePlan PlanFor(uint32_t w, uint32_t h) {
  return *PlanModelImage(w, h, kScreenshotMaxSide);
}

TEST(AgentVisionMappingTest, MapsTheViewportOntoTheContent) {
  const ImagePlan plan = PlanFor(1280, 720);
  std::optional<ImageMapping> m = MapViewport(1280, 720, 1280, 720, plan);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale_x, 0.6);
  EXPECT_DOUBLE_EQ(m->scale_y, 0.6);
  EXPECT_EQ(m->content, (Rect{0, 0, 768, 432}));

  // Zoomed in to 200%: 640 x 360 CSS pixels in the same capture.
  m = MapViewport(640, 360, 1280, 720, plan);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale_x, 1.2);

  // A very tall view: the content is padded to 8:1, off-centre on x.
  const ImagePlan tall = PlanFor(150, 1536);
  ASSERT_GT(tall.offset_x, 0u);
  m = MapViewport(300, 3072, 150, 1536, tall);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->content.x, static_cast<int>(tall.offset_x));
  EXPECT_EQ(m->content.width, static_cast<int>(tall.content_width));
}

TEST(AgentVisionMappingTest, RefusesACaptureOfAnotherShape) {
  const ImagePlan plan = PlanFor(1280, 720);
  // Within 2%: rounding and scroll bars.
  EXPECT_TRUE(MapViewport(1280, 725, 1280, 720, plan));
  EXPECT_TRUE(MapViewport(1295, 720, 1280, 720, plan));
  // The view was resized between the measure and the copy.
  EXPECT_FALSE(MapViewport(1280, 800, 1280, 720, plan));
  EXPECT_FALSE(MapViewport(1000, 720, 1280, 720, plan));
  EXPECT_FALSE(MapViewport(0, 720, 1280, 720, plan));
  EXPECT_FALSE(MapViewport(1280, 720, 0, 720, plan));
  ImagePlan broken = plan;
  broken.offset_x = 1;  // content would end past the canvas
  EXPECT_FALSE(MapViewport(1280, 720, 1280, 720, broken));
  broken = plan;
  broken.content_height = 0;
  EXPECT_FALSE(MapViewport(1280, 720, 1280, 720, broken));
}

TEST(AgentVisionMappingTest, MapBoxRoundsOutwardsAndClips) {
  ImageMapping m;
  m.scale_x = m.scale_y = 0.6;
  m.content = Rect{0, 0, 768, 432};
  EXPECT_EQ(MapBox(Box{100, 50, 200, 40}, m, 0), (Rect{60, 30, 120, 24}));
  // A 1-pixel box at a fractional position covers both partial pixels.
  EXPECT_EQ(MapBox(Box{1, 1, 1, 1}, m, 0), (Rect{0, 0, 2, 2}));
  EXPECT_EQ(MapBox(Box{100, 50, 200, 40}, m, kMaskMargin),
            (Rect{60 - kMaskMargin, 30 - kMaskMargin, 120 + 2 * kMaskMargin,
                  24 + 2 * kMaskMargin}));
  // Clipped at the edges, margin included.
  EXPECT_EQ(MapBox(Box{0, 0, 10, 10}, m, kMaskMargin),
            (Rect{0, 0, 6 + kMaskMargin, 6 + kMaskMargin}));
  EXPECT_EQ(MapBox(Box{-50, 10, 100, 20}, m, 0), (Rect{0, 6, 30, 12}));
  EXPECT_EQ(MapBox(Box{1200, 700, 500, 500}, m, 0), (Rect{720, 420, 48, 12}));
  // Off screen: nothing.
  EXPECT_TRUE(MapBox(Box{-500, -500, 100, 100}, m, 0).IsEmpty());
  EXPECT_TRUE(MapBox(Box{1280, 0, 50, 50}, m, 0).IsEmpty());
  EXPECT_TRUE(MapBox(Box{0, 720, 50, 50}, m, 0).IsEmpty());
  EXPECT_TRUE(MapBox(Box{-kMaxCoordinate, -kMaxCoordinate, kMaxCoordinate,
                         kMaxCoordinate},
                     m, 0)
                  .IsEmpty());
  // Huge boxes clip to the content without overflow.
  EXPECT_EQ(MapBox(Box{-kMaxCoordinate, -kMaxCoordinate, 2 * kMaxCoordinate,
                       2 * kMaxCoordinate},
                   m, kMaskMargin),
            m.content);
  // Placed on the canvas by the content's offset.
  m.content = Rect{9, 0, 77, 768};
  m.scale_x = m.scale_y = 0.25;
  EXPECT_EQ(MapBox(Box{40, 40, 40, 40}, m, 0), (Rect{19, 10, 10, 10}));
  EXPECT_EQ(MapBox(Box{0, 0, 1000, 8}, m, 0), (Rect{9, 0, 77, 2}));
  // Empty boxes (never from the parser) map to nothing.
  EXPECT_TRUE(MapBox(Box{10, 10, 0, 5}, m, kMaskMargin).IsEmpty());
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

bool RectsOverlap(const Rect& a, const Rect& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
         b.y < a.y + a.height;
}

bool Inside(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.width <= outer.x + outer.width &&
         inner.y + inner.height <= outer.y + outer.height;
}

// The tag touches or overlaps its target.
bool Touches(const Rect& tag, const Rect& target) {
  return RectsOverlap(Rect{tag.x - 1, tag.y - 1, tag.width + 2, tag.height + 2},
                      target);
}

TEST(AgentVisionTagTest, TagSizes) {
  EXPECT_EQ(TagSize("e7"), (Rect{0, 0, 26, 18}));
  EXPECT_EQ(TagSize("e40"), (Rect{0, 0, 38, 18}));
  EXPECT_EQ(TagSize("e999"), (Rect{0, 0, 50, 18}));
  for (const char* bad :
       {"", "e", "7", "x1", "E1", "e1a", "e-1", "e1234", "e 1", "e\xD9\xA1"}) {
    EXPECT_TRUE(TagSize(bad).IsEmpty()) << bad;
  }
}

TEST(AgentVisionTagTest, LargeElementsInsideSmallOnesBeside) {
  const Rect bounds{0, 0, 768, 432};
  // A large element: the tag in its top-left corner.
  std::vector<Tag> tags =
      LayoutTags({{"e1", Rect{100, 100, 200, 100}}}, bounds);
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0].label, (Rect{100, 100, 26, 18}));
  EXPECT_EQ(tags[0].target, (Rect{100, 100, 200, 100}));
  EXPECT_EQ(tags[0].ref, "e1");
  // A small one (an icon button): above it, not over it.
  tags = LayoutTags({{"e2", Rect{100, 100, 24, 24}}}, bounds);
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0].label, (Rect{100, 82, 26, 18}));
  // At the top edge: below.
  tags = LayoutTags({{"e3", Rect{100, 0, 24, 24}}}, bounds);
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0].label, (Rect{100, 24, 26, 18}));
  // At the right edge: slid left to stay in bounds, still above it.
  tags = LayoutTags({{"e4", Rect{760, 100, 8, 8}}}, bounds);
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0].label, (Rect{742, 82, 26, 18}));
  // Filling the screenshot's height: beside it.
  tags = LayoutTags({{"e5", Rect{100, 0, 30, 432}}}, bounds);
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0].label, (Rect{74, 0, 26, 18}));
  // The bounds are the content on a padded canvas.
  const Rect content{9, 0, 77, 768};
  tags = LayoutTags({{"e6", Rect{9, 0, 10, 10}}}, content);
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0].label, (Rect{9, 10, 26, 18}));
}

TEST(AgentVisionTagTest, TagsNeverOverlap) {
  const Rect bounds{0, 0, 768, 432};
  // Two icons side by side: the second tag can't go above (taken), so it
  // goes below.
  std::vector<Tag> tags = LayoutTags(
      {{"e1", Rect{100, 100, 24, 24}}, {"e2", Rect{110, 100, 24, 24}}}, bounds);
  ASSERT_EQ(tags.size(), 2u);
  EXPECT_EQ(tags[0].label, (Rect{100, 82, 26, 18}));
  EXPECT_EQ(tags[1].label, (Rect{110, 124, 26, 18}));
  // Six marks on one spot: four free places around it (inside would
  // overlap the tag on its right), the others get no tag.
  std::vector<Mark> crowd;
  for (int i = 1; i <= 6; ++i)
    crowd.push_back({"e" + base::NumberToString(i), Rect{300, 200, 24, 24}});
  tags = LayoutTags(crowd, bounds);
  EXPECT_EQ(tags.size(), 4u);
  for (size_t i = 0; i < tags.size(); ++i) {
    for (size_t j = i + 1; j < tags.size(); ++j)
      EXPECT_FALSE(RectsOverlap(tags[i].label, tags[j].label));
  }
}

TEST(AgentVisionTagTest, CapAndSkippedMarks) {
  const Rect bounds{0, 0, 1024, 1024};
  std::vector<Mark> marks;
  for (int i = 0; i < 100; ++i) {
    marks.push_back({"e" + base::NumberToString(i + 1),
                     Rect{(i % 10) * 100 + 10, (i / 10) * 100 + 30, 40, 20}});
  }
  std::vector<Tag> tags = LayoutTags(marks, bounds);
  ASSERT_EQ(tags.size(), kMaxTags);
  for (size_t i = 0; i < tags.size(); ++i)
    EXPECT_EQ(tags[i].ref, marks[i].ref);  // in order, the first ones

  // Marks that can't be tagged are skipped, not drawn wrong.
  tags = LayoutTags({{"x1", Rect{10, 30, 40, 20}},
                     {"e1", Rect{10, 30, 0, 20}},
                     {"e1000", Rect{10, 30, 40, 20}},
                     {"e2", Rect{10, 30, 40, 20}}},
                    bounds);
  ASSERT_EQ(tags.size(), 1u);
  EXPECT_EQ(tags[0].ref, "e2");
  // No room at all.
  EXPECT_TRUE(
      LayoutTags({{"e1", Rect{0, 0, 10, 10}}}, Rect{0, 0, 20, 10}).empty());
  EXPECT_TRUE(LayoutTags({{"e1", Rect{0, 0, 10, 10}}}, Rect()).empty());
}

TEST(AgentVisionTagTest, RandomLayoutsKeepTheInvariants) {
  uint32_t seed = 12345;
  auto next = [&](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<int>((seed >> 8) % n);
  };
  for (int round = 0; round < 2000; ++round) {
    const Rect bounds{next(40), next(40), 32 + next(800), 32 + next(800)};
    std::vector<Mark> marks;
    const int n = next(60);
    for (int i = 0; i < n; ++i) {
      // Targets inside the bounds, as MapBox makes them.
      const int x = bounds.x + next(bounds.width);
      const int y = bounds.y + next(bounds.height);
      const int w = 1 + next(bounds.x + bounds.width - x);
      const int h = 1 + next(bounds.y + bounds.height - y);
      marks.push_back({"e" + base::NumberToString(i + 1), Rect{x, y, w, h}});
    }
    const std::vector<Tag> tags = LayoutTags(marks, bounds);
    ASSERT_LE(tags.size(), kMaxTags);
    for (size_t i = 0; i < tags.size(); ++i) {
      ASSERT_TRUE(Inside(bounds, tags[i].label)) << round;
      ASSERT_TRUE(Touches(tags[i].label, tags[i].target)) << round;
      ASSERT_EQ(tags[i].label.width, TagSize(tags[i].ref).width);
      for (size_t j = i + 1; j < tags.size(); ++j)
        ASSERT_FALSE(RectsOverlap(tags[i].label, tags[j].label)) << round;
    }
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

constexpr uint8_t kPage[3] = {10, 200, 30};
constexpr uint8_t kSecret[3] = {1, 2, 3};

ModelImage Filled(uint32_t w, uint32_t h, const uint8_t rgb[3]) {
  ModelImage image;
  image.width = w;
  image.height = h;
  image.rgb.resize(size_t{w} * h * 3);
  for (size_t i = 0; i < image.rgb.size(); i += 3) {
    image.rgb[i] = rgb[0];
    image.rgb[i + 1] = rgb[1];
    image.rgb[i + 2] = rgb[2];
  }
  return image;
}

bool PixelIs(const ModelImage& image, int x, int y, const uint8_t rgb[3]) {
  const size_t i = (size_t{static_cast<uint32_t>(y)} * image.width +
                    static_cast<uint32_t>(x)) *
                   3;
  return image.rgb[i] == rgb[0] && image.rgb[i + 1] == rgb[1] &&
         image.rgb[i + 2] == rgb[2];
}

int CountPixels(const ModelImage& image, const Rect& r, const uint8_t rgb[3]) {
  int n = 0;
  for (int y = r.y; y < r.y + r.height; ++y) {
    for (int x = r.x; x < r.x + r.width; ++x)
      n += PixelIs(image, x, y, rgb);
  }
  return n;
}

const Rect kWhole{0, 0, 100, 60};

TEST(AgentVisionDrawTest, FillAndOutlineStayInTheirRects) {
  ModelImage image = Filled(100, 60, kPage);
  FillRect(image, Rect{90, 50, 30, 30}, kMaskRgb);  // clipped
  FillRect(image, Rect{-5, -5, 10, 10}, kMaskRgb);  // clipped
  FillRect(image, Rect{200, 200, 5, 5}, kMaskRgb);  // outside: nothing
  FillRect(image, Rect{10, 10, 0, 5}, kMaskRgb);    // empty: nothing
  EXPECT_EQ(CountPixels(image, kWhole, kMaskRgb), 10 * 10 + 5 * 5);
  EXPECT_EQ(CountPixels(image, Rect{90, 50, 10, 10}, kMaskRgb), 100);
  EXPECT_EQ(CountPixels(image, Rect{0, 0, 5, 5}, kMaskRgb), 25);

  ModelImage outline = Filled(100, 60, kPage);
  OutlineRect(outline, Rect{20, 20, 10, 6}, kTagRgb);
  EXPECT_EQ(CountPixels(outline, kWhole, kTagRgb), 2 * 10 + 2 * 4);
  EXPECT_EQ(CountPixels(outline, Rect{21, 21, 8, 4}, kTagRgb), 0);
  EXPECT_TRUE(PixelIs(outline, 20, 20, kTagRgb));
  EXPECT_TRUE(PixelIs(outline, 29, 25, kTagRgb));

  // A broken image (wrong buffer size) is left alone.
  ModelImage broken = Filled(10, 10, kPage);
  broken.rgb.pop_back();
  FillRect(broken, Rect{0, 0, 10, 10}, kMaskRgb);
  EXPECT_EQ(broken.rgb[0], kPage[0]);
}

TEST(AgentVisionDrawTest, DrawsTheTagText) {
  ModelImage image = Filled(100, 60, kPage);
  const Tag tag{"e1", Rect{10, 20, 26, 18}, Rect{10, 38, 30, 10}};
  DrawTags(image, {tag});
  // The label: tag colour around white text, nothing outside label and
  // outline.
  EXPECT_TRUE(PixelIs(image, 10, 20, kTagRgb));
  EXPECT_TRUE(PixelIs(image, 35, 37, kTagRgb));
  // 'e' row 0 is empty, row 2 is .###.: cell (1, 2) is lit.
  const int tx = 10 + kTagPadding;
  const int ty = 20 + kTagPadding;
  EXPECT_TRUE(PixelIs(image, tx, ty, kTagRgb));
  EXPECT_TRUE(
      PixelIs(image, tx + 1 * kGlyphScale, ty + 2 * kGlyphScale, kTagTextRgb));
  // '1' row 0 is ..#..: cell (2, 0) of the second glyph is lit.
  const int x1 = tx + 6 * kGlyphScale;
  EXPECT_TRUE(PixelIs(image, x1 + 2 * kGlyphScale, ty, kTagTextRgb));
  EXPECT_TRUE(PixelIs(image, x1, ty, kTagRgb));
  // 'e' has 14 lit cells, '1' 10.
  EXPECT_EQ(CountPixels(image, kWhole, kTagTextRgb),
            (14 + 10) * kGlyphScale * kGlyphScale);
  EXPECT_EQ(CountPixels(image, tag.label, kTagTextRgb),
            (14 + 10) * kGlyphScale * kGlyphScale);
  // The target's outline.
  EXPECT_TRUE(PixelIs(image, 39, 47, kTagRgb));
  EXPECT_TRUE(PixelIs(image, 20, 42, kPage));
  // Outlines are drawn before labels: a later outline doesn't cross an
  // earlier label.
  ModelImage two = Filled(100, 60, kPage);
  DrawTags(two, {tag, Tag{"e2", Rect{60, 0, 26, 18}, Rect{0, 25, 100, 10}}});
  EXPECT_EQ(CountPixels(two, tag.label, kTagTextRgb),
            (14 + 10) * kGlyphScale * kGlyphScale);
}

// ---------------------------------------------------------------------------
// AnnotateScreenshot
// ---------------------------------------------------------------------------

struct Scene {
  Observation observation;
  ScreenshotGeometry geometry;
};

// A 1280 x 720 viewport (0.6 model pixels per CSS pixel): e1 a link, e2 a
// password field, e3 a card number field, e4 an ordinary field, e5 a
// password field out of view; one frame to mask.
Scene MakeScene() {
  Scene s;
  s.observation.elements = {
      Element("e1", "#home"),
      Element("e2", "#pw", FieldKind::kPassword),
      Element("e3", "#card", FieldKind::kPaymentCard),
      Element("e4", "#name"),
      Element("e5", "#pw2", FieldKind::kPassword),
  };
  s.geometry.origin = "https://shop.example";
  s.geometry.viewport_width = 1280;
  s.geometry.viewport_height = 720;
  s.geometry.boxes = {Box{20, 20, 100, 20}, Box{200, 200, 300, 40},
                      Box{200, 300, 300, 40}, Box{200, 400, 300, 40},
                      std::nullopt};
  s.geometry.masks = {Box{800, 100, 400, 300}};
  return s;
}

// Paints |box| (CSS pixels, mapped like the real screenshot) in kSecret,
// with a fringe of |fringe| model pixels around it, as a scaler's bleed
// would leave.
void PaintSecret(ModelImage& image,
                 const ImageMapping& m,
                 const Box& box,
                 int fringe) {
  FillRect(image, MapBox(box, m, fringe), kSecret);
}

TEST(AgentVisionAnnotateTest, MasksEverySecretPixelAndTagsTheRest) {
  const Scene s = MakeScene();
  const ImagePlan plan = PlanFor(1280, 720);
  const ImageMapping m = *MapViewport(1280, 720, 1280, 720, plan);
  ModelImage image = Filled(plan.canvas_width, plan.canvas_height, kPage);
  // Secrets: the password field, the card field and the frame, each with a
  // 4-pixel fringe of scaler bleed.
  PaintSecret(image, m, *s.geometry.boxes[1], 4);
  PaintSecret(image, m, *s.geometry.boxes[2], 4);
  PaintSecret(image, m, s.geometry.masks[0], 4);
  ASSERT_GT(CountPixels(image, m.content, kSecret), 0);

  ASSERT_TRUE(
      AnnotateScreenshot(image, plan, 1280, 720, s.observation, s.geometry));
  // Not one secret pixel left, fringe included.
  EXPECT_EQ(CountPixels(image, m.content, kSecret), 0);
  // The masks are grey (bar the tags drawn on top).
  const Rect frame = MapBox(s.geometry.masks[0], m, kMaskMargin);
  EXPECT_EQ(CountPixels(image, frame, kMaskRgb), frame.width * frame.height);
  EXPECT_TRUE(PixelIs(image, frame.x - 1, frame.y + 10, kPage));
  const Rect pw = MapBox(*s.geometry.boxes[1], m, kMaskMargin);
  EXPECT_GT(CountPixels(image, pw, kMaskRgb), pw.width * pw.height / 2);
  // The ordinary field is not masked: page pixels inside it remain.
  const Rect name = MapBox(*s.geometry.boxes[3], m, 0);
  EXPECT_EQ(CountPixels(image, name, kMaskRgb), 0);
  EXPECT_GT(CountPixels(image, name, kPage), name.width * name.height / 2);
  // Four tags (e5 is out of view), each with its text.
  const int text_pixels = CountPixels(image, m.content, kTagTextRgb);
  EXPECT_GT(text_pixels, 0);
  EXPECT_EQ(text_pixels % (kGlyphScale * kGlyphScale), 0);
  // e1's tag: e1 is 100 x 20 CSS = 60 x 12 model pixels, small, so above
  // it would be off the top (y = 12 - 18): below it.
  EXPECT_TRUE(PixelIs(image, 12, 24, kTagRgb));
}

TEST(AgentVisionAnnotateTest, MasksStayOnTheScreenshot) {
  // A tall page padded on the canvas: masks and tags never touch the
  // padding.
  Scene s = MakeScene();
  s.geometry.viewport_width = 300;
  s.geometry.viewport_height = 3072;
  s.geometry.boxes = {std::nullopt, Box{-100, -100, 1000, 400}, std::nullopt,
                      std::nullopt, std::nullopt};
  s.geometry.masks = {Box{250, 3000, 500, 500}};
  const ImagePlan plan = PlanFor(150, 1536);
  ModelImage image = Filled(plan.canvas_width, plan.canvas_height, kPage);
  ASSERT_TRUE(
      AnnotateScreenshot(image, plan, 150, 1536, s.observation, s.geometry));
  const Rect content{static_cast<int>(plan.offset_x),
                     static_cast<int>(plan.offset_y),
                     static_cast<int>(plan.content_width),
                     static_cast<int>(plan.content_height)};
  int outside_changed = 0;
  for (int y = 0; y < static_cast<int>(image.height); ++y) {
    for (int x = 0; x < static_cast<int>(image.width); ++x) {
      const bool in = x >= content.x && x < content.x + content.width &&
                      y >= content.y && y < content.y + content.height;
      if (!in && !PixelIs(image, x, y, kPage))
        ++outside_changed;
    }
  }
  EXPECT_EQ(outside_changed, 0);
  EXPECT_GT(CountPixels(image, content, kMaskRgb), 0);
}

TEST(AgentVisionAnnotateTest, RefusesWhatDoesNotAddUp) {
  const Scene s = MakeScene();
  const ImagePlan plan = PlanFor(1280, 720);
  const ModelImage original =
      Filled(plan.canvas_width, plan.canvas_height, kPage);
  // An image that isn't the plan's canvas.
  ModelImage other = Filled(plan.canvas_width + 2, plan.canvas_height, kPage);
  EXPECT_FALSE(
      AnnotateScreenshot(other, plan, 1280, 720, s.observation, s.geometry));
  other = Filled(plan.canvas_width, plan.canvas_height + 2, kPage);
  EXPECT_FALSE(
      AnnotateScreenshot(other, plan, 1280, 720, s.observation, s.geometry));
  ModelImage broken = original;
  broken.rgb.pop_back();
  EXPECT_FALSE(
      AnnotateScreenshot(broken, plan, 1280, 720, s.observation, s.geometry));
  // A capture of another shape than the viewport measured.
  ModelImage image = original;
  EXPECT_FALSE(
      AnnotateScreenshot(image, plan, 1280, 900, s.observation, s.geometry));
  EXPECT_EQ(image.rgb, original.rgb);
  // Geometry for another observation.
  Scene fewer = s;
  fewer.geometry.boxes.pop_back();
  EXPECT_FALSE(AnnotateScreenshot(image, plan, 1280, 720, fewer.observation,
                                  fewer.geometry));
  EXPECT_EQ(image.rgb, original.rgb);
  // Nothing in view, nothing to mask: the image is valid and unchanged.
  Scene empty;
  empty.geometry.viewport_width = 1280;
  empty.geometry.viewport_height = 720;
  EXPECT_TRUE(AnnotateScreenshot(image, plan, 1280, 720, empty.observation,
                                 empty.geometry));
  EXPECT_EQ(image.rgb, original.rgb);
}

// ---------------------------------------------------------------------------
// After a request with an image
// ---------------------------------------------------------------------------

TEST(AgentVisionRetryTest, WhichFailuresDropTheImage) {
  agent_actions::ModelReply ok;
  ok.success = true;
  ok.text = "{}";
  EXPECT_FALSE(ShouldRetryWithoutImage(ok, false));

  agent_actions::ModelReply failed;
  failed.error_message = "The on-device model stopped.";
  EXPECT_TRUE(ShouldRetryWithoutImage(failed, false));
  EXPECT_TRUE(ShouldRetryWithoutImage(failed, true));

  // The model can't take images (another model was loaded meanwhile):
  // always without the image, whatever else the reply says.
  agent_actions::ModelReply unsupported = failed;
  EXPECT_TRUE(ShouldRetryWithoutImage(unsupported, true));
  unsupported.invalid_output = true;
  EXPECT_TRUE(ShouldRetryWithoutImage(unsupported, true));

  // Not for these: the user stopped it, or the reply is an unusable action
  // (asked again as usual), or the schema is refused (a bug: stop).
  agent_actions::ModelReply cancelled = failed;
  cancelled.cancelled = true;
  EXPECT_FALSE(ShouldRetryWithoutImage(cancelled, false));
  EXPECT_FALSE(ShouldRetryWithoutImage(cancelled, true));
  agent_actions::ModelReply invalid = failed;
  invalid.invalid_output = true;
  EXPECT_FALSE(ShouldRetryWithoutImage(invalid, false));
  agent_actions::ModelReply cut = failed;
  cut.truncated = true;
  EXPECT_FALSE(ShouldRetryWithoutImage(cut, false));
  agent_actions::ModelReply schema = failed;
  schema.constraint_unsupported = true;
  EXPECT_FALSE(ShouldRetryWithoutImage(schema, false));
}

}  // namespace
}  // namespace agent_vision
}  // namespace molt_ai
