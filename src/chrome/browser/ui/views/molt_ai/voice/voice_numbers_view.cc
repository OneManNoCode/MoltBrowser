// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_numbers_view.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_style.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/native/native_view_host.h"
#include "ui/views/controls/webview/webview.h"

namespace molt_ai {
namespace voice {

namespace {

gfx::RectF ToRectF(const OverlayRect& r) {
  return gfx::RectF(r.x, r.y, r.width, r.height);
}

}  // namespace

VoiceNumbersView::VoiceNumbersView(views::WebView* contents_view)
    : contents_view_(contents_view) {
  // Above the page's layer, see-through, and out of the way: clicks reach
  // the page, focus stays in it, and assistive technology skips it.
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  layer()->SetName("VoiceNumbersView");
  SetCanProcessEventsWithinSubtree(false);
  SetFocusBehavior(FocusBehavior::NEVER);
  GetViewAccessibility().SetIsInvisible(true);
  SetVisible(false);

  attached_ = contents_view_->AddWebContentsAttachedCallback(
      base::BindRepeating(&VoiceNumbersView::OnWebContentsAttached,
                          base::Unretained(this)));
  detached_ = contents_view_->AddWebContentsDetachedCallback(
      base::BindRepeating(&VoiceNumbersView::OnWebContentsDetached,
                          base::Unretained(this)));
  observations_.AddObservation(contents_view_);
  observations_.AddObservation(contents_view_->holder());
  VoiceOverlay::AddNumbersSink(this);
}

VoiceNumbersView::~VoiceNumbersView() {
  VoiceOverlay::RemoveNumbersSink(this);
}

OverlayRect VoiceNumbersView::PageViewport() const {
  if (!contents_view_) {
    return OverlayRect();
  }
  // The page's native view, letterboxed in MoltNet windows.
  const views::View* holder = contents_view_->holder();
  gfx::RectF page(holder->GetLocalBounds());
  views::View::ConvertRectToTarget(holder, this, &page);
  page.Intersect(gfx::RectF(GetLocalBounds()));
  return {page.x(), page.y(), page.width(), page.height()};
}

content::WebContents* VoiceNumbersView::GetShownContents() const {
  return contents_view_ ? contents_view_->web_contents() : nullptr;
}

void VoiceNumbersView::OnOverlayChanged(const VoiceOverlay* overlay) {
  const bool was_showing = showing_;
  showing_ = overlay != nullptr;
  if (overlay) {
    items_ = overlay->items();
    numbers_shown_ = overlay->numbers_shown();
    current_ = overlay->current_item();
    page_scale_factor_ = overlay->page_scale_factor();
  } else {
    items_.clear();
    numbers_shown_ = false;
    current_ = 0;
  }
  if (showing_ && !was_showing) {
    UpdateStyle();
  }
  Replan();
  if (GetVisible() != showing_) {
    SetVisible(showing_);
  }
}

void VoiceNumbersView::OnWebContentsAttached(views::WebView* web_view) {
  // A tab comes into this area: draw its overlay, if it has one shown.
  content::WebContents* contents = GetShownContents();
  VoiceOverlay* overlay =
      contents ? VoiceOverlay::FromWebContents(contents) : nullptr;
  const bool shown = overlay && (overlay->numbers_shown() ||
                                 overlay->current_item() > 0);
  OnOverlayChanged(shown ? overlay : nullptr);
}

void VoiceNumbersView::OnWebContentsDetached(views::WebView* web_view) {
  // Its tab is leaving this area; it hides its numbers itself when it is
  // hidden (VoiceOverlay::OnVisibilityChanged()), or another area shows
  // them.
  OnOverlayChanged(nullptr);
}

void VoiceNumbersView::UpdateStyle() {
  content::WebContents* contents = GetShownContents();
  const VoiceOverlayStyle style = VoiceOverlayStyle::For(*this, contents);
  sizes_ = style.sizes;
  palette_ = style.palette;
  style_ = style.badge_style;
  for (size_t i = 0; i < kBadgeScaleSteps; ++i) {
    badge_fonts_[i] = style.badge_fonts[i];
  }
}

void VoiceNumbersView::Replan() {
  plan_ = BadgePlan();
  content::WebContents* contents = GetShownContents();
  if (!showing_ || !contents || items_.empty()) {
    SchedulePaint();
    return;
  }
#if BUILDFLAG(IS_MAC)
  // On macOS the accessibility trees leave out the pinch zoom and the
  // visual viewport's offset, which the browser doesn't know here: badges
  // would land in the wrong places, so none are drawn.
  if (std::abs(page_scale_factor_ - 1) > 0.001f) {
    SchedulePaint();
    return;
  }
#endif
  const content::RenderWidgetHostView* page_view =
      contents->GetRenderWidgetHostView();
  const float scale_factor = page_view ? page_view->GetDeviceScaleFactor()
                                       : layer()->device_scale_factor();
  const OverlayRect viewport = PageViewport();
  // The page's coordinates start at its native view's origin (which
  // PageViewport() may have clipped away).
  gfx::Point origin;
  views::View::ConvertPointToTarget(contents_view_->holder(), this, &origin);
  const PageToView to_view = {static_cast<float>(origin.x()),
                              static_cast<float>(origin.y()), scale_factor};
  std::vector<BadgeRequest> requests = VisibleItems(items_, to_view, viewport);
  if (!numbers_shown_) {
    // The ring alone: the current item keeps its badge.
    std::erase_if(requests, [this](const BadgeRequest& request) {
      return request.number != current_;
    });
  }
  plan_ = PlaceBadges(requests, current_, viewport, style_, sizes_.ring_width);
  SchedulePaint();
}

void VoiceNumbersView::OnPaint(gfx::Canvas* canvas) {
  if (plan_.ring) {
    PaintRing(canvas, *plan_.ring);
  }
  // The current item's badge last, on top.
  for (auto it = plan_.badges.rbegin(); it != plan_.badges.rend(); ++it) {
    PaintBadge(canvas, *it);
  }
}

void VoiceNumbersView::PaintRing(gfx::Canvas* canvas,
                                 const OverlayRect& ring) const {
  const float width = sizes_.ring_width;
  const float edge = std::max(1.5f, width / 4);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setStyle(cc::PaintFlags::kStroke_Style);
  // The bright band, with a dark line on either side, shows on any page.
  flags.setColor(palette_.ring_band);
  flags.setStrokeWidth(width);
  canvas->DrawRoundRect(ToRectF(ring), width, flags);
  flags.setColor(palette_.ring_edge);
  flags.setStrokeWidth(edge);
  canvas->DrawRoundRect(ToRectF(ring.Outset(width / 2)), width * 1.5f, flags);
  const OverlayRect inner = ring.Outset(-width / 2);
  if (!inner.empty()) {
    canvas->DrawRoundRect(ToRectF(inner), width / 2, flags);
  }
}

void VoiceNumbersView::PaintBadge(gfx::Canvas* canvas,
                                  const BadgePlacement& badge) const {
  const BadgeScales scales = BadgeScalesFor(style_);
  size_t step = 0;
  for (size_t i = 0; i < scales.count; ++i) {
    if (std::abs(scales.scale[i] - badge.scale) < 0.001f) {
      step = i;
    }
  }
  const gfx::RectF rect = ToRectF(badge.badge);
  const float radius = rect.height() / 4;
  const float border = std::max(1.5f, 2 * badge.scale);
  cc::PaintFlags flags;
  flags.setAntiAlias(true);
  flags.setStyle(cc::PaintFlags::kFill_Style);
  flags.setColor(badge.current ? palette_.current_fill : palette_.badge_fill);
  canvas->DrawRoundRect(rect, radius, flags);
  flags.setStyle(cc::PaintFlags::kStroke_Style);
  flags.setStrokeWidth(border);
  flags.setColor(badge.current ? palette_.current_border
                               : palette_.badge_border);
  gfx::RectF stroke = rect;
  stroke.Inset(border / 2);
  canvas->DrawRoundRect(stroke, radius, flags);
  canvas->DrawStringRectWithFlags(
      base::NumberToString16(badge.number), badge_fonts_[step],
      badge.current ? palette_.current_text : palette_.badge_text,
      gfx::ToEnclosingRect(rect), gfx::Canvas::TEXT_ALIGN_CENTER);
}

void VoiceNumbersView::OnBoundsChanged(const gfx::Rect& previous_bounds) {
  Replan();
}

void VoiceNumbersView::OnThemeChanged() {
  views::View::OnThemeChanged();
  if (showing_) {
    UpdateStyle();
    Replan();
  }
}

void VoiceNumbersView::OnViewBoundsChanged(views::View* observed_view) {
  if (showing_) {
    Replan();
  }
}

void VoiceNumbersView::OnViewIsDeleting(views::View* observed_view) {
  observations_.RemoveObservation(observed_view);
  if (observed_view == contents_view_) {
    if (contents_view_->holder() &&
        observations_.IsObservingSource(contents_view_->holder())) {
      observations_.RemoveObservation(contents_view_->holder());
    }
    attached_ = base::CallbackListSubscription();
    detached_ = base::CallbackListSubscription();
    contents_view_ = nullptr;
    OnOverlayChanged(nullptr);
  }
}

BEGIN_METADATA(VoiceNumbersView)
END_METADATA

}  // namespace voice
}  // namespace molt_ai
