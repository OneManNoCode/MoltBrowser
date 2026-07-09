// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/toolbar/molt_net_bubble.h"

#include <memory>
#include <utility>

#include "base/time/time.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/browser_context.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace {

// The single live popover (bubbles are singletons; a second click toggles it
// closed). `g_closed_at` swallows the immediate reopen when the click that
// dismissed the bubble via close-on-deactivate is the same click on the globe.
MoltNetBubbleView* g_instance = nullptr;
base::TimeTicks g_closed_at;

}  // namespace

MoltNetBubbleView::MoltNetBubbleView(views::View* anchor,
                                     content::BrowserContext* context)
    : views::BubbleDialogDelegateView(
          views::BubbleDialogDelegateView::CreatePassKey(),
          anchor,
          views::BubbleBorder::TOP_RIGHT) {
  SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  set_margins(gfx::Insets());
  // Punch the frame transparent so the page's CSS glass is what shows; the
  // widget layer is already translucent for bubbles. The web contents is made
  // transparent below. NO_SHADOW because the page draws its own drop shadow.
  SetBackgroundColor(SK_ColorTRANSPARENT);
  set_shadow(views::BubbleBorder::NO_SHADOW);
  set_close_on_deactivate(true);

  SetLayoutManager(std::make_unique<views::FillLayout>());
  auto web_view = std::make_unique<views::WebView>(context);
  web_view->SetPreferredSize(gfx::Size(344, 476));
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      web_view->GetWebContents(), SK_ColorTRANSPARENT);
  web_view->set_allow_accelerators(true);
  web_view->LoadInitialURL(GURL(chrome::kChromeUIMoltNetURL));
  web_view_ = AddChildView(std::move(web_view));

  g_instance = this;
}

MoltNetBubbleView::~MoltNetBubbleView() {
  if (g_instance == this) {
    g_instance = nullptr;
    g_closed_at = base::TimeTicks::Now();
  }
}

// static
void MoltNetBubbleView::Show(views::View* anchor,
                             content::BrowserContext* context) {
  // Re-click on an open bubble toggles it closed.
  if (g_instance) {
    if (views::Widget* widget = g_instance->GetWidget()) {
      widget->Close();
    }
    return;
  }
  // If the bubble just dismissed itself on deactivation (the same click landed
  // on the globe), don't immediately reopen — read the click as the toggle.
  if (!g_closed_at.is_null() &&
      base::TimeTicks::Now() - g_closed_at < base::Milliseconds(200)) {
    return;
  }

  auto delegate = std::make_unique<MoltNetBubbleView>(anchor, context);
  views::Widget* widget =
      views::BubbleDialogDelegateView::CreateBubble(std::move(delegate));
  widget->Show();
}

BEGIN_METADATA(MoltNetBubbleView)
END_METADATA
