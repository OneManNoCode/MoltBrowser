// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_caption_bar.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/webview/webview.h"

namespace molt_ai {
namespace voice {

namespace {

std::unique_ptr<views::Label> MakeLabel() {
  auto label = std::make_unique<views::Label>();
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetElideBehavior(gfx::ELIDE_TAIL);
  label->SetMultiLine(false);
  // The bar's own colours, as they are: they were chosen for contrast.
  label->SetAutoColorReadabilityEnabled(false);
  return label;
}

}  // namespace

VoiceCaptionBar::VoiceCaptionBar(BrowserWindowInterface* window,
                                 views::WebView* contents_view)
    : window_(window), contents_view_(contents_view) {
  // Its own layer, so it is drawn above a tab-modal dialog's scrim, which
  // covers the whole contents area.
  SetPaintToLayer();
  layer()->SetName("VoiceCaptionBar");
  SetCanProcessEventsWithinSubtree(false);
  SetFocusBehavior(FocusBehavior::NEVER);
  SetVisible(false);

  state_label_ = AddChildView(MakeLabel());
  heard_label_ = AddChildView(MakeLabel());
  status_label_ = AddChildView(MakeLabel());
  word_label_ = AddChildView(MakeLabel());
  word_label_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
  word_label_->SetElideBehavior(gfx::NO_ELIDE);
  word_label_->SetVisible(false);

  // A polite live region named "Voice mode": what was heard and what is
  // happening are read as they change. The state is its description (read
  // when the user reaches the bar), not announced at every change.
  views::ViewAccessibility& accessibility = GetViewAccessibility();
  accessibility.SetRole(ax::mojom::Role::kStatus);
  accessibility.SetName(kCaptionAccessibleName);
  accessibility.SetLiveRegionContainer(
      views::ViewAccessibility::LiveRegionStatus::kPolite);
  state_label_->GetViewAccessibility().SetIsIgnored(true);
  // The confirmation word is for the eyes only: never in the tree a screen
  // reader (or any program reading it) gets.
  word_label_->GetViewAccessibility().SetIsIgnored(true);

  if (window_) {
    active_tab_changed_ = window_->RegisterActiveTabDidChange(
        base::BindRepeating([](VoiceCaptionBar* bar,
                               BrowserWindowInterface*) { bar->UpdateUp(); },
                            base::Unretained(this)));
    window_closed_ = window_->RegisterBrowserDidClose(base::BindRepeating(
        &VoiceCaptionBar::OnWindowClosed, base::Unretained(this)));
  }
  attached_ = contents_view_->AddWebContentsAttachedCallback(
      base::BindRepeating([](VoiceCaptionBar* bar,
                             views::WebView*) { bar->UpdateUp(); },
                          base::Unretained(this)));
  contents_observation_.Observe(contents_view_);
  VoiceOverlay::AddCaptionSink(this);
}

VoiceCaptionBar::~VoiceCaptionBar() {
  VoiceOverlay::RemoveCaptionSink(this);
}

int VoiceCaptionBar::HeightForArea(int area_height) const {
  const CaptionMetrics metrics =
      FitCaption(style_.caption_metrics, style_.caption_min_font, area_height);
  return static_cast<int>(std::ceil(CaptionBarHeight(metrics)));
}

void VoiceCaptionBar::SetLowerCorners(const gfx::RoundedCornersF& corners) {
  if (layer()->rounded_corner_radii() == corners) {
    return;
  }
  layer()->SetRoundedCornerRadius(corners);
  layer()->SetIsFastRoundedCorner(true);
}

BrowserWindowInterface* VoiceCaptionBar::GetWindow() const {
  return window_;
}

void VoiceCaptionBar::OnCaptionChanged(
    const std::optional<CaptionText>& caption) {
  const bool was_set = caption_.has_value();
  caption_ = caption;
  if (caption_ && !was_set) {
    UpdateStyle();
  }
  UpdateText();
  UpdateUp();
}

bool VoiceCaptionBar::ShowsActiveTab() const {
  if (!window_ || !contents_view_ || !contents_view_->web_contents()) {
    return false;
  }
  tabs::TabInterface* active = window_->GetActiveTabInterface();
  return active && active->GetContents() == contents_view_->web_contents();
}

void VoiceCaptionBar::UpdateUp() {
  const bool up = caption_.has_value() && ShowsActiveTab();
  if (up == GetVisible()) {
    return;
  }
  SetVisible(up);
  // The page area gives the bar its height, or takes it back.
  if (parent()) {
    parent()->InvalidateLayout();
  }
}

void VoiceCaptionBar::UpdateText() {
  if (!caption_) {
    return;
  }
  state_label_->SetText(base::UTF8ToUTF16(caption_->state_label));
  state_label_->SetBackgroundColor(StateFill(style_.palette, caption_->state));
  heard_label_->SetText(base::UTF8ToUTF16(caption_->heard));
  status_label_->SetText(base::UTF8ToUTF16(caption_->status));
  word_label_->SetText(base::UTF8ToUTF16(caption_->word));
  word_label_->SetVisible(!caption_->word.empty());
  GetViewAccessibility().SetDescription(caption_->state_label);
  // The pill's width follows the state's word.
  InvalidateLayout();
  SchedulePaint();
}

void VoiceCaptionBar::UpdateStyle() {
  style_ = VoiceOverlayStyle::For(
      *this, contents_view_ ? contents_view_->web_contents() : nullptr);
  const OverlayPalette& palette = style_.palette;
  for (views::Label* label :
       {heard_label_.get(), status_label_.get(), word_label_.get()}) {
    label->SetEnabledColor(palette.caption_text);
    label->SetBackgroundColor(palette.caption_fill);
  }
  state_label_->SetEnabledColor(palette.state_text);
  label_font_ = 0;  // set again by Layout()
  layer()->SetFillsBoundsOpaquely(false);
  InvalidateLayout();
  SchedulePaint();
}

void VoiceCaptionBar::Layout(PassKey) {
  // The metrics that make this height (HeightForArea() gave it from a page
  // area at least three times as tall, or the text is at its floor).
  const CaptionMetrics metrics = FitCaption(
      style_.caption_metrics, style_.caption_min_font, 3.0f * height());
  if (metrics.font != label_font_) {
    label_font_ = metrics.font;
    const gfx::FontList font = VoiceOverlayStyle::CaptionFontAt(metrics.font);
    for (views::Label* label :
         {state_label_.get(), heard_label_.get(), status_label_.get()}) {
      label->SetFontList(font);
    }
    // The word: bold, half as large again, across both lines.
    word_label_->SetFontList(
        VoiceOverlayStyle::CaptionFontAt(metrics.font * 1.5f)
            .DeriveWithWeight(gfx::Font::Weight::BOLD));
  }
  const int padding = static_cast<int>(std::round(metrics.padding));
  const int line = static_cast<int>(std::ceil(metrics.line_height));
  const int pill_padding = std::max(4, padding / 2);
  const int x = padding;
  const int first = static_cast<int>(std::round(metrics.border)) + padding;
  const int second = first + line + padding / 2;
  int width = std::max(0, this->width() - 2 * padding);

  // The word's box at the end, across both lines; the text gives way.
  word_box_ = gfx::Rect();
  if (caption_ && !caption_->word.empty()) {
    const int word_width = std::min(
        word_label_->GetPreferredSize().width() + 2 * padding, width / 2);
    const int word_height = 2 * line + padding / 2;
    word_box_ =
        gfx::Rect(x + width - word_width, first, word_width, word_height);
    word_label_->SetBoundsRect(gfx::Rect(
        word_box_.x() + pill_padding, word_box_.y(),
        std::max(0, word_box_.width() - 2 * pill_padding), word_box_.height()));
    width = std::max(0, width - word_width - padding);
  }

  const int state_width =
      std::min(state_label_->GetPreferredSize().width(), width / 3);
  state_pill_ = gfx::Rect(x, first, state_width + 2 * pill_padding, line);
  state_label_->SetBounds(x + pill_padding, first, state_width, line);
  const int heard_x = state_pill_.right() + padding;
  heard_label_->SetBounds(heard_x, first,
                          std::max(0, x + width - heard_x), line);
  status_label_->SetBounds(x, second, width, line);
}

void VoiceCaptionBar::OnPaint(gfx::Canvas* canvas) {
  const OverlayPalette& palette = style_.palette;
  canvas->DrawColor(palette.caption_fill);
  // The line between the page and the bar.
  const float border = style_.caption_metrics.border;
  canvas->FillRect(
      gfx::Rect(0, 0, width(), static_cast<int>(std::round(border))),
      palette.caption_border);
  if (caption_ && !state_pill_.IsEmpty()) {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(StateFill(palette, caption_->state));
    canvas->DrawRoundRect(gfx::RectF(state_pill_), state_pill_.height() / 4.0f,
                          flags);
  }
  if (!word_box_.IsEmpty()) {
    // A frame in the border's colour around the word.
    cc::PaintFlags frame;
    frame.setAntiAlias(true);
    frame.setStyle(cc::PaintFlags::kStroke_Style);
    frame.setStrokeWidth(border);
    frame.setColor(palette.caption_border);
    gfx::RectF box(word_box_);
    box.Inset(border / 2);
    canvas->DrawRoundRect(box, word_box_.height() / 6.0f, frame);
  }
}

void VoiceCaptionBar::OnThemeChanged() {
  views::View::OnThemeChanged();
  if (caption_) {
    UpdateStyle();
  }
}

void VoiceCaptionBar::OnViewIsDeleting(views::View* observed_view) {
  contents_observation_.Reset();
  attached_ = base::CallbackListSubscription();
  contents_view_ = nullptr;
}

void VoiceCaptionBar::OnWindowClosed(BrowserWindowInterface* window) {
  active_tab_changed_ = base::CallbackListSubscription();
  window_closed_ = base::CallbackListSubscription();
  window_ = nullptr;
}

BEGIN_METADATA(VoiceCaptionBar)
END_METADATA

}  // namespace voice
}  // namespace molt_ai
