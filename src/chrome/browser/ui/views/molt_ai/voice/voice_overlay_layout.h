// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice overlay's geometry, text and colours, as pure functions
// (docs/design/TOUCHLESS_BROWSING.md §5.3, §5.5): where a page item is on
// screen, where its number badge goes so badges never cover each other,
// the focus ring, the caption bar's lines and height, the sizes that
// follow the user's text settings, and the colours (forced colours
// included). voice_numbers_view.h and voice_caption_bar.h draw what these
// compute.
//
// Std-only (with the std-only page map), so molt_ai_unittests and a
// standalone harness build the same code.

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_LAYOUT_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_LAYOUT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"

namespace molt_ai {
namespace voice {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// A rectangle in the overlay view's coordinates, in DIPs.
struct OverlayRect {
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;

  float right() const { return x + width; }
  float bottom() const { return y + height; }
  bool empty() const { return !(width > 0) || !(height > 0); }
  // Whether the two share an area (touching edges don't).
  bool Intersects(const OverlayRect& other) const;
  bool Contains(const OverlayRect& other) const;
  // The shared area; empty if there is none.
  OverlayRect Intersect(const OverlayRect& other) const;
  // Grown by |d| on every side (shrunk for a negative |d|, to empty).
  OverlayRect Outset(float d) const;

