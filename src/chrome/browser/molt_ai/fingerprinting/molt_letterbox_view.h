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
//
// Tabs are letterboxed in MoltNet profiles while fingerprinting protection
// is on (molt_fingerprinting_browser.h, RendererMode::kUniform): exactly the
// tabs whose renderers report the uniform screen.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_VIEW_H_

#include <optional>

#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/insets.h"

namespace views {
class WebView;
}  // namespace views

namespace molt_ai::fingerprinting {

// The insets that letterbox the page shown in `view` (a tab's
// ContentsWebView), from the view's current size. Empty unless the page
// belongs to a letterboxed (MoltNet) profile.
gfx::Insets LetterboxInsets(const views::WebView& view);

// The colour of `view`'s letterbox margins: the toolbar's (the surface the
// page sits on, which follows light / dark mode and the theme), or, while the
// tab is fullscreen, Chrome's own colour around letterboxed fullscreen
// content (near black). Always opaque. nullopt unless `view` belongs to a
// letterboxed profile.
std::optional<SkColor> LetterboxMarginColor(views::WebView& view);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_LETTERBOX_VIEW_H_
