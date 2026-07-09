// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A translucent "Liquid Glass" bubble anchored to the toolbar Import button.
// Hosts the chrome://molt-import/ WebUI (detected-browser list + a
// "include passwords" toggle + import actions) in a WebView whose widget,
// frame, and web contents are all made transparent so the page's CSS glass
// reads through. Sibling of MoltNetBubbleView; replaces the tab the Import
// toolbar button used to open.

#ifndef CHROME_BROWSER_UI_VIEWS_TOOLBAR_MOLT_IMPORT_BUBBLE_H_
#define CHROME_BROWSER_UI_VIEWS_TOOLBAR_MOLT_IMPORT_BUBBLE_H_

#include "base/memory/raw_ptr.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace views {
class WebView;
}  // namespace views

class MoltImportBubbleView : public views::BubbleDialogDelegateView {
  METADATA_HEADER(MoltImportBubbleView, views::BubbleDialogDelegateView)

 public:
  // Shows the popover anchored at `anchor`, or toggles it closed if already
  // open. `context` is the browser context the WebView renders under.
  static void Show(views::View* anchor, content::BrowserContext* context);

  MoltImportBubbleView(views::View* anchor, content::BrowserContext* context);
  ~MoltImportBubbleView() override;

  MoltImportBubbleView(const MoltImportBubbleView&) = delete;
  MoltImportBubbleView& operator=(const MoltImportBubbleView&) = delete;

 private:
  raw_ptr<views::WebView> web_view_ = nullptr;
};

#endif  // CHROME_BROWSER_UI_VIEWS_TOOLBAR_MOLT_IMPORT_BUBBLE_H_
