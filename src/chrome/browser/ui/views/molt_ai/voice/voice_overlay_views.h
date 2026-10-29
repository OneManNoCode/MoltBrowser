// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice overlay's views in a contents area: what the hooks in
// ContentsContainerView call (chrome/browser/ui/views/frame/
// contents_container_view.{h,cc}, each marked "MoltBrowser:"):
//
//   constructor             CreateVoiceNumbersView() above the page (over
//                           the watermark, under everything else drawn
//                           over a page), CreateVoiceCaptionBar() after
//                           the scrim (readable over tab-modal dialogs)
//   CalculateProposedLayout LayOutVoiceCaptionBar() takes the bar's height
//                           off the page area first, and lays the bar out
//                           ahead of the page; LayOutVoiceNumbers() lays
//                           the numbers over the page
//   GetContentsViewBounds   VoiceCaptionBarHeight() counts the bar as page
//                           area, so DevTools' docked side is still found
//   SetBorderRoundedCornersFrom, ChildVisibilityChanged
//                           IsVoiceCaptionBarUp(), SetVoiceCaptionBarCorners()
//                           (the bar takes the page area's lower corners)
//
// The views are held as views::View, so the upstream header needs nothing
// of MoltBrowser's.

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_VIEWS_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_VIEWS_H_

#include <memory>

#include "ui/gfx/geometry/rect.h"

class BrowserWindowInterface;

namespace gfx {
class RoundedCornersF;
}  // namespace gfx

namespace views {
struct ProposedLayout;
class View;
class WebView;
}  // namespace views

namespace molt_ai {
namespace voice {

// The numbers and ring over |contents_view|'s page (voice_numbers_view.h).
std::unique_ptr<views::View> CreateVoiceNumbersView(
    views::WebView* contents_view);

// The caption bar of |window| under |contents_view|'s page
// (voice_caption_bar.h).
std::unique_ptr<views::View> CreateVoiceCaptionBar(
    BrowserWindowInterface* window,
    views::WebView* contents_view);

// While |caption_bar| is up, takes its height off the bottom of
// |page_area| (leaving the page at least a little) and lays it out there;
// otherwise lays it out hidden. Its layout goes ahead of the page's, so its
// bounds are set when the page's change (GetContentsViewBounds() reads
// them). |host| (the contents area) mirrors the rect in right-to-left
// layouts. |caption_bar| may be null.
void LayOutVoiceCaptionBar(const views::View& host,
                           views::View* caption_bar,
                           gfx::Rect& page_area,
                           views::ProposedLayout& layouts);

// Lays the numbers view out over |page_rect| (the contents view's, already
// mirrored). |numbers_view| may be null.
void LayOutVoiceNumbers(views::View* numbers_view,
                        const gfx::Rect& page_rect,
                        views::ProposedLayout& layouts);

// The caption bar's height while it is up, else 0.
int VoiceCaptionBarHeight(const views::View* caption_bar);
bool IsVoiceCaptionBarUp(const views::View* caption_bar);

// Gives the caption bar the page area's lower corners.
void SetVoiceCaptionBarCorners(views::View* caption_bar,
                               const gfx::RoundedCornersF& corners);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_VIEWS_H_
