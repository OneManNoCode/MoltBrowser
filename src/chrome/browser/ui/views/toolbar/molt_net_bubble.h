// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A translucent "Liquid Glass" bubble anchored to the toolbar MoltNet globe.
// Hosts the chrome://molt-net/ WebUI (status + enable toggle + exit-country
// picker) in a WebView whose widget, frame, and web contents are all made
// transparent so the page's CSS glass reads through. Replaces the native
// SimpleMenuModel menu the globe button used to open.

#ifndef CHROME_BROWSER_UI_VIEWS_TOOLBAR_MOLT_NET_BUBBLE_H_
#define CHROME_BROWSER_UI_VIEWS_TOOLBAR_MOLT_NET_BUBBLE_H_

#include "base/memory/raw_ptr.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace views {
class WebView;
}  // namespace views

class MoltNetBubbleView : public views::BubbleDialogDelegateView {
  METADATA_HEADER(MoltNetBubbleView, views::BubbleDialogDelegateView)

 public:
  // Shows the popover anchored at `anchor`, or toggles it closed if already
  // open. `context` is the browser context the WebView renders under.
  static void Show(views::View* anchor, content::BrowserContext* context);

  MoltNetBubbleView(views::View* anchor, content::BrowserContext* context);
  ~MoltNetBubbleView() override;

  MoltNetBubbleView(const MoltNetBubbleView&) = delete;
  MoltNetBubbleView& operator=(const MoltNetBubbleView&) = delete;

 private:
  raw_ptr<views::WebView> web_view_ = nullptr;
};

#endif  // CHROME_BROWSER_UI_VIEWS_TOOLBAR_MOLT_NET_BUBBLE_H_