  friend bool operator==(const OverlayRect&, const OverlayRect&) = default;
};

// How the page map's coordinates land on the overlay view.
//
// A PageRect is what ax_page_source.h reads from Blink's accessibility
// trees: physical pixels, relative to the top left of the page's viewport
// (the page's own scrolling and every frame's offset applied, the page's
// zoom included, since Blink lays out zoomed). On Windows, Linux and
// ChromeOS the trees' root also applies the pinch zoom and the visual
// viewport's offset; on macOS neither (BrowserAccessibility::
// RelativeToAbsoluteBounds() and AXObject::GetRelativeBounds() at 156).
struct PageToView {
  // Where the page's viewport (the web contents' native view) starts in
  // the overlay view, in DIPs.
  float origin_x = 0;
  float origin_y = 0;
  // Physical pixels per DIP: the page's screen.
  float device_scale_factor = 1;
};

// Caps on what PageRectToView() takes.
inline constexpr float kMinDeviceScaleFactor = 0.25f;
inline constexpr float kMaxDeviceScaleFactor = 8;
inline constexpr float kMaxOverlayCoordinate = 1000000;

// |rect| in the overlay view's coordinates; nullopt for an empty or
// non-finite rect, one beyond kMaxOverlayCoordinate, or a scale out of
// range.
std::optional<OverlayRect> PageRectToView(const PageRect& rect,
                                          const PageToView& to_view);

// ---------------------------------------------------------------------------
// What the overlay keeps of a page map
// ---------------------------------------------------------------------------

// One numbered item, as the overlay draws it.
struct OverlayItem {
  int number = 0;
  PageNodeHandle handle;
  PageRect bounds;
  bool on_screen = true;
};

// Caps on the overlay's own copy of a map.
inline constexpr size_t kMaxOverlayItems = 2000;

// The map's items, in number order (at most kMaxOverlayItems).
std::vector<OverlayItem> OverlayItemsFromMap(const PageMap& map);

enum class FollowResult {
  kStale,  // the numbers moved: hide them
  kSame,   // nothing moved
  kMoved,  // the items moved (scrolling, layout) under the same numbers
};

// Brings |shown| up to date with |fresh|, a later map of the same page,
// when the numbers the user sees still name the same things: every item of
// |shown| is in |fresh| under its number (items added after the last are
// fine; they aren't shown until the voice controller maps the page again).
// Then the bounds and on-screen marks are taken from |fresh|. Otherwise (an
// item before the last came or went, so numbers moved) |shown| is left as
// it was and the result is kStale: the numbers on screen are stale.
FollowResult FollowPage(std::vector<OverlayItem>& shown,
                        const std::vector<OverlayItem>& fresh);

// ---------------------------------------------------------------------------
// Sizes from the user's settings
// ---------------------------------------------------------------------------

struct OverlaySizes {
  // How much larger than the defaults the user's settings make text.
  float scale = 1;
  // The badges' number, at full size and at the smallest a crowded badge
  // may shrink to, in DIPs.
  float badge_font = 22;
  float badge_min_font = 14;
  // The caption bar's text.
  float caption_font = 26;
  float caption_min_font = 16;
  // The focus ring's width.
  float ring_width = 6;
};

// The defaults are for the platforms' usual 12 to 13 DIP interface font
// and a 16 px default page font. |ui_font_size| is the browser's interface
// font in DIPs (it follows the OS text size on Windows and Linux);
// |page_font_size| is the user's default font size for pages in CSS
// pixels (Settings, Appearance, Font size; 16 by default). The larger of
// the two scales everything, from 1 to 3 times.
OverlaySizes ComputeOverlaySizes(float ui_font_size, float page_font_size);

// ---------------------------------------------------------------------------
// Badges
// ---------------------------------------------------------------------------

struct BadgeStyle {
  // The text's line height and the widest digit's advance, at full size.
  float text_height = 26;
  float digit_width = 14;
  float padding_x = 6;
  float padding_y = 2;
  // Room kept between two badges.
  float gap = 2;
  // A badge that has no room at full size shrinks in steps down to this
  // fraction of it (OverlaySizes::badge_min_font / badge_font).
  float min_scale = 0.64f;
};

struct OverlaySize {
  float width = 0;
  float height = 0;
};

// The badge for |number| at |scale|: one line of text, at least as wide as
// it is high.
OverlaySize BadgeSize(int number, const BadgeStyle& style, float scale);

// The sizes a badge is tried at, largest first: full size, then two steps
// down to the floor (style.min_scale, from 0.3 to 1).
inline constexpr size_t kBadgeScaleSteps = 3;
struct BadgeScales {
  float scale[kBadgeScaleSteps] = {1, 1, 1};
  size_t count = 1;
};
BadgeScales BadgeScalesFor(const BadgeStyle& style);

// Where a badge sits relative to its item, in the order they are tried.
enum class BadgeAnchor : uint8_t {
  kInsideTopLeft,  // over the item's top left corner (as OS voice control)
  kOutsideLeft,    // left of the item, top aligned
  kAbove,          // above the item, left aligned
  kInsideTopRight,
  kOutsideRight,
  kBelow,
  kInsideBottomLeft,
  kCenter,
};

struct BadgeRequest {
  int number = 0;
  OverlayRect item;  // in view coordinates, not clipped
};

struct BadgePlacement {
  int number = 0;
  OverlayRect item;
  OverlayRect badge;
  float scale = 1;
  BadgeAnchor anchor = BadgeAnchor::kInsideTopLeft;
  bool current = false;
};

struct BadgePlan {
  // Placed badges, the current item's first. No two overlap, each is
  // wholly inside the viewport and touches or is within a gap of its item.
  std::vector<BadgePlacement> badges;
  // The current item's focus ring (the centre line of its stroke), when it
  // is on screen.
  std::optional<OverlayRect> ring;
  // On-screen items that got no badge: no readable spot near them was
  // free, even at the smallest size (the voice controller can suggest
  // zooming in).
  size_t crowded = 0;
  // Badges placed smaller than full size.
  size_t shrunk = 0;
  // Items left out because kMaxBadges were placed.
  size_t over_cap = 0;
};

inline constexpr size_t kMaxBadges = 500;

// Badges for |requests| (items in view coordinates, in number order) within
// |viewport| (the page's visible area in the view). Items that don't reach
// into the viewport get none. The current item (|current|, 0 for none) is
// placed first; then each item in turn takes the first anchor where its
// badge is free of the others, at full size, then at smaller sizes down to
// the floor. If that leaves items without a badge, the placement is tried
// again with the items closer together than a full-size badge starting
// smaller, and the plan that numbers more items is kept (a tie keeps the
// larger badges). Linear in the number of items (grids hold the badges and
// the items' corners).
BadgePlan PlaceBadges(const std::vector<BadgeRequest>& requests,
                      int current,
                      const OverlayRect& viewport,
                      const BadgeStyle& style,
                      float ring_width);

// The requests for |items|: those marked on screen, converted to view
// coordinates, that reach into |viewport|.
std::vector<BadgeRequest> VisibleItems(const std::vector<OverlayItem>& items,
                                       const PageToView& to_view,
                                       const OverlayRect& viewport);

// ---------------------------------------------------------------------------
// The caption bar
// ---------------------------------------------------------------------------

// Voice mode's states (TOUCHLESS_BROWSING.md §5.1).
enum class VoiceState : uint8_t {
  kOff,        // not listening
  kWaiting,    // listening for the wake phrase only
  kListening,  // a command is being spoken
  kThinking,
  kSpeaking,
};

// "Off", "Waiting", "Listening", "Thinking", "Speaking".
std::string_view VoiceStateLabel(VoiceState state);

// The words the overlay shows itself (English for now, like the grammar).
inline constexpr std::string_view kCaptionAccessibleName = "Voice mode";
inline constexpr std::string_view kHeardPrefix = "Heard: ";

// Caps on the caption's text, in code points, before the prefix.
inline constexpr size_t kMaxCaptionHeardChars = 200;
inline constexpr size_t kMaxCaptionStatusChars = 200;

struct CaptionText {
  VoiceState state = VoiceState::kOff;
  std::string state_label;
  // "Heard: play the live one by Queen", or empty.
  std::string heard;
  // What is happening ("Playing: Bohemian Rhapsody, Live Aid"), or empty.
  std::string status;

