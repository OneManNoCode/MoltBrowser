// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox_view.h"

#include <algorithm>

#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_letterbox.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "net/base/url_util.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/controls/webview/webview.h"
#include "url/gurl.h"

namespace molt_ai::fingerprinting {

namespace {

bool IsLetterboxed(content::BrowserContext* context) {
  // kOff for a null context.
  return ModeForBrowserContext(context) == RendererMode::kUniform;
}

// The zoom level content sends the page's renderer together with its size,
// as WebContentsImpl::GetPendingZoomLevel() picks it for a primary main
// frame at 152: the host zoom of the pending navigation's URL while there is
// one, the page's own otherwise (temporary zoom levels included); clamped as
// the renderer clamps it (WebViewImpl::ClampZoomLevel()).
double RendererZoomLevel(content::WebContents& contents) {
  double level = 0.0;
  if (content::NavigationEntry* pending =
          contents.GetController().GetPendingEntry()) {
    const GURL& url = pending->GetURL();
    level = content::HostZoomMap::Get(
                contents.GetPrimaryMainFrame()->GetSiteInstance())
                ->GetZoomLevelForHostAndScheme(url.GetScheme(),
                                               net::GetHostOrSpecFromURL(url));
  } else {
    level = content::HostZoomMap::GetZoomLevel(&contents);
  }
  return std::max(
      blink::ZoomFactorToZoomLevel(blink::kMinimumBrowserZoomFactor),
      std::min(blink::ZoomFactorToZoomLevel(blink::kMaximumBrowserZoomFactor),
               level));
}

// The page's scale as its renderer has it: the device scale factor of its
// RenderWidgetHostView (the one content sends with the page's size;
// `fallback_device_scale_factor` before there is a view, after which its
// first visual properties lay the page out again) and its page zoom factor,
// cast to float as WebFrameWidgetImpl::SetZoomInternal() casts it.
PageScale PageScaleOf(content::WebContents& contents,
                      float fallback_device_scale_factor) {
  const content::RenderWidgetHostView* view =
      contents.GetRenderWidgetHostView();
  return {view ? view->GetDeviceScaleFactor() : fallback_device_scale_factor,
          static_cast<float>(
              blink::ZoomLevelToZoomFactor(RendererZoomLevel(contents)))};
}

}  // namespace

gfx::Insets LetterboxInsets(const views::WebView& view) {
  // No page, nothing to letterbox. A tab's page always comes from the
  // window's profile.
  content::WebContents* contents = view.web_contents();
  if (!contents || !IsLetterboxed(contents->GetBrowserContext())) {
    return gfx::Insets();
  }
  const ui::Layer* layer = view.layer();
  const LetterboxMargins margins = LetterboxMarginsFor(
      view.width(), view.height(),
      PageScaleOf(*contents, layer ? layer->device_scale_factor() : 1.0f));
  return gfx::Insets::TLBR(margins.top, margins.left, margins.bottom,
                           margins.right);
}

bool LetterboxNeedsLayout(const views::WebView& view,
                          const gfx::Rect& page_bounds) {
  content::WebContents* contents = view.web_contents();
  return contents && IsLetterboxed(contents->GetBrowserContext()) &&
         page_bounds != view.GetContentsBounds();
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

// static
void BackgroundTabLetterbox::MaybeCreateForWebContents(
    content::WebContents* web_contents) {
  if (web_contents && IsLetterboxed(web_contents->GetBrowserContext())) {
    CreateForWebContents(web_contents);
  }
}

BackgroundTabLetterbox::BackgroundTabLetterbox(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<BackgroundTabLetterbox>(*web_contents) {}

BackgroundTabLetterbox::~BackgroundTabLetterbox() = default;

void BackgroundTabLetterbox::PrimaryMainFrameWasResized(bool width_changed) {
  Update();
}

void BackgroundTabLetterbox::PrimaryPageChanged(content::Page& page) {
  Update();
}

void BackgroundTabLetterbox::Update() {
  content::WebContents* contents = web_contents();
  // A tab that is shown is laid out by its ContentsWebView; a WebContents
  // that isn't a tab isn't letterboxed.
  const tabs::TabInterface* tab =
      tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab || tab->IsVisible() ||
      !IsLetterboxed(contents->GetBrowserContext())) {
    return;
  }
  const gfx::Size size = contents->GetSize();
  const PageScale scale = PageScaleOf(*contents, 1.0f);
  const gfx::Size page(LetterboxPageLength(size.width(), kLetterboxStepWidth,
                                           kLetterboxMaxWidth, scale),
                       LetterboxPageLength(size.height(), kLetterboxStepHeight,
                                           kLetterboxMaxHeight, scale));
  // Within its size, so a page letterboxed already stays as it is; the
  // resize reaches the renderer right behind what it was just sent.
  if (page != size) {
    contents->Resize(gfx::Rect(page));
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(BackgroundTabLetterbox);

}  // namespace molt_ai::fingerprinting
