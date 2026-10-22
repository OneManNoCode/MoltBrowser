// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Screenshots for the web agent (docs/design/AGENT_ACTIONS.md §2.6).
//
// When the loaded on-device model can see images, WebAgent gives it a
// screenshot of the visible viewport with each observation. The element
// ids of the DOM snapshot stay the only way to target anything: the image
// is context, and the model still answers with an id from the ELEMENTS
// list, under the same grammar (agent_actions.h). To link pixels to ids
// without asking the model for coordinates, the browser draws a small tag
// (e1, e2, ...) at each listed element in view ("set of marks"), on its own
// copy of the screenshot, after masking what the model must not read:
// password, payment and one-time-code fields, autofilled fields (their
// previews aren't visible to the page) and embedded frames (other sites'
// content the page itself can't read).
//
// This file is the pure part, unit-tested in molt_ai_unittests:
//  - DecideCapture(): whether a capture may happen at all;
//  - the geometry script and its strict parser, and the before / after
//    comparison that keeps a page that moved during the capture out;
//  - the arithmetic from CSS pixels to model-image pixels, masks, the tag
//    layout and the drawing on the RGB pixels.
// The capture itself (compositor, worker) is AgentScreenshotter
// (agent_screenshot.h).

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_VISION_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_VISION_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/agents/agent_actions.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"

namespace base {
class Value;
}  // namespace base

namespace molt_ai {

struct ImagePlan;

namespace agent_vision {

// ---------------------------------------------------------------------------
// Caps
// ---------------------------------------------------------------------------

// The model image's long side: the web pages' cap (model_image.h), at most
// 576 tokens on a 32-pixel token grid (Qwen-VL); a 16:9 viewport is
// 768 x 432, about 330 tokens. One image per step, never older ones.
inline constexpr uint32_t kScreenshotMaxSide = kWebImageMaxSide;
// The compositor scales the viewport to at most this long side before the
// browser's own (Lanczos) scaling to kScreenshotMaxSide: sharper text than
// one GPU downscale, and a bounded readback on large screens.
inline constexpr uint32_t kCaptureMaxSide = 2 * kScreenshotMaxSide;
// Masked regions in view. A page with more gets no screenshot (they can't
// all be masked), only the text observation.
inline constexpr size_t kMaxMasks = 32;
// Tags drawn: at most one per listed element (agent_actions.h).
inline constexpr size_t kMaxTags = agent_actions::kMaxObservedElements;
// Model-image pixels added around every mask (after rounding outwards), so
// the scalers can't carry a masked pixel past its edge: Skia's Lanczos-3
// (PrepareImageForModel) reaches 3 model-image pixels, the compositor's
// downscale to the capture size about 1.5 capture pixels, which are no
// larger than model-image pixels (the browser never scales up).
inline constexpr int kMaskMargin = 5;
// Geometry the parser accepts (CSS pixels).
inline constexpr int kMaxViewportSide = 16384;
inline constexpr int kMaxCoordinate = 1000000;
// Elements the geometry script walks at most (more: no screenshot).
inline constexpr int kMaxGeometryWalk = 50000;

// ---------------------------------------------------------------------------
// Whether to capture
// ---------------------------------------------------------------------------

struct CaptureFacts {
  CaptureFacts();
  ~CaptureFacts();
  CaptureFacts(const CaptureFacts&);
  CaptureFacts& operator=(const CaptureFacts&);

