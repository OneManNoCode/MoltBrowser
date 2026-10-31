// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What voice mode shows on screen (docs/design/TOUCHLESS_BROWSING.md §5.3,
// §5.5, §6.6): a large number on everything the user can act on, a thick
// ring around the item the browser is reading or acting on, and a caption
// bar with the state, what was heard and what is happening. This is the
// API the voice controller calls; it needs no views code.
//
//   VoiceOverlay* overlay = VoiceOverlay::GetOrCreateForWebContents(tab);
//   overlay->ShowNumbers(map);              // the page map, page_map.h
//   overlay->SetCurrentItem(map, 4);        // the ring on item 4
//   overlay->SetCaption(VoiceState::kSpeaking, "play the fourth one",
//                       "Playing: Bohemian Rhapsody, Live Aid");
//   overlay->SetHiddenCallback(...);        // the numbers went stale
//
// Numbers and the ring belong to the tab; the caption bar belongs to the
// tab's window and stays as it is when the user switches tabs.
//
// Drawn by the browser, above and below the page, never inside it
// (voice_numbers_view.h, voice_caption_bar.h, one of each per contents
// area of a window, ContentsContainerView): pages can't read, style or
// fake any of it, it never takes keyboard focus from the page, and it
// changes nothing in the page's layout. The caption bar takes its height
// from the bottom of the page area, as other browser bars do; its height
// doesn't depend on its text, so the page is resized only when the bar is
// shown or hidden (keep it shown for the whole voice session).
//
// Numbers follow the page: while they (or the ring) are shown, the
// overlay listens to the tab's VoicePageController and maps the page again
// (at most ten times a second) when its trees change, scrolling included.
// As long as the numbers the user sees still name the same items (items
// may be added after the last), they move with them. When the numbering
// changes, when the page navigates (a jump within the page aside) or when
// the tab is hidden, the numbers and the ring are hidden and the hidden
// callback says why; the controller maps the page again and shows the new
// numbers (in the same task, so nothing flickers) or says what changed.
//
// UI thread only.

#ifndef CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_H_
#define CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_H_

#include <optional>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

class BrowserWindowInterface;

