// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_vision.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/molt_net_requests_policy.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"

namespace molt_ai {
namespace agent_vision {

namespace {

// ---- Capture ----------------------------------------------------------------

bool IsWebScheme(std::string_view scheme) {
  return scheme == "http" || scheme == "https";
}

// A serialized http(s) origin ("https://example.com"); an opaque origin
// serializes as "null".
bool IsWebOriginText(std::string_view origin) {
  for (std::string_view prefix :
       {std::string_view("http://"), std::string_view("https://")}) {
    if (origin.size() > prefix.size() &&
        origin.substr(0, prefix.size()) == prefix) {
      return true;
    }
  }
  return false;
}

// ---- Parsing ----------------------------------------------------------------

constexpr size_t kMaxOriginBytes = 512;

bool HasControlChars(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return c < 0x20 || c == 0x7F;
  });
}

// An integer in [lo, hi]. The page's numbers reach the browser as ints, or
// as doubles when they don't fit one; a double must be a whole number.
std::optional<int> ReadInt(const base::Value* v, int lo, int hi) {
  if (!v)
    return std::nullopt;
  double d;
  if (v->is_int()) {
    d = v->GetInt();
  } else if (v->is_double()) {
    d = v->GetDouble();
    if (!std::isfinite(d) || std::floor(d) != d)
      return std::nullopt;
  } else {
    return std::nullopt;
  }
  if (d < lo || d > hi)
    return std::nullopt;
  return static_cast<int>(d);
}

// [x, y, width, height].
std::optional<Box> ReadBox(const base::Value& v) {
  if (!v.is_list() || v.GetList().size() != 4)
    return std::nullopt;
  const base::ListValue& l = v.GetList();
  std::optional<int> x = ReadInt(&l[0], -kMaxCoordinate, kMaxCoordinate);
  std::optional<int> y = ReadInt(&l[1], -kMaxCoordinate, kMaxCoordinate);
  std::optional<int> w = ReadInt(&l[2], 1, kMaxCoordinate);
  std::optional<int> h = ReadInt(&l[3], 1, kMaxCoordinate);
  if (!x || !y || !w || !h)
    return std::nullopt;
  return Box{*x, *y, *w, *h};
}

bool Near(int a, int b) {
  return std::abs(static_cast<long long>(a) - b) <= 1;
}

bool NearBox(const Box& a, const Box& b) {
  return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.width, b.width) &&
         Near(a.height, b.height);
}

// ---- Tags -------------------------------------------------------------------

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphGap = 1;  // cells between glyphs
constexpr size_t kMaxRefDigits = 3;

struct Glyph {
  char c;
  // Rows top to bottom; bit 4 is the leftmost cell.
  uint8_t rows[kGlyphHeight];
};

constexpr Glyph kGlyphs[] = {
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'e', {0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110}},
};

const Glyph* FindGlyph(char c) {
  for (const Glyph& g : kGlyphs) {
    if (g.c == c)
      return &g;
  }
  return nullptr;
}

