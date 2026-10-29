// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_views.h"

#include <algorithm>

#include "chrome/browser/ui/views/molt_ai/voice/voice_caption_bar.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_numbers_view.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace molt_ai {
namespace voice {

namespace {

// The page area keeps at least this much of its height for the page.
constexpr int kMinPageHeight = 100;

const VoiceCaptionBar* AsBar(const views::View* view) {
  return view ? views::AsViewClass<VoiceCaptionBar>(view) : nullptr;
}

}  // namespace

std::unique_ptr<views::View> CreateVoiceNumbersView(
    views::WebView* contents_view) {
  return std::make_unique<VoiceNumbersView>(contents_view);
}

std::unique_ptr<views::View> CreateVoiceCaptionBar(
    BrowserWindowInterface* window,
    views::WebView* contents_view) {
  return std::make_unique<VoiceCaptionBar>(window, contents_view);
}

void LayOutVoiceCaptionBar(const views::View& host,
                           views::View* caption_bar,
                           gfx::Rect& page_area,
                           views::ProposedLayout& layouts) {
  const VoiceCaptionBar* bar = AsBar(caption_bar);
  if (!bar) {
    return;
  }
  gfx::Rect rect;
  if (bar->GetVisible() && page_area.height() > kMinPageHeight) {
    const int height = std::min(bar->HeightForArea(page_area.height()),
                                page_area.height() - kMinPageHeight);
    page_area.set_height(page_area.height() - height);
    rect = gfx::Rect(page_area.x(), page_area.bottom(), page_area.width(),
                     height);
  }
  // Up but with no room (a very short window): kept up, with no size.
  layouts.child_layouts.push_back(
      {caption_bar, caption_bar->GetVisible(), host.GetMirroredRect(rect)});
}

void LayOutVoiceNumbers(views::View* numbers_view,
                        const gfx::Rect& page_rect,
                        views::ProposedLayout& layouts) {
  if (numbers_view) {
    layouts.child_layouts.push_back(
        {numbers_view, numbers_view->GetVisible(), page_rect});
  }
}

int VoiceCaptionBarHeight(const views::View* caption_bar) {
  return IsVoiceCaptionBarUp(caption_bar) ? caption_bar->height() : 0;
}

bool IsVoiceCaptionBarUp(const views::View* caption_bar) {
  return caption_bar && caption_bar->GetVisible();
}

void SetVoiceCaptionBarCorners(views::View* caption_bar,
                               const gfx::RoundedCornersF& corners) {
  if (VoiceCaptionBar* bar =
          caption_bar ? views::AsViewClass<VoiceCaptionBar>(caption_bar)
                      : nullptr) {
    bar->SetLowerCorners(corners);
  }
}

}  // namespace voice
}  // namespace molt_ai