namespace content {
class NavigationHandle;
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

// Why the numbers and the ring went away by themselves.
enum class OverlayHidden {
  // The page navigated (any committed navigation of the main frame but a
  // jump to a fragment of the same document).
  kNavigated,
  // The tab was hidden: the user switched tabs or minimized the window.
  kTabHidden,
  // The numbers moved (an item before the last came or went), or the
  // page's renderer went away.
  kPageChanged,
};

class VoiceOverlay : public content::WebContentsObserver,
                     public content::WebContentsUserData<VoiceOverlay> {
 public:
  using HiddenCallback = base::RepeatingCallback<void(OverlayHidden)>;

  // How often the overlay maps the page again while it follows it.
  static constexpr base::TimeDelta kFollowInterval = base::Milliseconds(100);

  VoiceOverlay(const VoiceOverlay&) = delete;
  VoiceOverlay& operator=(const VoiceOverlay&) = delete;
  ~VoiceOverlay() override;

  // --- Numbers and the focus ring (this tab) ---------------------------------

  // Shows a badge on each item of |map| that is on screen, and makes |map|
  // the one SetCurrentItem(number) uses. Returns false, and shows nothing,
  // if the tab isn't shown in a browser window now.
  bool ShowNumbers(const PageMap& map);
  void HideNumbers();
  bool numbers_shown() const { return numbers_shown_; }

  // Draws the ring around item |number| of |map| (and, when numbers are
  // shown, draws them from |map| too). Returns false, and draws no ring,
  // if the tab isn't shown or |map| has no such item.
  bool SetCurrentItem(const PageMap& map, int number);
  // The same on the last map given.
  bool SetCurrentItem(int number);
  void ClearCurrentItem();
  int current_item() const { return current_item_; }

  // Runs when the numbers or the ring hide by themselves (not after
  // HideNumbers() or ClearCurrentItem()), synchronously.
  void SetHiddenCallback(HiddenCallback callback);

  // --- The caption bar (this tab's window) -----------------------------------

  // Shows the caption bar of the tab's window with |state|, what was heard
  // and what is happening (both cleaned for display and capped,
  // MakeCaptionText()). Returns false if the tab isn't in a window.
  bool SetCaption(VoiceState state,
                  std::string_view heard,
                  std::string_view status);
  // Hides the caption bar (voice mode is over).
  bool HideCaption();

  // The same for a window, for a controller that has no tab at hand;
  // |word| is the voice gesture's word to show, or empty
  // (CaptionText::word).
  static void SetCaptionForWindow(BrowserWindowInterface* window,
                                  VoiceState state,
                                  std::string_view heard,
                                  std::string_view status,
                                  std::string_view word = {});
  static void HideCaptionForWindow(BrowserWindowInterface* window);

  // --- For the views ----------------------------------------------------------

  // The numbers of the map shown (or given with the current item), with
  // their bounds as the page last reported them; empty when neither the
  // numbers nor the ring is shown.
  const std::vector<OverlayItem>& items() const { return items_; }
  // The page's pinch zoom (1 when not zoomed).
  float page_scale_factor() const { return page_scale_factor_; }

  // Each contents area of a browser window draws the numbers and ring of
  // the tab it shows.
  class NumbersSink : public base::CheckedObserver {
   public:
    // The tab shown in this sink's contents area, or null.
    virtual content::WebContents* GetShownContents() const = 0;
    // |overlay| (the shown tab's) changed, or null: there is nothing to
    // draw.
    virtual void OnOverlayChanged(const VoiceOverlay* overlay) = 0;
  };

  // Each contents area of a browser window has a caption bar; the bars of a
  // window show the same caption (the one in the active tab's area is
  // visible).
  class CaptionSink : public base::CheckedObserver {
   public:
    virtual BrowserWindowInterface* GetWindow() const = 0;
    // The window's caption; nullopt hides the bar.
    virtual void OnCaptionChanged(const std::optional<CaptionText>& caption) = 0;
  };

  static void AddNumbersSink(NumbersSink* sink);
  static void RemoveNumbersSink(NumbersSink* sink);
  static void AddCaptionSink(CaptionSink* sink);
  static void RemoveCaptionSink(CaptionSink* sink);

  // The sinks that show |contents| or belong to |window|, for tests.
  static NumbersSink* FindNumbersSinkForTesting(content::WebContents* contents);
  static std::vector<CaptionSink*> FindCaptionSinksForTesting(
      BrowserWindowInterface* window);

  // Maps the page again now, as a timer tick of the follower would.
  void FollowPageNowForTesting();

 private:
  friend class content::WebContentsUserData<VoiceOverlay>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();

  explicit VoiceOverlay(content::WebContents* web_contents);

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void OnVisibilityChanged(content::Visibility visibility) override;
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;
  void OnPageScaleFactorChanged(float page_scale_factor) override;
  void WebContentsDestroyed() override;

  bool IsShown() const;
  bool Showing() const { return numbers_shown_ || current_item_ > 0; }
  // Takes |map|'s items as the overlay's.
  void TakeMap(const PageMap& map);
  // Hides the numbers and the ring, and tells the controller why.
  void HideStale(OverlayHidden reason);
  void Clear();
  // Follows the page (or stops following) as Showing() asks.
  void UpdateFollowing();
  void OnTreesUpdated();
  // Maps the page again and moves the numbers, or hides them if they moved.
  void RefreshFromPage();
  void NotifySinks();

  bool numbers_shown_ = false;
  int current_item_ = 0;
  std::vector<OverlayItem> items_;
  float page_scale_factor_ = 1;
  HiddenCallback hidden_callback_;
  base::CallbackListSubscription trees_updated_;
  base::OneShotTimer follow_timer_;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_VIEWS_MOLT_AI_VOICE_VOICE_OVERLAY_H_
