// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"

namespace molt_ai {
namespace voice {

namespace {

bool Finite(float v) {
  return std::isfinite(v);
}

float Clamp(float v, float lo, float hi) {
  return std::max(lo, std::min(v, hi));
}

// The anchors in the order PlaceBadges() tries them.
constexpr BadgeAnchor kAnchors[] = {
    BadgeAnchor::kInsideTopLeft,  BadgeAnchor::kOutsideLeft,
    BadgeAnchor::kAbove,          BadgeAnchor::kInsideTopRight,
    BadgeAnchor::kOutsideRight,   BadgeAnchor::kBelow,
    BadgeAnchor::kInsideBottomLeft, BadgeAnchor::kCenter,
};

OverlayRect AtAnchor(BadgeAnchor anchor,
                     const OverlayRect& item,
                     const OverlaySize& size,
                     float gap) {
  const float w = size.width;
  const float h = size.height;
  switch (anchor) {
    case BadgeAnchor::kInsideTopLeft:
      return {item.x, item.y, w, h};
    case BadgeAnchor::kOutsideLeft:
      return {item.x - w - gap, item.y, w, h};
    case BadgeAnchor::kAbove:
      return {item.x, item.y - h - gap, w, h};
    case BadgeAnchor::kInsideTopRight:
      return {item.right() - w, item.y, w, h};
    case BadgeAnchor::kOutsideRight:
      return {item.right() + gap, item.y, w, h};
    case BadgeAnchor::kBelow:
      return {item.x, item.bottom() + gap, w, h};
    case BadgeAnchor::kInsideBottomLeft:
      return {item.x, item.bottom() - h, w, h};
    case BadgeAnchor::kCenter:
      return {item.x + (item.width - w) / 2, item.y + (item.height - h) / 2, w,
              h};
  }
  return {item.x, item.y, w, h};
}

// |r| moved (not resized) to lie inside |bounds|, which is at least as
// large.
OverlayRect MoveInside(OverlayRect r, const OverlayRect& bounds) {
  r.x = Clamp(r.x, bounds.x, bounds.right() - r.width);
  r.y = Clamp(r.y, bounds.y, bounds.bottom() - r.height);
  return r;
}

// Placed badges, bucketed by area, so a new badge is checked only against
// its neighbours.
class BadgeGrid {
 public:
  BadgeGrid(const OverlayRect& area, float cell) : area_(area) {
    // At most about 64k cells, whatever the area.
    cell_ = std::max(cell, std::max(area.width, area.height) / 256.0f);
    cell_ = std::max(cell_, 1.0f);
    columns_ = static_cast<int>(std::ceil(area.width / cell_)) + 1;
    rows_ = static_cast<int>(std::ceil(area.height / cell_)) + 1;
    cells_.resize(static_cast<size_t>(columns_) * static_cast<size_t>(rows_));
  }

  // Whether |r| comes within |gap| of a placed badge.
  bool Collides(const OverlayRect& r,
                float gap,
                const std::vector<BadgePlacement>& placed) const {
    const OverlayRect probe = r.Outset(gap);
    bool hit = false;
    ForCells(probe, [&](const std::vector<uint32_t>& cell) {
      for (uint32_t index : cell) {
        if (probe.Intersects(placed[index].badge)) {
          hit = true;
          return false;
        }
      }
      return true;
    });
    return hit;
  }

  void Add(const OverlayRect& r, uint32_t index) {
    ForCells(r, [&](std::vector<uint32_t>& cell) {
      cell.push_back(index);
      return true;
    });
  }