  // chrome://flags/#molt-agent-screenshots (agent_features.h).
  bool switched_on = false;
  // No request with an image failed earlier in this run.
  bool run_allows_images = false;
  // The loaded model takes images (BrowserAIRuntime::LoadedModelSeesImages).
  bool model_sees_images = false;
  // The image goes to the on-device model in the sandboxed inference
  // service (always, for WebAgent), not to a cloud provider.
  bool on_device_model = false;
  // The tab's profile is MoltNet's.
  bool in_moltnet = false;
  // The tab is shown (content::Visibility::VISIBLE): a hidden or occluded
  // tab's surface is stale or missing.
  bool tab_visible = false;
  // The document the agent observed is still the tab's active primary main
  // document (WeakDocumentPtr).
  bool same_document = false;
  // That document is an error page (RenderFrameHost::IsErrorDocument()).
  bool error_document = false;
  // The committed URL's scheme, lower case.
  std::string url_scheme;
  // The committed origin (url::Origin::Serialize(), "null" when opaque) and
  // the location.origin the observation's scripts read in the document.
  std::string committed_origin;
  std::string observed_origin;
  // The committed origin is in the run's allowed set (WebAgent's scope).
  bool origin_allowed = false;
};

enum class CaptureDecision {
  kCapture,
  kSwitchedOff,   // the flag is off
  kRunOff,        // an image failed earlier in this run
  kNoVision,      // the loaded model can't see images
  kMoltNetCloud,  // MoltNet: only the on-device model may see the page
  kTabHidden,
  kDocumentChanged,
  kNotWebPage,  // not an http(s) URL and origin: molt://, chrome://, ...
  kErrorPage,
  kOriginChanged,  // the committed origin isn't the one observed
  kOriginNotAllowed,
};

// Captures only the agent's own tab, shown, on the document it observed,
// an http(s) page (never molt://, chrome://, file:, data:, about: or an
// error page) whose origin is the observed one and in the run's scope.
// MoltNet lets the agent run with the on-device model (cloud AI is off
// there, molt_net_requests_policy.h), so a MoltNet page is captured only
// for the on-device model, which keeps the pixels on the device.
CaptureDecision DecideCapture(const CaptureFacts& facts);

// For logs.
std::string_view CaptureDecisionName(CaptureDecision decision);

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// A rectangle in CSS pixels, relative to the viewport (getBoundingClientRect
// rounded outwards). width and height are positive.
struct Box {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  friend bool operator==(const Box&, const Box&) = default;
};

struct ScreenshotGeometry {
  ScreenshotGeometry();
  ~ScreenshotGeometry();
  ScreenshotGeometry(const ScreenshotGeometry&);
  ScreenshotGeometry& operator=(const ScreenshotGeometry&);
  ScreenshotGeometry(ScreenshotGeometry&&);
  ScreenshotGeometry& operator=(ScreenshotGeometry&&);

  // location.origin in the document the script ran in.
  std::string origin;
  // The layout viewport in CSS pixels (innerWidth x innerHeight, scroll
  // bars included, as the compositor draws them) and the scroll position.
  int viewport_width = 0;
  int viewport_height = 0;
  int scroll_x = 0;
  int scroll_y = 0;
  // One per element of the observation, in order: its box if it is in
  // view, nullopt if it isn't (or is gone).
  std::vector<std::optional<Box>> boxes;
  // What to mask, in view: password fields, fields whose autocomplete names
  // a password, payment card or one-time code, autofilled (or previewed)
  // fields, and frames (iframe, frame, embed, object, fencedframe), in the
  // document and its open shadow roots.
  std::vector<Box> masks;
  // The frame elements the script found there, in view or not.
  size_t frame_elements = 0;
};

// A script (run it in the agent's isolated world) that measures the
// viewport, the box of each element of |observation| (by the selector the
// snapshot built, as a CLICK finds it) and the regions to mask. Selectors
// go in as JSON, never as code.
std::string BuildGeometryScript(const agent_actions::Observation& observation);

// Parses the script's result for an observation of |element_count|
// elements. nullopt (no screenshot) if anything is missing, malformed or
// out of range, if the page is pinch-zoomed (the screenshot would show
// another rectangle than the layout viewport), or if there are more than
// kMaxMasks regions to mask.
std::optional<ScreenshotGeometry> ParseGeometry(const base::Value& result,
                                                size_t element_count);

// Whether the script found every frame of the document it measured:
// |child_frames| is the browser's count of the frames that document owns
// (its iframes, frames and fenced frames, and guests it embeds). A frame
// the script can't find, such as one inside a closed shadow root, would
// be drawn unmasked, so then there is no screenshot.
bool FoundEveryFrame(const ScreenshotGeometry& geometry, size_t child_frames);

// Whether the page stood still while it was captured: the geometry
// measured before and after the capture match (same origin, viewport and
// scroll position, frame count; every box and mask within 1 CSS pixel).
// Otherwise the tags and masks could miss what the screenshot shows.
bool SameGeometry(const ScreenshotGeometry& before,
                  const ScreenshotGeometry& after);

// ---------------------------------------------------------------------------
// From the viewport to the model image
// ---------------------------------------------------------------------------

// The bitmap size to ask the compositor for, for a view of |view_width| x
// |view_height| DIPs: the same aspect ratio, the long side at most
// kCaptureMaxSide, never larger than the view, each side at least 1.
// nullopt for an empty or absurd view.
struct CaptureSize {
  int width = 0;
  int height = 0;
};
std::optional<CaptureSize> CaptureSizeFor(int view_width, int view_height);

// A rectangle in model-image pixels: [x, x + width) x [y, y + height).
struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  bool IsEmpty() const { return width <= 0 || height <= 0; }
  friend bool operator==(const Rect&, const Rect&) = default;
};

struct ImageMapping {
  // Model-image pixels per CSS pixel.
  double scale_x = 0;
  double scale_y = 0;
  // The screenshot's place on the model image's canvas (image_input.h:
  // ImagePlan's content and offset).
  Rect content;
};

// The mapping for a viewport of |viewport_width| x |viewport_height| CSS
// pixels captured as a |capture_width| x |capture_height| bitmap that
// PrepareImageForModel() turned into a model image by |plan|. nullopt if
// the capture doesn't have the viewport's aspect ratio (within 2%: the
// view was resized, or isn't the viewport measured).
std::optional<ImageMapping> MapViewport(int viewport_width,
                                        int viewport_height,
                                        uint32_t capture_width,
                                        uint32_t capture_height,
                                        const ImagePlan& plan);

// |box| in model-image pixels, rounded outwards, grown by |margin| on every
// side and clipped to the screenshot (mapping.content); empty if nothing of
// it is in view.
Rect MapBox(const Box& box, const ImageMapping& mapping, int margin);

// ---------------------------------------------------------------------------
// Tags ("set of marks")
// ---------------------------------------------------------------------------

// What to tag: an element id ("e7") and its box on the model image.
struct Mark {
  Mark(std::string ref, Rect target);
  ~Mark();
  Mark(const Mark&);
  Mark& operator=(const Mark&);
  Mark(Mark&&);
  Mark& operator=(Mark&&);

