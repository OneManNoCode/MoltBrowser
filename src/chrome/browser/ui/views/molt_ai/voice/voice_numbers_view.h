// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's numbers over a page (docs/design/TOUCHLESS_BROWSING.md
// §5.3): a large, high-contrast badge on each on-screen item of the page
// map, and a thick ring around the current item. One per contents area of
// a browser window (ContentsContainerView), laid over the area's
// ContentsWebView; it draws the VoiceOverlay (voice_overlay.h) of the tab
// that view shows.
//
// Like the other views ContentsContainerView lays over a page (the
// enterprise watermark, the tab-capture border), it paints to its own
// layer above the page's, takes no events (clicks go to the page), never
// takes focus, and is invisible to assistive technology: screen readers
// and OS voice control have their own numbers. It changes nothing in the
// page. It is hidden while there is nothing to draw.

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_NUMBERS_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_NUMBERS_VIEW_H_

#include <vector>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/font_list.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

namespace gfx {
class Canvas;
}  // namespace gfx

namespace views {
class WebView;
}  // namespace views

namespace molt_ai {
namespace voice {

class VoiceNumbersView : public views::View,
                         public views::ViewObserver,
                         public VoiceOverlay::NumbersSink {
  METADATA_HEADER(VoiceNumbersView, views::View)

 public:
  // |contents_view| shows the page; this view is laid out over it.
  explicit VoiceNumbersView(views::WebView* contents_view);
  VoiceNumbersView(const VoiceNumbersView&) = delete;
  VoiceNumbersView& operator=(const VoiceNumbersView&) = delete;
  ~VoiceNumbersView() override;

  // What is drawn now (view coordinates).
  const BadgePlan& plan() const { return plan_; }
  views::WebView* contents_view() const { return contents_view_; }
  const OverlaySizes& sizes() const { return sizes_; }
  // The page's viewport in this view's coordinates.
  OverlayRect PageViewport() const;

  // VoiceOverlay::NumbersSink:
  content::WebContents* GetShownContents() const override;
  void OnOverlayChanged(const VoiceOverlay* overlay) override;

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override;
  void OnThemeChanged() override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  void OnWebContentsAttached(views::WebView* web_view);
  void OnWebContentsDetached(views::WebView* web_view);
  // Sizes, fonts and colours from the user's settings and the theme.
  void UpdateStyle();
  // Places the badges again (the items, the view or the page moved).
  void Replan();
  void PaintRing(gfx::Canvas* canvas, const OverlayRect& ring) const;
  void PaintBadge(gfx::Canvas* canvas, const BadgePlacement& badge) const;

  raw_ptr<views::WebView> contents_view_;
  base::CallbackListSubscription attached_;
  base::CallbackListSubscription detached_;
  base::ScopedMultiSourceObservation<views::View, views::ViewObserver>
      observations_{this};

  // The shown tab's overlay, as last reported.
  bool showing_ = false;
  // Numbers on every item, or only the current one's with its ring.
  bool numbers_shown_ = false;
  std::vector<OverlayItem> items_;
  int current_ = 0;
  float page_scale_factor_ = 1;

  OverlaySizes sizes_;
  BadgeStyle style_;
  OverlayPalette palette_;
  // The badges' font at full size and at the two smaller steps.
  gfx::FontList badge_fonts_[3];
  BadgePlan plan_;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_NUMBERS_VIEW_H_