 private:
  template <typename Cells, typename Visit>
  static void ForCellsOf(Cells& self, const OverlayRect& r, Visit visit) {
    const int c0 = self.Column(r.x);
    const int c1 = self.Column(r.right());
    const int r0 = self.Row(r.y);
    const int r1 = self.Row(r.bottom());
    for (int row = r0; row <= r1; ++row) {
      for (int column = c0; column <= c1; ++column) {
        if (!visit(self.cells_[static_cast<size_t>(row) * self.columns_ +
                               column])) {
          return;
        }
      }
    }
  }
  template <typename Visit>
  void ForCells(const OverlayRect& r, Visit visit) const {
    ForCellsOf(*this, r, visit);
  }
  template <typename Visit>
  void ForCells(const OverlayRect& r, Visit visit) {
    ForCellsOf(*this, r, visit);
  }

  int Column(float x) const {
    return static_cast<int>(
        Clamp(std::floor((x - area_.x) / cell_), 0, columns_ - 1));
  }
  int Row(float y) const {
    return static_cast<int>(
        Clamp(std::floor((y - area_.y) / cell_), 0, rows_ - 1));
  }

  OverlayRect area_;
  float cell_ = 64;
  int columns_ = 1;
  int rows_ = 1;
  std::vector<std::vector<uint32_t>> cells_;
};

int DigitCount(int number) {
  int digits = 1;
  for (int64_t n = number < 0 ? -static_cast<int64_t>(number) : number;
       n >= 10; n /= 10) {
    ++digits;
  }
  return digits;
}

// sRGB channel to linear light, as WCAG 2 defines it.
double Linear(uint32_t channel) {
  const double c = channel / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double Luminance(OverlayColor color) {
  return 0.2126 * Linear((color >> 16) & 0xFF) +
         0.7152 * Linear((color >> 8) & 0xFF) + 0.0722 * Linear(color & 0xFF);
}

constexpr OverlayColor kBlack = 0xFF000000;
constexpr OverlayColor kWhite = 0xFFFFFFFF;
constexpr OverlayColor kYellow = 0xFFFFE600;

}  // namespace

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

bool OverlayRect::Intersects(const OverlayRect& other) const {
  // The same comparisons as Intersect(), so the two always agree (a
  // sub-pixel overlap that float rounding loses is no overlap).
  return !empty() && !other.empty() &&
         std::max(x, other.x) < std::min(right(), other.right()) &&
         std::max(y, other.y) < std::min(bottom(), other.bottom());
}

bool OverlayRect::Contains(const OverlayRect& other) const {
  return x <= other.x && y <= other.y && other.right() <= right() &&
         other.bottom() <= bottom();
}

OverlayRect OverlayRect::Intersect(const OverlayRect& other) const {
  if (!Intersects(other)) {
    return OverlayRect();
  }
  const float left = std::max(x, other.x);
  const float top = std::max(y, other.y);
  return {left, top, std::min(right(), other.right()) - left,
          std::min(bottom(), other.bottom()) - top};
}

OverlayRect OverlayRect::Outset(float d) const {
  OverlayRect r{x - d, y - d, width + 2 * d, height + 2 * d};
  if (r.empty()) {
    return {x + width / 2, y + height / 2, 0, 0};
  }
  return r;
}

std::optional<OverlayRect> PageRectToView(const PageRect& rect,
                                          const PageToView& to_view) {
  const float scale = to_view.device_scale_factor;
  if (!Finite(scale) || scale < kMinDeviceScaleFactor ||
      scale > kMaxDeviceScaleFactor) {
    return std::nullopt;
  }
  if (!Finite(rect.x) || !Finite(rect.y) || !Finite(rect.width) ||
      !Finite(rect.height) || rect.empty() || !Finite(to_view.origin_x) ||
      !Finite(to_view.origin_y)) {
    return std::nullopt;
  }
  const double x = to_view.origin_x + static_cast<double>(rect.x) / scale;
  const double y = to_view.origin_y + static_cast<double>(rect.y) / scale;
  const double width = static_cast<double>(rect.width) / scale;
  const double height = static_cast<double>(rect.height) / scale;
  const double limit = kMaxOverlayCoordinate;
  if (std::abs(x) > limit || std::abs(y) > limit || width > limit ||
      height > limit || std::abs(x + width) > limit ||
      std::abs(y + height) > limit) {
    return std::nullopt;
  }
  return OverlayRect{static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(width), static_cast<float>(height)};
}

// ---------------------------------------------------------------------------
// The overlay's copy of a map
// ---------------------------------------------------------------------------

std::vector<OverlayItem> OverlayItemsFromMap(const PageMap& map) {
  std::vector<OverlayItem> items;
  const size_t count = std::min(map.items.size(), kMaxOverlayItems);
  items.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const PageItem& item = map.items[i];
    items.push_back({item.number, item.handle, item.bounds, item.on_screen});
  }
  return items;
}

FollowResult FollowPage(std::vector<OverlayItem>& shown,
                        const std::vector<OverlayItem>& fresh) {
  if (fresh.size() < shown.size()) {
    return FollowResult::kStale;
  }
  for (size_t i = 0; i < shown.size(); ++i) {
    if (fresh[i].number != shown[i].number ||
        !(fresh[i].handle == shown[i].handle)) {
      return FollowResult::kStale;
    }
  }
  bool moved = false;
  for (size_t i = 0; i < shown.size(); ++i) {
    const PageRect& a = shown[i].bounds;
    const PageRect& b = fresh[i].bounds;
    // Bitwise, so a NaN from a page counts as a change only once.
    moved |= std::memcmp(&a, &b, sizeof(PageRect)) != 0 ||
             shown[i].on_screen != fresh[i].on_screen;
    shown[i].bounds = b;
    shown[i].on_screen = fresh[i].on_screen;
  }
  return moved ? FollowResult::kMoved : FollowResult::kSame;
}

// ---------------------------------------------------------------------------
// Sizes
// ---------------------------------------------------------------------------

OverlaySizes ComputeOverlaySizes(float ui_font_size, float page_font_size) {
  // The interface font the defaults are drawn for (macOS's; Windows' is
  // 12, Linux's usually larger) and the default page font.
  constexpr float kReferenceUiFont = 13;
  constexpr float kReferencePageFont = 16;
  float scale = 1;
  if (Finite(ui_font_size) && ui_font_size > 0) {
    scale = std::max(scale, ui_font_size / kReferenceUiFont);
  }
  if (Finite(page_font_size) && page_font_size > 0) {
    scale = std::max(scale, page_font_size / kReferencePageFont);
  }
  scale = Clamp(scale, 1, 3);

  OverlaySizes sizes;
  sizes.scale = scale;
  sizes.badge_font *= scale;
  sizes.badge_min_font *= scale;
  sizes.caption_font *= scale;
  sizes.caption_min_font *= scale;
  sizes.ring_width = 4 + 2 * scale;
  return sizes;
}

// ---------------------------------------------------------------------------
// Badges
// ---------------------------------------------------------------------------

OverlaySize BadgeSize(int number, const BadgeStyle& style, float scale) {
  const float height = (style.text_height + 2 * style.padding_y) * scale;
  const float width =
      (DigitCount(number) * style.digit_width + 2 * style.padding_x) * scale;
  return {std::max(width, height), height};
}

BadgeScales BadgeScalesFor(const BadgeStyle& style) {
  BadgeScales scales;
  const float min_scale =
      Finite(style.min_scale) ? Clamp(style.min_scale, 0.3f, 1) : 1;
  if (min_scale < 1) {
    scales.scale[1] = (1 + min_scale) / 2;
    scales.scale[2] = min_scale;
    scales.count = 3;
  }
  return scales;
}

namespace {

// Places badges for |requests| in |order|, each tried from scale step
// |first_step[i]| down to the floor. See PlaceBadges().
BadgePlan PlaceInOrder(const std::vector<BadgeRequest>& requests,
                       const std::vector<size_t>& order,
                       const std::vector<size_t>& first_step,
                       int current,
                       const OverlayRect& viewport,
                       const BadgeStyle& style,
                       float gap,
                       float ring_width) {
  BadgePlan plan;
  const BadgeScales scales = BadgeScalesFor(style);
  const OverlaySize full = BadgeSize(1, style, 1);
  BadgeGrid grid(viewport, std::max(64.0f, 2 * full.height));
  for (size_t index : order) {
    const BadgeRequest& request = requests[index];
    const OverlayRect visible = request.item.Intersect(viewport);
    if (visible.empty()) {
      continue;
    }
    const bool is_current = current > 0 && request.number == current;
    if (is_current) {
      const OverlayRect ring = request.item.Outset(ring_width / 2 + 1)
                                   .Intersect(viewport.Outset(-ring_width / 2));
      if (!ring.empty()) {
        plan.ring = ring;
      }
    }
    if (plan.badges.size() >= kMaxBadges) {
      ++plan.over_cap;
      continue;
    }
    bool placed = false;
    for (size_t step = first_step[index]; step < scales.count && !placed;
         ++step) {
      const float scale = scales.scale[step];
      const OverlaySize size = BadgeSize(request.number, style, scale);
      if (size.width > viewport.width || size.height > viewport.height) {
        continue;
      }
      for (BadgeAnchor anchor : kAnchors) {
        const OverlayRect badge =
            MoveInside(AtAnchor(anchor, visible, size, gap), viewport);
        // Moved inside the viewport, it must still be at its item.
        if (!badge.Outset(gap + 1).Intersects(visible) ||
            grid.Collides(badge, gap, plan.badges)) {
          continue;
        }
        grid.Add(badge, static_cast<uint32_t>(plan.badges.size()));
        plan.badges.push_back(
            {request.number, request.item, badge, scale, anchor, is_current});
        if (scale < 1) {
          ++plan.shrunk;
        }
        placed = true;
        break;
      }
    }
    if (!placed) {
      ++plan.crowded;
    }
  }
  return plan;
}

// For each request, the first scale step at which its badge would clear
// the corner of its nearest neighbour (the corner badges sit on first):
// items closer together than a full-size badge start smaller. Neighbours
// are found through a grid of corners, so this stays linear.
std::vector<size_t> StepsByDensity(const std::vector<BadgeRequest>& requests,
                                   const OverlayRect& viewport,
                                   const BadgeStyle& style,
                                   float gap) {
  const BadgeScales scales = BadgeScalesFor(style);
  const float full = BadgeSize(1, style, 1).height + gap;
  std::vector<size_t> steps(requests.size(), 0);
  if (!(full > 0) || scales.count < 2) {
    return steps;
  }
  // Corners in cells at least a full badge wide (so the 3 by 3 cells
  // around a corner hold every corner nearer than that), at most about 64k
  // cells whatever the viewport.
  const float cell_size =
      std::max(full, std::max(viewport.width, viewport.height) / 256);
  const int columns = static_cast<int>(viewport.width / cell_size) + 1;
  const int rows = static_cast<int>(viewport.height / cell_size) + 1;
  auto cell_of = [&](float v, float origin, int count) {
    return static_cast<int>(Clamp(std::floor((v - origin) / cell_size), 0,
                                  static_cast<float>(count - 1)));
  };
  std::vector<std::vector<uint32_t>> cells(static_cast<size_t>(columns) *
                                           static_cast<size_t>(rows));
  std::vector<OverlayRect> visible(requests.size());
  for (size_t i = 0; i < requests.size(); ++i) {
    visible[i] = requests[i].item.Intersect(viewport);
    if (visible[i].empty()) {
      continue;
    }
    const size_t cell =
        static_cast<size_t>(cell_of(visible[i].y, viewport.y, rows)) *
            static_cast<size_t>(columns) +
        static_cast<size_t>(cell_of(visible[i].x, viewport.x, columns));
    cells[cell].push_back(static_cast<uint32_t>(i));
  }
  for (size_t i = 0; i < requests.size(); ++i) {
    if (visible[i].empty()) {
      continue;
    }
    const int column = cell_of(visible[i].x, viewport.x, columns);
    const int row = cell_of(visible[i].y, viewport.y, rows);
    float nearest = full;
    for (int r = std::max(0, row - 1); r <= std::min(rows - 1, row + 1); ++r) {
      for (int c = std::max(0, column - 1); c <= std::min(columns - 1, column + 1);
           ++c) {
        for (uint32_t j : cells[static_cast<size_t>(r) *
                                    static_cast<size_t>(columns) +
                                static_cast<size_t>(c)]) {
          if (j == i) {
            continue;
          }
          const float distance =
              std::max(std::abs(visible[j].x - visible[i].x),
                       std::abs(visible[j].y - visible[i].y));
          nearest = std::min(nearest, distance);
        }
      }
    }
    size_t step = 0;
    while (step + 1 < scales.count && scales.scale[step] * full > nearest) {
      ++step;
    }
    steps[i] = step;
  }
  return steps;
}

}  // namespace

BadgePlan PlaceBadges(const std::vector<BadgeRequest>& requests,
                      int current,
                      const OverlayRect& viewport,
                      const BadgeStyle& style,
                      float ring_width) {
  if (viewport.empty() || !Finite(viewport.x) || !Finite(viewport.y) ||
      !Finite(viewport.width) || !Finite(viewport.height)) {
    return BadgePlan();
  }
  const float gap = Finite(style.gap) ? std::max(style.gap, 0.0f) : 0;

  // The current item first, then the rest in number order.
  std::vector<size_t> order;
  order.reserve(requests.size());
  for (size_t i = 0; i < requests.size(); ++i) {
    if (current > 0 && requests[i].number == current) {
      order.insert(order.begin(), i);
    } else {
      order.push_back(i);
    }
  }

  // Every badge at full size where it fits, smaller only where nothing
  // else is left: the largest badges, moved aside where items are close.
  BadgePlan plan =
      PlaceInOrder(requests, order, std::vector<size_t>(requests.size(), 0),
                   current, viewport, style, gap, ring_width);
  if (plan.crowded == 0) {
    return plan;
  }
  // Some items got no badge. Where items are closer together than a badge,
  // full-size badges take the room smaller ones could share: start those
  // smaller, and keep that plan if it numbers more items.
  BadgePlan dense =
      PlaceInOrder(requests, order,
                   StepsByDensity(requests, viewport, style, gap), current,
                   viewport, style, gap, ring_width);
  if (dense.badges.size() > plan.badges.size()) {
    return dense;
  }
  return plan;
}

std::vector<BadgeRequest> VisibleItems(const std::vector<OverlayItem>& items,
                                       const PageToView& to_view,
                                       const OverlayRect& viewport) {
  std::vector<BadgeRequest> requests;
  for (const OverlayItem& item : items) {
    if (!item.on_screen) {
      continue;
    }
    const std::optional<OverlayRect> rect =
        PageRectToView(item.bounds, to_view);
    if (rect && rect->Intersects(viewport)) {
      requests.push_back({item.number, *rect});
    }
  }
  return requests;
}

// ---------------------------------------------------------------------------
// The caption bar
// ---------------------------------------------------------------------------

std::string_view VoiceStateLabel(VoiceState state) {
  switch (state) {
    case VoiceState::kOff:
      return "Off";
    case VoiceState::kWaiting:
      return "Waiting";
    case VoiceState::kListening:
      return "Listening";
    case VoiceState::kThinking:
      return "Thinking";
    case VoiceState::kSpeaking:
      return "Speaking";
  }
  return "Off";
}

CaptionText MakeCaptionText(VoiceState state,
                            std::string_view heard,
                            std::string_view status,
                            std::string_view word) {
  CaptionText caption;
  if (!word.empty() && word.size() <= kMaxCaptionWordChars &&
      std::all_of(word.begin(), word.end(),
                  [](char c) { return c >= 'a' && c <= 'z'; })) {
    caption.word = std::string(word);
  }
  caption.state = state;
  caption.state_label = std::string(VoiceStateLabel(state));
  const std::string heard_text = SpeechSafeText(heard, kMaxCaptionHeardChars);
  if (!heard_text.empty()) {
    caption.heard = std::string(kHeardPrefix) + heard_text;
  }
  caption.status = SpeechSafeText(status, kMaxCaptionStatusChars);
  return caption;
}

float CaptionBarHeight(const CaptionMetrics& metrics) {
  // Padding above, between the lines (half) and below.
  return metrics.border + 2.5f * metrics.padding + 2 * metrics.line_height;
}

CaptionMetrics FitCaption(const CaptionMetrics& preferred,
                          float min_font,
                          float area_height) {
  if (!Finite(area_height) || area_height <= 0 || !(preferred.font > 0)) {
    return preferred;
  }
  const float budget = area_height / 3 - preferred.border;
  const float scalable = 2.5f * preferred.padding + 2 * preferred.line_height;
  if (scalable <= budget) {
    return preferred;
  }
  const float floor = Finite(min_font) ? Clamp(min_font / preferred.font, 0, 1)
                                       : 1;
  const float factor = Clamp(budget > 0 ? budget / scalable : 0, floor, 1);
  CaptionMetrics fitted = preferred;
  fitted.font *= factor;
  fitted.line_height *= factor;
  fitted.padding *= factor;
  return fitted;
}

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------

OverlayPalette ChoosePalette(bool forced_colors, const SystemColors& system) {
  OverlayPalette palette;
  if (forced_colors) {
    palette.badge_fill = system.highlight;
    palette.badge_text = system.highlight_text;
    palette.badge_border = system.window_text;
    palette.current_fill = system.window;
    palette.current_text = system.window_text;
    palette.current_border = system.highlight;
    palette.ring_band = system.highlight;
    palette.ring_edge = system.window_text;
    palette.caption_fill = system.window;
    palette.caption_text = system.window_text;
    palette.caption_border = system.window_text;
    palette.state_text = system.highlight_text;
    std::fill(std::begin(palette.state_fill), std::end(palette.state_fill),
              system.highlight);
    return palette;
  }
  palette.badge_fill = kYellow;
  palette.badge_text = kBlack;
  palette.badge_border = kBlack;
  palette.current_fill = kBlack;
  palette.current_text = kYellow;
  palette.current_border = kYellow;
  palette.ring_band = kYellow;
  palette.ring_edge = kBlack;
  palette.caption_fill = kBlack;
  palette.caption_text = kWhite;
  palette.caption_border = kYellow;
  palette.state_text = kBlack;
  palette.state_fill[static_cast<int>(VoiceState::kOff)] = 0xFFC8C8C8;
  palette.state_fill[static_cast<int>(VoiceState::kWaiting)] = kWhite;
  palette.state_fill[static_cast<int>(VoiceState::kListening)] = kYellow;
  palette.state_fill[static_cast<int>(VoiceState::kThinking)] = 0xFF9CD3FF;
  palette.state_fill[static_cast<int>(VoiceState::kSpeaking)] = 0xFF9EF0A0;
  return palette;
}

OverlayColor StateFill(const OverlayPalette& palette, VoiceState state) {
  const size_t index = static_cast<size_t>(state);
  return index < std::size(palette.state_fill) ? palette.state_fill[index]
                                               : palette.state_fill[0];
}

double ContrastRatio(OverlayColor a, OverlayColor b) {
  const double la = Luminance(a);
  const double lb = Luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

}  // namespace voice
}  // namespace molt_ai