  std::string ref;
  Rect target;
};

struct Tag {
  Tag(std::string ref, Rect label, Rect target);
  ~Tag();
  Tag(const Tag&);
  Tag& operator=(const Tag&);
  Tag(Tag&&);
  Tag& operator=(Tag&&);

  std::string ref;
  // The tag itself (a filled label) and the element it names.
  Rect label;
  Rect target;
};

// Glyphs are 5 x 7 cells drawn kGlyphScale pixels each, kTagPadding
// pixels of tag colour around the text.
inline constexpr int kGlyphScale = 2;
inline constexpr int kTagPadding = 2;

// The size of the tag for |ref| ("e" and 1 to 3 digits); empty for any
// other text.
Rect TagSize(std::string_view ref);

// Places a tag for each mark, in order, at most kMaxTags, never two tags
// overlapping and every tag inside |bounds| (the screenshot on the canvas).
// A large element gets its tag inside its top-left corner; a small one
// (an icon button) next to it, so the tag doesn't hide it: above, below,
// left or right, else inside. A mark with no free place gets no tag (its
// id is still in the ELEMENTS list).
std::vector<Tag> LayoutTags(const std::vector<Mark>& marks, const Rect& bounds);

// ---------------------------------------------------------------------------
// Drawing on the model image (8-bit RGB, model_image.h)
// ---------------------------------------------------------------------------

// Mask fill: neutral grey, the canvas padding colour (image_input.h).
inline constexpr uint8_t kMaskRgb[3] = {128, 128, 128};
// Tags: white text on magenta, a colour pages rarely use; the element's
// outline in the same magenta.
inline constexpr uint8_t kTagRgb[3] = {214, 0, 120};
inline constexpr uint8_t kTagTextRgb[3] = {255, 255, 255};

// Fills |rect| (clipped to the image) with |rgb|.
void FillRect(ModelImage& image, const Rect& rect, const uint8_t rgb[3]);
// A 1-pixel outline just inside |rect| (clipped to the image).
void OutlineRect(ModelImage& image, const Rect& rect, const uint8_t rgb[3]);
// Every tag's target outline first, then every label with its text, so no
// outline crosses a label.
void DrawTags(ModelImage& image, const std::vector<Tag>& tags);

// Masks and tags |image|, the model image PrepareImageForModel() made from
// a |capture_width| x |capture_height| capture by |plan|, for the elements
// of |observation| measured in |geometry|. Masks: the geometry's, and every
// listed element agent_safety marks sensitive (password, payment card,
// one-time code), each grown by kMaskMargin. Then a tag per listed element
// in view. false (drop the image) if the image isn't the plan's canvas or
// the capture isn't the viewport measured.
bool AnnotateScreenshot(ModelImage& image,
                        const ImagePlan& plan,
                        uint32_t capture_width,
                        uint32_t capture_height,
                        const agent_actions::Observation& observation,
                        const ScreenshotGeometry& geometry);

// ---------------------------------------------------------------------------
// After a request with an image
// ---------------------------------------------------------------------------

// Whether a failed request that carried a screenshot should be asked again
// without it (and the run go on text-only): the model can't take images
// after all (images_unsupported, e.g. another model was loaded), or the
// generation failed (an image the service refused, a crash or a timeout
// while encoding it). Not for a cancelled request, nor for an unusable
// reply (InterpretReply asks again for those) or an unsupported schema.
bool ShouldRetryWithoutImage(const agent_actions::ModelReply& reply,
                             bool images_unsupported);

}  // namespace agent_vision
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_AGENT_VISION_H_
