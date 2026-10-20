// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox_view.h"

#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "ui/color/color_provider.h"
#include "ui/views/controls/webview/webview.h"

namespace molt_ai::fingerprinting {

namespace {

bool IsLetterboxed(content::BrowserContext* context) {
  // kOff for a null context.
  return ModeForBrowserContext(context) == RendererMode::kUniform;
}

}  // namespace

gfx::Insets LetterboxInsets(const views::WebView& view) {
  // No page, nothing to letterbox. A tab's page always comes from the
  // window's profile.
  content::WebContents* contents = view.web_contents();
  if (!contents || !IsLetterboxed(contents->GetBrowserContext())) {
    return gfx::Insets();
  }
  const LetterboxMargins margins =
      LetterboxMarginsFor(view.width(), view.height());
  return gfx::Insets::TLBR(margins.top, margins.left, margins.bottom,
                           margins.right);
}

std::optional<SkColor> LetterboxMarginColor(views::WebView& view) {
  // The view's own profile (the window's), so the colour is right even
  // before a page is attached.
  if (!IsLetterboxed(view.GetBrowserContext())) {
    return std::nullopt;
  }
  const ui::ColorProvider* colors = view.GetColorProvider();
  if (!colors) {
    return std::nullopt;
  }
  content::WebContents* contents = view.web_contents();
  content::WebContentsDelegate* delegate =
      contents ? contents->GetDelegate() : nullptr;
  const bool tab_fullscreen =
      delegate && delegate->IsFullscreenForTabOrPending(contents);
  const ui::ColorId margin =
      tab_fullscreen ? kColorWebContentsBackgroundLetterboxing : kColorToolbar;
  // The margins are a solid-colour layer, which must be opaque.
  return SkColorSetA(colors->GetColor(margin), SK_AlphaOPAQUE);
}

}  // namespace molt_ai::fingerprinting
