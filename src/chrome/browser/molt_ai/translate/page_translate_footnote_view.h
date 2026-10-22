// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Translate bubble's footnote in MoltBrowser (where Chrome-branded
// builds show Google Translate's wordmark): that the page is translated on
// this device, the progress with Stop, and why a translation failed, with
// "Choose a model" (molt://ai) when there is no model. Follows the tab's
// PageTranslateStatus as it changes. Design:
// docs/design/PAGE_TRANSLATION.md §4.5.

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_FOOTNOTE_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_FOOTNOTE_VIEW_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/molt_ai/translate/page_translate_messages.h"
#include "chrome/browser/molt_ai/translate/page_translate_status.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/layout/box_layout_view.h"

namespace content {
class WebContents;
}  // namespace content

namespace views {
class Label;
class Link;
}  // namespace views

namespace molt_ai::page_translate {

class PageTranslateFootnoteView : public views::BoxLayoutView,
                                  public PageTranslateStatus::Observer {
  METADATA_HEADER(PageTranslateFootnoteView, views::BoxLayoutView)

 public:
  explicit PageTranslateFootnoteView(content::WebContents* web_contents);
  PageTranslateFootnoteView(const PageTranslateFootnoteView&) = delete;
  PageTranslateFootnoteView& operator=(const PageTranslateFootnoteView&) =
      delete;
  ~PageTranslateFootnoteView() override;

  // PageTranslateStatus::Observer:
  void OnPageTranslateStatusChanged() override;
  void OnPageTranslateStatusDestroyed() override;

 private:
  void Update();
  void OnLinkClicked();

  base::WeakPtr<content::WebContents> web_contents_;
  raw_ptr<PageTranslateStatus> status_ = nullptr;
  raw_ptr<views::Label> label_ = nullptr;
  raw_ptr<views::Link> link_ = nullptr;
  FootnoteLink link_kind_ = FootnoteLink::kNone;
  base::ScopedObservation<PageTranslateStatus, PageTranslateStatus::Observer>
      observation_{this};
};

// The footnote for |web_contents|'s Translate bubble (nullptr if there is no
// tab, as in the bubble's unit tests). translate_bubble_view.cc's
// CreateWordmarkView() returns it in non-Google builds.
std::unique_ptr<views::View> CreatePageTranslateFootnote(
    content::WebContents* web_contents);

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_FOOTNOTE_VIEW_H_
