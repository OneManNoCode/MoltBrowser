// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent's screenshot logic (agents/agent_vision.h) on what the page
// controls: the result of the geometry script (any JSON: the page shapes
// the DOM it measures and can race the script), the selectors and labels
// of its elements, and the size of its view.
//
// Checks: no memory or UB error; ParseGeometry() accepts only geometry
// within its documented ranges, deterministically, and SameGeometry() is
// reflexive; LayoutTags() never overlaps two tags, keeps every tag inside
// the screenshot and makes at most kMaxTags; AnnotateScreenshot() never
// touches the canvas padding, and leaves no original pixel inside any mask
// (the script's masks and every sensitive listed field, grown by
// kMaskMargin): a page can't get a password field's pixels to the model.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/agents/agent_vision.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"

namespace molt_ai::agent_vision {
namespace {

// The canvas before annotation: a colour no mask or tag uses.
constexpr uint8_t kOriginal[3] = {7, 9, 11};

int Coordinate(FuzzedDataProvider& p, int lo, int hi) {
  switch (p.ConsumeIntegralInRange<int>(0, 7)) {
    case 0:
      return p.ConsumeIntegral<int>();
    case 1:
      return lo;
    case 2:
      return hi;
    default:
      return p.ConsumeIntegralInRange<int>(lo, hi);
  }
}

base::Value Number(FuzzedDataProvider& p, int lo, int hi) {
  const int v = Coordinate(p, lo, hi);
  switch (p.ConsumeIntegralInRange<int>(0, 5)) {
    case 0:
      return base::Value(static_cast<double>(v));
    case 1:
      return base::Value(v + 0.5);
    case 2:
      return base::Value(p.ConsumeFloatingPoint<double>());
    default:
      return base::Value(v);
  }
}

base::Value BoxValue(FuzzedDataProvider& p, int view_w, int view_h) {
  base::ListValue box;
  const size_t n = p.ConsumeIntegralInRange<int>(0, 15) == 0 ? 3 : 4;
  box.Append(Number(p, -view_w, 2 * view_w));
  box.Append(Number(p, -view_h, 2 * view_h));
  box.Append(Number(p, 1, 2 * view_w));
  if (n == 4) {
    box.Append(Number(p, 1, 2 * view_h));
  }
  return base::Value(std::move(box));
}

base::Value GeometryValue(FuzzedDataProvider& p, size_t elements) {
  if (p.ConsumeIntegralInRange<int>(0, 31) == 0) {
    // Any JSON at all.
    std::optional<base::Value> any = base::JSONReader::Read(
        p.ConsumeRandomLengthString(512), base::JSON_PARSE_RFC);
    return any ? std::move(*any) : base::Value();
  }
  const int view_w = p.ConsumeIntegralInRange<int>(1, 4096);
  const int view_h = p.ConsumeIntegralInRange<int>(1, 4096);
  base::DictValue d;
  d.Set("origin", p.ConsumeBool() ? std::string("https://example.com")
                                  : p.ConsumeRandomLengthString(64));
  d.Set("over", p.ConsumeIntegralInRange<int>(0, 15) == 0);
  base::DictValue view;
  view.Set("w", Number(p, 1, view_w));
  view.Set("h", Number(p, 1, view_h));
  view.Set("sx", Number(p, -100000, 100000));
  view.Set("sy", Number(p, -100000, 100000));
  view.Set("zoomed", p.ConsumeIntegralInRange<int>(0, 15) == 0);
  d.Set("view", std::move(view));
  base::ListValue boxes;
  const size_t box_count =
      p.ConsumeIntegralInRange<int>(0, 15) == 0
          ? p.ConsumeIntegralInRange<size_t>(0, elements + 1)
          : elements;
  for (size_t i = 0; i < box_count; ++i) {
    if (p.ConsumeIntegralInRange<int>(0, 3) == 0) {
      boxes.Append(base::Value());
    } else {
      boxes.Append(BoxValue(p, view_w, view_h));
    }
  }
  d.Set("boxes", std::move(boxes));
  base::ListValue masks;
  const size_t mask_count = p.ConsumeIntegralInRange<size_t>(0, kMaxMasks + 1);
  for (size_t i = 0; i < mask_count; ++i) {
    masks.Append(BoxValue(p, view_w, view_h));
  }
  d.Set("masks", std::move(masks));
  // The frame elements found: usually a count (also when the input has run
  // out, so older seeds still parse), sometimes missing or off.
  if (p.ConsumeIntegralInRange<int>(0, 15) != 15) {
    d.Set("frames", Number(p, 0, 64));
  }
  return base::Value(std::move(d));
}

bool IsOriginal(const ModelImage& image, int x, int y) {
  const size_t at = (static_cast<size_t>(y) * image.width + x) * 3;
  return image.rgb[at] == kOriginal[0] && image.rgb[at + 1] == kOriginal[1] &&
         image.rgb[at + 2] == kOriginal[2];
}

void CheckMasked(const ModelImage& image, const Rect& r) {
  for (int y = r.y; y < r.y + r.height; ++y) {
    for (int x = r.x; x < r.x + r.width; ++x) {
      CHECK(x >= 0 && y >= 0 && x < static_cast<int>(image.width) &&
            y < static_cast<int>(image.height));
      CHECK(!IsOriginal(image, x, y)) << "a masked pixel reached the model";
    }
  }
}

bool Overlap(const Rect& a, const Rect& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
         b.y < a.y + a.height;
}

bool Inside(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         int64_t{inner.x} + inner.width <= int64_t{outer.x} + outer.width &&
         int64_t{inner.y} + inner.height <= int64_t{outer.y} + outer.height;
}

void CheckTags(const std::vector<Tag>& tags, const Rect& bounds) {
  CHECK(tags.size() <= kMaxTags);
  for (size_t i = 0; i < tags.size(); ++i) {
    CHECK(!tags[i].label.IsEmpty());
    CHECK(Inside(bounds, tags[i].label)) << "a tag outside the screenshot";
    for (size_t j = 0; j < i; ++j) {
      CHECK(!Overlap(tags[i].label, tags[j].label)) << "overlapping tags";
    }
  }
}

}  // namespace
}  // namespace molt_ai::agent_vision

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  namespace av = molt_ai::agent_vision;
  namespace aa = molt_ai::agent_actions;
  FuzzedDataProvider p(data, size);

  aa::Observation observation;
  const size_t elements = p.ConsumeIntegralInRange<size_t>(0, 64);
  for (size_t i = 0; i < elements; ++i) {
    aa::ObservedElement e;
    e.ref = p.ConsumeIntegralInRange<int>(0, 31) == 0
                ? p.ConsumeRandomLengthString(6)
                : "e" + std::to_string(i + 1);
    e.selector = "#x" + std::to_string(i);
    e.kind = "input";
    e.field_kind = static_cast<molt_ai::agent_safety::FieldKind>(
        p.ConsumeIntegralInRange<int>(0, 3));
    observation.elements.push_back(std::move(e));
  }

  const base::Value result = av::GeometryValue(p, elements);
  const std::optional<av::ScreenshotGeometry> geometry =
      av::ParseGeometry(result, elements);
  // Also the script builds for any observation.
  (void)av::BuildGeometryScript(observation);

  // Tags on their own, for marks and bounds anywhere a mapping could put
  // them (MapBox() keeps them on the model image; +-1e6 is far beyond).
  {
    auto coordinate = [&p](int lo, int hi) {
      const int v = av::Coordinate(p, lo, hi);
      return v < -1000000 ? -1000000 : v > 1000000 ? 1000000 : v;
    };
    std::vector<av::Mark> marks;
    const size_t n = p.ConsumeIntegralInRange<size_t>(0, 80);
    for (size_t i = 0; i < n; ++i) {
      marks.emplace_back(
          p.ConsumeBool() ? "e" + std::to_string(p.ConsumeIntegral<uint16_t>())
                          : p.ConsumeRandomLengthString(5),
          av::Rect{coordinate(-2000, 2000), coordinate(-2000, 2000),
                   coordinate(-10, 2000), coordinate(-10, 2000)});
    }
    const av::Rect bounds{coordinate(-100, 1000),
                          coordinate(-100, 1000),
                          coordinate(-10, 1000),
                          coordinate(-10, 1000)};
    av::CheckTags(av::LayoutTags(marks, bounds), bounds);
  }

  if (!geometry) {
    return 0;
  }
  const av::ScreenshotGeometry& g = *geometry;
  CHECK(g.boxes.size() == elements);
  CHECK(g.masks.size() <= av::kMaxMasks);
  CHECK(g.viewport_width >= 1 && g.viewport_width <= av::kMaxViewportSide);
  CHECK(g.viewport_height >= 1 && g.viewport_height <= av::kMaxViewportSide);
  for (const av::Box& b : g.masks) {
    CHECK(b.width >= 1 && b.height >= 1);
  }
  CHECK(g.frame_elements <= static_cast<size_t>(av::kMaxGeometryWalk));
  // A document with a frame more than the script found gets no screenshot.
  CHECK(av::FoundEveryFrame(g, g.frame_elements));
  CHECK(!av::FoundEveryFrame(g, g.frame_elements + 1));
  CHECK(av::SameGeometry(g, g));
  const std::optional<av::ScreenshotGeometry> again =
      av::ParseGeometry(result, elements);
  CHECK(again && av::SameGeometry(g, *again));

  // The capture: the size the browser asks for, or one the view changed to.
  std::optional<av::CaptureSize> capture =
      av::CaptureSizeFor(g.viewport_width, g.viewport_height);
  CHECK(capture);
  CHECK(capture->width >= 1 && capture->height >= 1 &&
        capture->width <= static_cast<int>(av::kCaptureMaxSide) &&
        capture->height <= static_cast<int>(av::kCaptureMaxSide));
  uint32_t cw = static_cast<uint32_t>(capture->width);
  uint32_t ch = static_cast<uint32_t>(capture->height);
  if (p.ConsumeIntegralInRange<int>(0, 7) == 0) {
    cw = p.ConsumeIntegralInRange<uint32_t>(1, av::kCaptureMaxSide);
    ch = p.ConsumeIntegralInRange<uint32_t>(1, av::kCaptureMaxSide);
  }
  const std::optional<molt_ai::ImagePlan> plan =
      molt_ai::PlanModelImage(cw, ch, av::kScreenshotMaxSide);
  if (!plan) {
    return 0;
  }
  molt_ai::ModelImage image;
  image.width = plan->canvas_width;
  image.height = plan->canvas_height;
  image.rgb.resize(size_t{image.width} * image.height * 3);
  for (size_t i = 0; i < image.rgb.size(); ++i) {
    image.rgb[i] = av::kOriginal[i % 3];
  }
  if (!av::AnnotateScreenshot(image, *plan, cw, ch, observation, g)) {
    return 0;
  }
  CHECK(image.rgb.size() == size_t{image.width} * image.height * 3);
  const std::optional<av::ImageMapping> mapping = av::MapViewport(
      g.viewport_width, g.viewport_height, cw, ch, *plan);
  CHECK(mapping);
  const av::Rect content = mapping->content;
  // The padding around the screenshot is never drawn on.
  for (int y = 0; y < static_cast<int>(image.height); ++y) {
    for (int x = 0; x < static_cast<int>(image.width); ++x) {
      if (!av::Inside(content, av::Rect{x, y, 1, 1})) {
        CHECK(av::IsOriginal(image, x, y)) << "drew on the padding";
      }
    }
  }
  // Nothing of a mask survives.
  for (const av::Box& b : g.masks) {
    av::CheckMasked(image, av::MapBox(b, *mapping, av::kMaskMargin));
  }
  for (size_t i = 0; i < elements; ++i) {
    if (g.boxes[i] &&
        molt_ai::agent_safety::IsSensitive(observation.elements[i].field_kind)) {
      av::CheckMasked(image, av::MapBox(*g.boxes[i], *mapping, av::kMaskMargin));
    }
  }
  return 0;
}