  friend bool operator==(const CaptionText&, const CaptionText&) = default;
};

// The caption for what the voice controller says. |heard| (what the
// recognizer heard) and |status| (which may quote the page) are untrusted:
// both go through SpeechSafeText() and are capped.
CaptionText MakeCaptionText(VoiceState state,
                            std::string_view heard,
                            std::string_view status);

struct CaptionMetrics {
  float font = 26;
  float line_height = 32;
  float padding = 10;
  float border = 3;  // the line above the bar
};

// The caption bar's height: two lines (the state and what was heard; what
// is happening) whatever they say, so the page's size never depends on the
// text: it changes only when the bar is shown or hidden.
float CaptionBarHeight(const CaptionMetrics& metrics);

// The caption's metrics for a page area |area_height| DIPs tall: |preferred|
// as it is when the bar takes at most a third of the area, otherwise with
// a smaller font (the line height scales with it), never below |min_font|.
CaptionMetrics FitCaption(const CaptionMetrics& preferred,
                          float min_font,
                          float area_height);

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

// 0xAARRGGBB, as SkColor.
using OverlayColor = uint32_t;

// The OS's colours in forced-colours (high contrast) mode: Window,
// WindowText, Highlight and HighlightText.
struct SystemColors {
  OverlayColor window = 0xFFFFFFFF;
  OverlayColor window_text = 0xFF000000;
  OverlayColor highlight = 0xFF0078D7;
  OverlayColor highlight_text = 0xFFFFFFFF;
};

struct OverlayPalette {
  OverlayColor badge_fill = 0;
  OverlayColor badge_text = 0;
  OverlayColor badge_border = 0;
  // The current item's badge, inverted.
  OverlayColor current_fill = 0;
  OverlayColor current_text = 0;
  OverlayColor current_border = 0;
  // The focus ring: a bright band between two dark lines, visible on any
  // page.
  OverlayColor ring_band = 0;
  OverlayColor ring_edge = 0;
  OverlayColor caption_fill = 0;
  OverlayColor caption_text = 0;
  OverlayColor caption_border = 0;
  // The state's label; a fill per state (with the same text colour), so
  // the states differ by colour as well as by word.
  OverlayColor state_text = 0;
  OverlayColor state_fill[5] = {};
};

// The overlay's colours: fixed high-contrast ones (black on yellow badges,
// white on black caption; every text pair at least 7:1), or in forced
// colours mode only the OS's.
OverlayPalette ChoosePalette(bool forced_colors, const SystemColors& system);

OverlayColor StateFill(const OverlayPalette& palette, VoiceState state);

// WCAG 2's contrast ratio of two opaque colours (1 to 21).
double ContrastRatio(OverlayColor a, OverlayColor b);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_LAYOUT_H_