// "e" and 1 to kMaxRefDigits digits: the ids ParseElementSnapshot assigns.
bool IsTagText(std::string_view ref) {
  if (ref.size() < 2 || ref.size() > 1 + kMaxRefDigits || ref[0] != 'e')
    return false;
  return std::all_of(ref.begin() + 1, ref.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

bool Overlaps(const Rect& a, const Rect& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
         b.y < a.y + a.height;
}

bool Contains(const Rect& outer, const Rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.width <= outer.x + outer.width &&
         inner.y + inner.height <= outer.y + outer.height;
}

int Clamp(int v, int lo, int hi) {
  return hi < lo ? lo : std::clamp(v, lo, hi);
}

// ---- Pixels -----------------------------------------------------------------

// |rect| clipped to the image.
Rect ClipToImage(const ModelImage& image, const Rect& rect) {
  const long long x0 = std::max<long long>(rect.x, 0);
  const long long y0 = std::max<long long>(rect.y, 0);
  const long long x1 =
      std::min<long long>(static_cast<long long>(rect.x) + rect.width,
                          static_cast<long long>(image.width));
  const long long y1 =
      std::min<long long>(static_cast<long long>(rect.y) + rect.height,
                          static_cast<long long>(image.height));
  if (rect.IsEmpty() || x1 <= x0 || y1 <= y0)
    return Rect();
  return Rect{static_cast<int>(x0), static_cast<int>(y0),
              static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
}

bool HasPixels(const ModelImage& image) {
  return image.width > 0 && image.height > 0 &&
         image.rgb.size() == static_cast<size_t>(image.width) * image.height *
                                 kImageBytesPerPixel;
}

void SetPixel(ModelImage& image, int x, int y, const uint8_t rgb[3]) {
  if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= image.width ||
      static_cast<uint32_t>(y) >= image.height) {
    return;
  }
  const size_t i =
      (static_cast<size_t>(y) * image.width + static_cast<size_t>(x)) *
      kImageBytesPerPixel;
  image.rgb[i] = rgb[0];
  image.rgb[i + 1] = rgb[1];
  image.rgb[i + 2] = rgb[2];
}

void DrawText(ModelImage& image, const Tag& tag) {
  int x = tag.label.x + kTagPadding;
  const int y = tag.label.y + kTagPadding;
  for (char c : tag.ref) {
    const Glyph* g = FindGlyph(c);
    if (!g)
      return;
    for (int row = 0; row < kGlyphHeight; ++row) {
      for (int col = 0; col < kGlyphWidth; ++col) {
        if (!(g->rows[row] & (1 << (kGlyphWidth - 1 - col))))
          continue;
        FillRect(image,
                 Rect{x + col * kGlyphScale, y + row * kGlyphScale, kGlyphScale,
                      kGlyphScale},
                 kTagTextRgb);
      }
    }
    x += (kGlyphWidth + kGlyphGap) * kGlyphScale;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Whether to capture
// ---------------------------------------------------------------------------

CaptureFacts::CaptureFacts() = default;
CaptureFacts::~CaptureFacts() = default;
CaptureFacts::CaptureFacts(const CaptureFacts&) = default;
CaptureFacts& CaptureFacts::operator=(const CaptureFacts&) = default;

CaptureDecision DecideCapture(const CaptureFacts& facts) {
  if (!facts.switched_on)
    return CaptureDecision::kSwitchedOff;
  if (!facts.run_allows_images)
    return CaptureDecision::kRunOff;
  if (!facts.model_sees_images)
    return CaptureDecision::kNoVision;
  // The screenshot goes where the page text goes. In MoltNet that is only
  // the on-device model: cloud AI is off there.
  if (!facts.on_device_model &&
      !tor::MoltNetRequestAccessForProfile(facts.in_moltnet).cloud_ai) {
    return CaptureDecision::kMoltNetCloud;
  }
  if (!facts.tab_visible)
    return CaptureDecision::kTabHidden;
  if (!facts.same_document)
    return CaptureDecision::kDocumentChanged;
  if (!IsWebScheme(facts.url_scheme) ||
      !IsWebOriginText(facts.committed_origin)) {
    return CaptureDecision::kNotWebPage;
  }
  if (facts.error_document)
    return CaptureDecision::kErrorPage;
  if (facts.observed_origin != facts.committed_origin)
    return CaptureDecision::kOriginChanged;
  if (!facts.origin_allowed)
    return CaptureDecision::kOriginNotAllowed;
  // A mask covers only a frame's box, and the page decides how the frame
  // is painted: another origin's frame anywhere in the page means no
  // screenshot.
  if (facts.cross_origin_frames > 0)
    return CaptureDecision::kCrossOriginFrame;
  if (!facts.frames_settled)
    return CaptureDecision::kFramesChanging;
  return CaptureDecision::kCapture;
}

std::string_view CaptureDecisionName(CaptureDecision decision) {
  switch (decision) {
    case CaptureDecision::kCapture:
      return "capture";
    case CaptureDecision::kSwitchedOff:
      return "switched off";
    case CaptureDecision::kRunOff:
      return "off for this run";
    case CaptureDecision::kNoVision:
      return "model without vision";
    case CaptureDecision::kMoltNetCloud:
      return "MoltNet, not on-device";
    case CaptureDecision::kTabHidden:
      return "tab hidden";
    case CaptureDecision::kDocumentChanged:
      return "document changed";
    case CaptureDecision::kNotWebPage:
      return "not an http(s) page";
    case CaptureDecision::kErrorPage:
      return "error page";
    case CaptureDecision::kOriginChanged:
      return "origin changed";
    case CaptureDecision::kOriginNotAllowed:
      return "origin not allowed";
    case CaptureDecision::kCrossOriginFrame:
      return "frame of another origin";
    case CaptureDecision::kFramesChanging:
      return "frames changing";
  }
  return "";
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

ScreenshotGeometry::ScreenshotGeometry() = default;
ScreenshotGeometry::~ScreenshotGeometry() = default;
ScreenshotGeometry::ScreenshotGeometry(const ScreenshotGeometry&) = default;
ScreenshotGeometry& ScreenshotGeometry::operator=(const ScreenshotGeometry&) =
    default;
ScreenshotGeometry::ScreenshotGeometry(ScreenshotGeometry&&) = default;
ScreenshotGeometry& ScreenshotGeometry::operator=(ScreenshotGeometry&&) =
    default;

std::string BuildGeometryScript(const agent_actions::Observation& observation) {
  // The selectors the snapshot built (page-derived), as one JSON array.
  base::ListValue selectors;
  for (const agent_actions::ObservedElement& e : observation.elements)
    selectors.Append(e.selector);
  std::string selectors_json;
  base::JSONWriter::Write(selectors, &selectors_json);

  // Runs in the agent's isolated world: the page shapes the DOM it
  // measures, not this code. Boxes are getBoundingClientRect() rounded
  // outwards, relative to the layout viewport. What to mask is found in
  // the document and in open shadow roots (a closed one can't be read),
  // walking at most WALK elements; past that, or past MAX_MASKS regions in
  // view, |over| is set and the browser takes no screenshot. The two
  // limits are kept equal to the C++ ones by the static_asserts below.
  // |frames| counts the frame elements found, in view or not, for the
  // browser to compare with the frames the document has (FoundEveryFrame()).
  static_assert(kMaxMasks == 32, "update MAX_MASKS in the geometry script");
  static_assert(kMaxGeometryWalk == 50000, "update WALK in the script");
  return base::StringPrintf(
      R"JS((() => {
  try {
    const sels = %s;
    const MAX_MASKS = 32, WALK = 50000;
    const vw = window.innerWidth || 0, vh = window.innerHeight || 0;
    const vv = window.visualViewport;
    const zoomed = !!vv && (Math.abs(vv.scale - 1) > 0.001 ||
        Math.abs(vv.offsetLeft) > 0.5 || Math.abs(vv.offsetTop) > 0.5);
    const box = (el) => {
      const r = el.getBoundingClientRect();
      if (!(r.width > 0 && r.height > 0)) return null;
      const x = Math.floor(r.left), y = Math.floor(r.top);
      return [x, y, Math.ceil(r.right) - x, Math.ceil(r.bottom) - y];
    };
    const inView = (b) => !!b && b[0] < vw && b[1] < vh &&
        b[0] + b[2] > 0 && b[1] + b[3] > 0;
    const boxes = sels.map((s) => {
      let el = null;
      try { el = document.querySelector(s); } catch (e) { el = null; }
      const b = el ? box(el) : null;
      return inView(b) ? b : null;
    });
    const FRAMES = ['iframe', 'frame', 'embed', 'object', 'fencedframe',
                    'portal'];
    const autofilled = (el) => {
      try { return el.matches(':autofill'); } catch (e) {}
      try { return el.matches(':-webkit-autofill'); } catch (e) {}
      return false;
    };
    const secret = (el) => {
      const t = el.localName;
      if (FRAMES.includes(t)) return true;
      if (t !== 'input' && t !== 'textarea' && t !== 'select') return false;
      if (t === 'input' && String(el.type || '').toLowerCase() === 'password')
        return true;
      const ac = String(el.getAttribute('autocomplete') || '').toLowerCase();
      if (ac.includes('password') || ac.includes('cc-') ||
          ac.includes('one-time-code'))
        return true;
      return autofilled(el);
    };
    const masks = [];
    let over = false, walked = 0, frames = 0;
    const roots = [document];
    for (let i = 0; i < roots.length && !over; i++) {
      const w = document.createTreeWalker(roots[i], NodeFilter.SHOW_ELEMENT);
      for (let el = w.nextNode(); el; el = w.nextNode()) {
        if (++walked > WALK) { over = true; break; }
        if (el.shadowRoot) roots.push(el.shadowRoot);
        if (FRAMES.includes(el.localName)) frames++;
        if (!secret(el)) continue;
        const b = box(el);
        if (!inView(b)) continue;
        if (masks.length >= MAX_MASKS) { over = true; break; }
        masks.push(b);
      }
    }
    return {origin: String(location.origin),
            view: {w: vw, h: vh, sx: Math.round(window.scrollX || 0),
                   sy: Math.round(window.scrollY || 0), zoomed: zoomed},
            boxes: boxes, masks: masks, frames: frames, over: over};
  } catch (e) {
    return {origin: '', over: true};
  }
})())JS",
      selectors_json.c_str());
}

std::optional<ScreenshotGeometry> ParseGeometry(const base::Value& result,
                                                size_t element_count) {
  if (!result.is_dict())
    return std::nullopt;
  const base::DictValue& d = result.GetDict();
  // More to mask than the cap, or the script failed: no screenshot.
  std::optional<bool> over = d.FindBool("over");
  if (!over || *over)
    return std::nullopt;

  ScreenshotGeometry g;
  const std::string* origin = d.FindString("origin");
  if (!origin || origin->empty() || origin->size() > kMaxOriginBytes ||
      HasControlChars(*origin)) {
    return std::nullopt;
  }
  g.origin = *origin;

  const base::DictValue* view = d.FindDict("view");
  if (!view)
    return std::nullopt;
  // Pinch-zoomed: the screenshot shows the visual viewport, not the layout
  // viewport the boxes are relative to.
  std::optional<bool> zoomed = view->FindBool("zoomed");
  if (!zoomed || *zoomed)
    return std::nullopt;
  std::optional<int> w = ReadInt(view->Find("w"), 1, kMaxViewportSide);
  std::optional<int> h = ReadInt(view->Find("h"), 1, kMaxViewportSide);
  std::optional<int> sx =
      ReadInt(view->Find("sx"), -kMaxCoordinate, kMaxCoordinate);
  std::optional<int> sy =
      ReadInt(view->Find("sy"), -kMaxCoordinate, kMaxCoordinate);
  if (!w || !h || !sx || !sy)
    return std::nullopt;
  g.viewport_width = *w;
  g.viewport_height = *h;
  g.scroll_x = *sx;
  g.scroll_y = *sy;

  const base::ListValue* boxes = d.FindList("boxes");
  if (!boxes || boxes->size() != element_count)
    return std::nullopt;
  for (const base::Value& v : *boxes) {
    if (v.is_none()) {
      g.boxes.emplace_back(std::nullopt);
      continue;
    }
    std::optional<Box> b = ReadBox(v);
    if (!b)
      return std::nullopt;
    g.boxes.emplace_back(*b);
  }

  // Every mask must be readable: one that isn't can't be applied.
  const base::ListValue* masks = d.FindList("masks");
  if (!masks || masks->size() > kMaxMasks)
    return std::nullopt;
  for (const base::Value& v : *masks) {
    std::optional<Box> b = ReadBox(v);
    if (!b)
      return std::nullopt;
    g.masks.push_back(*b);
  }

  // Without the count, frames the walk didn't see can't be ruled out.
  std::optional<int> frames = ReadInt(d.Find("frames"), 0, kMaxGeometryWalk);
  if (!frames)
    return std::nullopt;
  g.frame_elements = static_cast<size_t>(*frames);
  return g;
}

bool FoundEveryFrame(const ScreenshotGeometry& geometry, size_t child_frames) {
  return child_frames <= geometry.frame_elements;
}

bool SameGeometry(const ScreenshotGeometry& before,
                  const ScreenshotGeometry& after) {
  if (before.origin != after.origin ||
      before.viewport_width != after.viewport_width ||
      before.viewport_height != after.viewport_height ||
      before.scroll_x != after.scroll_x || before.scroll_y != after.scroll_y ||
      before.boxes.size() != after.boxes.size() ||
      before.masks.size() != after.masks.size() ||
      before.frame_elements != after.frame_elements) {
    return false;
  }
  for (size_t i = 0; i < before.boxes.size(); ++i) {
    const std::optional<Box>& a = before.boxes[i];
    const std::optional<Box>& b = after.boxes[i];
    if (a.has_value() != b.has_value() || (a && !NearBox(*a, *b)))
      return false;
  }
  for (size_t i = 0; i < before.masks.size(); ++i) {
    if (!NearBox(before.masks[i], after.masks[i]))
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// From the viewport to the model image
// ---------------------------------------------------------------------------

std::optional<CaptureSize> CaptureSizeFor(int view_width, int view_height) {
  if (view_width <= 0 || view_height <= 0 || view_width > kMaxViewportSide ||
      view_height > kMaxViewportSide) {
    return std::nullopt;
  }
  const int max_side = static_cast<int>(kCaptureMaxSide);
  const int long_side = std::max(view_width, view_height);
  if (long_side <= max_side)
    return CaptureSize{view_width, view_height};
  // Rounded to the nearest pixel; both sides fit an int64 product.
  auto scale = [&](int side) {
    const long long scaled =
        (static_cast<long long>(side) * max_side + long_side / 2) / long_side;
    return static_cast<int>(std::clamp<long long>(scaled, 1, max_side));
  };
  return CaptureSize{scale(view_width), scale(view_height)};
}

std::optional<ImageMapping> MapViewport(int viewport_width,
                                        int viewport_height,
                                        uint32_t capture_width,
                                        uint32_t capture_height,
                                        const ImagePlan& plan) {
  if (viewport_width <= 0 || viewport_height <= 0 || capture_width == 0 ||
      capture_height == 0 || plan.content_width == 0 ||
      plan.content_height == 0 ||
      uint64_t{plan.offset_x} + plan.content_width > plan.canvas_width ||
      uint64_t{plan.offset_y} + plan.content_height > plan.canvas_height ||
      plan.canvas_width > kMaxImageSide || plan.canvas_height > kMaxImageSide) {
    return std::nullopt;
  }
  // The capture must show the viewport measured: the same aspect ratio,
  // within 2%.
  const uint64_t a =
      uint64_t{capture_width} * static_cast<uint64_t>(viewport_height);
  const uint64_t b =
      uint64_t{capture_height} * static_cast<uint64_t>(viewport_width);
  const uint64_t diff = a > b ? a - b : b - a;
  if (diff * 50 > std::max(a, b))
    return std::nullopt;
  ImageMapping m;
  m.scale_x = static_cast<double>(plan.content_width) / viewport_width;
  m.scale_y = static_cast<double>(plan.content_height) / viewport_height;
  m.content =
      Rect{static_cast<int>(plan.offset_x), static_cast<int>(plan.offset_y),
           static_cast<int>(plan.content_width),
           static_cast<int>(plan.content_height)};
  return m;
}

Rect MapBox(const Box& box, const ImageMapping& mapping, int margin) {
  if (box.width <= 0 || box.height <= 0)
    return Rect();
  const double x0 = std::floor(box.x * mapping.scale_x) - margin;
  const double y0 = std::floor(box.y * mapping.scale_y) - margin;
  const double x1 =
      std::ceil((static_cast<double>(box.x) + box.width) * mapping.scale_x) +
      margin;
  const double y1 =
      std::ceil((static_cast<double>(box.y) + box.height) * mapping.scale_y) +
      margin;
  // Clip to the screenshot, in its own coordinates, then place it on the
  // canvas.
  const double cx0 = std::max(x0, 0.0);
  const double cy0 = std::max(y0, 0.0);
  const double cx1 = std::min(x1, static_cast<double>(mapping.content.width));
  const double cy1 = std::min(y1, static_cast<double>(mapping.content.height));
  if (!(cx1 > cx0 && cy1 > cy0))
    return Rect();
  return Rect{mapping.content.x + static_cast<int>(cx0),
              mapping.content.y + static_cast<int>(cy0),
              static_cast<int>(cx1 - cx0), static_cast<int>(cy1 - cy0)};
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

Mark::Mark(std::string mark_ref, Rect mark_target)
    : ref(std::move(mark_ref)), target(mark_target) {}
Mark::~Mark() = default;
Mark::Mark(const Mark&) = default;
Mark& Mark::operator=(const Mark&) = default;
Mark::Mark(Mark&&) = default;
Mark& Mark::operator=(Mark&&) = default;

Tag::Tag(std::string tag_ref, Rect tag_label, Rect tag_target)
    : ref(std::move(tag_ref)), label(tag_label), target(tag_target) {}
Tag::~Tag() = default;
Tag::Tag(const Tag&) = default;
Tag& Tag::operator=(const Tag&) = default;
Tag::Tag(Tag&&) = default;
Tag& Tag::operator=(Tag&&) = default;

Rect TagSize(std::string_view ref) {
  if (!IsTagText(ref))
    return Rect();
  const int n = static_cast<int>(ref.size());
  return Rect{0, 0,
              2 * kTagPadding + n * kGlyphWidth * kGlyphScale +
                  (n - 1) * kGlyphGap * kGlyphScale,
              2 * kTagPadding + kGlyphHeight * kGlyphScale};
}

std::vector<Tag> LayoutTags(const std::vector<Mark>& marks,
                            const Rect& bounds) {
  std::vector<Tag> tags;
  if (bounds.IsEmpty())
    return tags;
  const int right = bounds.x + bounds.width;
  const int bottom = bounds.y + bounds.height;
  for (const Mark& mark : marks) {
    if (tags.size() >= kMaxTags)
      break;
    const Rect size = TagSize(mark.ref);
    const Rect& t = mark.target;
    if (size.IsEmpty() || t.IsEmpty())
      continue;
    const int w = size.width;
    const int h = size.height;
    // Along the edge the tag sits on, it may slide to stay in bounds.
    const int sx = Clamp(t.x, bounds.x, right - w);
    const int sy = Clamp(t.y, bounds.y, bottom - h);
    const Rect inside{sx, sy, w, h};
    const Rect above{sx, t.y - h, w, h};
    const Rect below{sx, t.y + t.height, w, h};
    const Rect left{t.x - w, sy, w, h};
    const Rect right_of{t.x + t.width, sy, w, h};
    // A large element (an input, a card) has room for its tag inside; a
    // small one would be hidden under it.
    const bool large = t.width >= 2 * w && t.height >= 2 * h;
    const std::array<Rect, 5> order =
        large ? std::array<Rect, 5>{inside, above, below, left, right_of}
              : std::array<Rect, 5>{above, below, left, right_of, inside};
    for (const Rect& r : order) {
      if (!Contains(bounds, r))
        continue;
      if (std::any_of(tags.begin(), tags.end(),
                      [&](const Tag& p) { return Overlaps(p.label, r); })) {
        continue;
      }
      tags.push_back(Tag{mark.ref, r, t});
      break;
    }
  }
  return tags;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void FillRect(ModelImage& image, const Rect& rect, const uint8_t rgb[3]) {
  if (!HasPixels(image))
    return;
  const Rect r = ClipToImage(image, rect);
  for (int y = r.y; y < r.y + r.height; ++y) {
    for (int x = r.x; x < r.x + r.width; ++x)
      SetPixel(image, x, y, rgb);
  }
}

void OutlineRect(ModelImage& image, const Rect& rect, const uint8_t rgb[3]) {
  if (rect.IsEmpty())
    return;
  FillRect(image, Rect{rect.x, rect.y, rect.width, 1}, rgb);
  FillRect(image, Rect{rect.x, rect.y + rect.height - 1, rect.width, 1}, rgb);
  FillRect(image, Rect{rect.x, rect.y, 1, rect.height}, rgb);
  FillRect(image, Rect{rect.x + rect.width - 1, rect.y, 1, rect.height}, rgb);
}

void DrawTags(ModelImage& image, const std::vector<Tag>& tags) {
  for (const Tag& tag : tags)
    OutlineRect(image, tag.target, kTagRgb);
  for (const Tag& tag : tags) {
    FillRect(image, tag.label, kTagRgb);
    DrawText(image, tag);
  }
}

bool AnnotateScreenshot(ModelImage& image,
                        const ImagePlan& plan,
                        uint32_t capture_width,
                        uint32_t capture_height,
                        const agent_actions::Observation& observation,
                        const ScreenshotGeometry& geometry) {
  if (!image.IsValid() || image.width != plan.canvas_width ||
      image.height != plan.canvas_height ||
      geometry.boxes.size() != observation.elements.size()) {
    return false;
  }
  const std::optional<ImageMapping> mapping =
      MapViewport(geometry.viewport_width, geometry.viewport_height,
                  capture_width, capture_height, plan);
  if (!mapping)
    return false;

  // Masks first: the regions the script found, and every listed field
  // agent_safety calls sensitive.
  for (const Box& box : geometry.masks)
    FillRect(image, MapBox(box, *mapping, kMaskMargin), kMaskRgb);
  for (size_t i = 0; i < observation.elements.size(); ++i) {
    if (geometry.boxes[i] &&
        agent_safety::IsSensitive(observation.elements[i].field_kind)) {
      FillRect(image, MapBox(*geometry.boxes[i], *mapping, kMaskMargin),
               kMaskRgb);
    }
  }

  // Then a tag per listed element in view.
  std::vector<Mark> marks;
  for (size_t i = 0; i < observation.elements.size(); ++i) {
    if (!geometry.boxes[i])
      continue;
    const Rect target = MapBox(*geometry.boxes[i], *mapping, 0);
    if (!target.IsEmpty())
      marks.push_back(Mark{observation.elements[i].ref, target});
  }
  DrawTags(image, LayoutTags(marks, mapping->content));
  return true;
}

// ---------------------------------------------------------------------------
// After a request with an image
// ---------------------------------------------------------------------------

bool ShouldRetryWithoutImage(const agent_actions::ModelReply& reply,
                             bool images_unsupported) {
  if (reply.cancelled)
    return false;
  if (images_unsupported)
    return true;
  if (reply.success || reply.invalid_output || reply.truncated ||
      reply.constraint_unsupported) {
    return false;
  }
  return true;
}

}  // namespace agent_vision
}  // namespace molt_ai
