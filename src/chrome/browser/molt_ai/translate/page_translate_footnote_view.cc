// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_footnote_view.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/link.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "url/gurl.h"

namespace molt_ai::page_translate {

PageTranslateFootnoteView::PageTranslateFootnoteView(
    content::WebContents* web_contents)
    : web_contents_(web_contents->GetWeakPtr()),
      status_(PageTranslateStatus::FromWebContents(web_contents)) {
  SetOrientation(views::BoxLayout::Orientation::kVertical);
  SetCrossAxisAlignment(views::BoxLayout::CrossAxisAlignment::kStretch);
  SetBetweenChildSpacing(4);

  label_ = AddChildView(std::make_unique<views::Label>(
      u"", views::style::CONTEXT_LABEL, views::style::STYLE_SECONDARY));
  label_->SetMultiLine(true);
  label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);

  link_ = AddChildView(std::make_unique<views::Link>(u""));
  link_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  link_->SetCallback(base::BindRepeating(
      &PageTranslateFootnoteView::OnLinkClicked, base::Unretained(this)));

  if (status_) {
    observation_.Observe(status_.get());
    if (status_->snapshot().phase == Phase::kIdle) {
      // Whether there is a model to translate with; the answer updates the
      // text. Nothing is downloaded.
      status_->RefreshDeviceFacts();
    }
  }
  Update();
}

PageTranslateFootnoteView::~PageTranslateFootnoteView() = default;

void PageTranslateFootnoteView::OnPageTranslateStatusChanged() {
  Update();
}

void PageTranslateFootnoteView::OnPageTranslateStatusDestroyed() {
  observation_.Reset();
  status_ = nullptr;
}

void PageTranslateFootnoteView::Update() {
  // Without a status (no MoltBrowser driver), what is always true.
  const Footnote footnote =
      FootnoteFor(status_ ? status_->snapshot() : StatusSnapshot());
  label_->SetText(base::UTF8ToUTF16(footnote.text));
  link_kind_ = footnote.link;
  link_->SetText(base::UTF8ToUTF16(footnote.link_text));
  link_->SetVisible(footnote.link != FootnoteLink::kNone);
}

void PageTranslateFootnoteView::OnLinkClicked() {
  switch (link_kind_) {
    case FootnoteLink::kNone:
      return;
    case FootnoteLink::kStop:
      if (status_) {
        status_->Stop();
      }
      return;
    case FootnoteLink::kChooseModel:
      // MoltBrowser's model picker, in a new tab of this window; the user
      // downloads a model from there, or doesn't.
      if (web_contents_) {
        web_contents_->OpenURL(
            content::OpenURLParams(GURL(chrome::kChromeUIMoltAIURL),
                                   content::Referrer(),
                                   WindowOpenDisposition::NEW_FOREGROUND_TAB,
                                   ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                   /*is_renderer_initiated=*/false),
            /*navigation_handle_callback=*/{});
      }
      return;
  }
}

std::unique_ptr<views::View> CreatePageTranslateFootnote(
    content::WebContents* web_contents) {
  if (!web_contents) {
    return nullptr;
  }
  return std::make_unique<PageTranslateFootnoteView>(web_contents);
}

BEGIN_METADATA(PageTranslateFootnoteView)
END_METADATA

}  // namespace molt_ai::page_translate
