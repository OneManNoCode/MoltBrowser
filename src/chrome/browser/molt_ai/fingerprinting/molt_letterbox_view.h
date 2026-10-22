// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet letterboxing in the browser window (see molt_letterbox.h for the
// geometry and docs/FINGERPRINTING.md, "Letterboxing", for the design).
//
// The active tab's page is shown by ContentsWebView, a views::WebView that
// lays the page's native view out in its GetContentsBounds(). Its hooks
// (chrome/browser/ui/views/frame/contents_web_view.{h,cc}, each marked
// "MoltBrowser:"):
//
//   ContentsWebView::GetInsets()                 -> LetterboxInsets
//     (so the page gets the rounded, centred rect; everything else in the
//     content area -- DevTools, overlays, the status bubble, dialogs -- keeps
//     using the view's full bounds)
//   ContentsWebView::UpdateBackgroundColor()     -> LetterboxMarginColor
//     (the view's solid-colour layer, under the page, shows as the margins)
//   ContentsWebView::DidToggleFullscreenModeForTab (repaints the margins)
//   ContentsWebView::PrimaryMainFrameWasResized,
//     PrimaryPageChanged                         -> LetterboxNeedsLayout
//     (the page's renderer has just been sent its zoom, scale and size, or a
//     new page, which may have another zoom, has committed; when a new zoom
//     or scale needs other margins, they are laid out at once, so the page's
//     new size follows in the same task)
//
// The page is letterboxed in CSS pixels, so the margins depend on its zoom
// and on the screen's scale factor, read as content sends them to the
// renderer (PageScaleOf in the .cc).
//
// Tabs that aren't shown (background tabs) aren't in a ContentsWebView:
// BackgroundTabLetterbox letterboxes their pages again, within the size they
// have, whenever their zoom or scale changes (attached to every MoltNet
// WebContents from chrome/browser/universal_web_contents_observers.cc).
//
// Tabs are letterboxed in MoltNet profiles while fingerprinting protection
// is on (molt_fingerprinting_browser.h, RendererMode::kUniform): exactly the
// tabs whose renderers report the uniform screen.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_VIEW_H_

#include <optional>

#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/insets.h"

namespace gfx {
class Rect;
}  // namespace gfx

namespace views {
class WebView;
}  // namespace views

namespace molt_ai::fingerprinting {

// The insets that letterbox the page shown in `view` (a tab's
// ContentsWebView), from the view's current size and the page's zoom and
// scale factor. Empty unless the page belongs to a letterboxed (MoltNet)
// profile.
gfx::Insets LetterboxInsets(const views::WebView& view);

// Whether the page shown in `view`, laid out at `page_bounds` (the bounds of
// the view's holder), must be laid out again: it is letterboxed, and its
// zoom or scale now asks for other margins.
bool LetterboxNeedsLayout(const views::WebView& view,
                          const gfx::Rect& page_bounds);

// The colour of `view`'s letterbox margins: the toolbar's (the surface the
// page sits on, which follows light / dark mode and the theme), or, while the
// tab is fullscreen, Chrome's own colour around letterboxed fullscreen
// content (near black). Always opaque. nullopt unless `view` belongs to a
// letterboxed profile.
std::optional<SkColor> LetterboxMarginColor(views::WebView& view);

// Keeps the page of a MoltNet tab that isn't shown letterboxed. A
// background tab keeps the size it had when it was last shown, or that of
// the tab it was opened from (TabStripModel gives new background tabs the
// active tab's size), and both were letterboxed for another zoom than the
// one its page may have now: a page zoomed in another tab of the same site,
// one opened from a zoomed page, one that navigated to a site zoomed
// differently. When its renderer is sent a new zoom or scale, the page is
// letterboxed again within that size; once the tab is shown,
// ContentsWebView letterboxes it in the whole content area.
class BackgroundTabLetterbox
    : public content::WebContentsObserver,
      public content::WebContentsUserData<BackgroundTabLetterbox> {
 public:
  // For MoltNet WebContents while fingerprinting protection is on; nothing
  // otherwise.
  static void MaybeCreateForWebContents(content::WebContents* web_contents);

  BackgroundTabLetterbox(const BackgroundTabLetterbox&) = delete;
  BackgroundTabLetterbox& operator=(const BackgroundTabLetterbox&) = delete;
  ~BackgroundTabLetterbox() override;

  // content::WebContentsObserver:
  void PrimaryMainFrameWasResized(bool width_changed) override;
  void PrimaryPageChanged(content::Page& page) override;

 private:
  friend class content::WebContentsUserData<BackgroundTabLetterbox>;
  explicit BackgroundTabLetterbox(content::WebContents* web_contents);

  // Letterboxes the page again within its size, unless the tab is shown.
  void Update();

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_VIEW_H_
