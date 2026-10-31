// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's caption bar (docs/design/TOUCHLESS_BROWSING.md §5.5): a
// large-type bar under the page with the state ("Listening"), what was
// heard ("Heard: play the live one by Queen") and what is happening
// ("Playing: Bohemian Rhapsody, Live Aid"). One per contents area of a
// browser window (ContentsContainerView); a window's bars show its caption
// (VoiceOverlay::SetCaption()), and the one in the area that shows the
// window's active tab is up.
//
// While it is up it takes its height from the bottom of the page area, as
// other browser bars do, so it covers nothing of the page; its height
// depends on the user's text size and the window's height, never on its
// text. It stays readable over a tab-modal dialog's scrim.
//
// For assistive technology it is a polite live region (role status, named
// "Voice mode", the state as its description): screen readers read what
// was heard and what is happening as they change. The voice gesture's
// confirmation word, while a prompt waits for it and it may be shown, is
// drawn large at the bar's end, across both lines, and left out of the
// accessibility tree: a screen reader would say it with a voice the speech
// helper hears in full (TOUCHLESS_BROWSING.md §7.3). It never takes focus,
// so the OS magnifier stays where the user is. Its sizes follow the user's
// text settings and its colours forced colours (voice_overlay_style.h).

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_CAPTION_BAR_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_CAPTION_BAR_H_

#include <optional>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_style.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

class BrowserWindowInterface;

namespace gfx {
class Canvas;
}  // namespace gfx

namespace views {
class Label;
class WebView;
}  // namespace views

namespace molt_ai {
namespace voice {

class VoiceCaptionBar : public views::View,
                        public views::ViewObserver,
                        public VoiceOverlay::CaptionSink {
  METADATA_HEADER(VoiceCaptionBar, views::View)

 public:
  // |window| is the browser window; |contents_view| shows the page of this
  // contents area.
  VoiceCaptionBar(BrowserWindowInterface* window,
                  views::WebView* contents_view);
  VoiceCaptionBar(const VoiceCaptionBar&) = delete;
  VoiceCaptionBar& operator=(const VoiceCaptionBar&) = delete;
  ~VoiceCaptionBar() override;

  // The bar's height in a page area |area_height| DIPs tall (a third of it
  // at most, while the text can shrink).
  int HeightForArea(int area_height) const;
  // The page area's lower corners, which the bar takes while it is up.
  void SetLowerCorners(const gfx::RoundedCornersF& corners);

  const std::optional<CaptionText>& caption() const { return caption_; }
  views::WebView* contents_view() const { return contents_view_; }
  const views::Label* heard_label() const { return heard_label_; }
  const views::Label* status_label() const { return status_label_; }
  const views::Label* state_label() const { return state_label_; }
  const views::Label* word_label() const { return word_label_; }

  // VoiceOverlay::CaptionSink:
  BrowserWindowInterface* GetWindow() const override;
  void OnCaptionChanged(const std::optional<CaptionText>& caption) override;

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  void OnThemeChanged() override;

  // views::ViewObserver:
  void OnViewIsDeleting(views::View* observed_view) override;

 private:
  // Whether this area shows the window's active tab.
  bool ShowsActiveTab() const;
  // Shows or hides the bar, and lays the contents area out again if that
  // changed.
  void UpdateUp();
  void UpdateText();
  void UpdateStyle();
  void OnWindowClosed(BrowserWindowInterface* window);

  raw_ptr<BrowserWindowInterface> window_;
  raw_ptr<views::WebView> contents_view_;
  raw_ptr<views::Label> state_label_ = nullptr;
  raw_ptr<views::Label> heard_label_ = nullptr;
  raw_ptr<views::Label> status_label_ = nullptr;
  raw_ptr<views::Label> word_label_ = nullptr;
  std::optional<CaptionText> caption_;
  VoiceOverlayStyle style_;
  // The font size the labels have now, and where the state's pill is.
  float label_font_ = 0;
  gfx::Rect state_pill_;
  // Where the word's box is (empty without a word).
  gfx::Rect word_box_;
  base::CallbackListSubscription active_tab_changed_;
  base::CallbackListSubscription window_closed_;
  base::CallbackListSubscription attached_;
  base::ScopedObservation<views::View, views::ViewObserver>
      contents_observation_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_CAPTION_BAR_H_
