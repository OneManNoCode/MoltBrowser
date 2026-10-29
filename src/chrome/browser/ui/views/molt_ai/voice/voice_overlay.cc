// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {

namespace {

base::ObserverList<VoiceOverlay::NumbersSink>& NumbersSinks() {
  static base::NoDestructor<base::ObserverList<VoiceOverlay::NumbersSink>>
      sinks;
  return *sinks;
}

base::ObserverList<VoiceOverlay::CaptionSink>& CaptionSinks() {
  static base::NoDestructor<base::ObserverList<VoiceOverlay::CaptionSink>>
      sinks;
  return *sinks;
}

// The window |contents| is a tab of, or null.
BrowserWindowInterface* WindowOf(content::WebContents* contents) {
  tabs::TabInterface* tab =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  return tab ? tab->GetBrowserWindowInterface() : nullptr;
}

void SetWindowCaption(BrowserWindowInterface* window,
                      const std::optional<CaptionText>& caption) {
  if (!window) {
    return;
  }
  for (VoiceOverlay::CaptionSink& sink : CaptionSinks()) {
    if (sink.GetWindow() == window) {
      sink.OnCaptionChanged(caption);
    }
  }
}

}  // namespace

WEB_CONTENTS_USER_DATA_KEY_IMPL(VoiceOverlay);

VoiceOverlay::VoiceOverlay(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<VoiceOverlay>(*web_contents) {}

VoiceOverlay::~VoiceOverlay() = default;

// --- Numbers and the ring -----------------------------------------------------

bool VoiceOverlay::ShowNumbers(const PageMap& map) {
  if (!IsShown()) {
    return false;
  }
  TakeMap(map);
  numbers_shown_ = true;
  UpdateFollowing();
  NotifySinks();
  return true;
}

void VoiceOverlay::HideNumbers() {
  if (!numbers_shown_) {
    return;
  }
  numbers_shown_ = false;
  if (!Showing()) {
    Clear();
  }
  UpdateFollowing();
  NotifySinks();
}

bool VoiceOverlay::SetCurrentItem(const PageMap& map, int number) {
  if (!IsShown() || !map.ItemByNumber(number)) {
    ClearCurrentItem();
    return false;
  }
  TakeMap(map);
  current_item_ = number;
  UpdateFollowing();
  NotifySinks();
  return true;
}

bool VoiceOverlay::SetCurrentItem(int number) {
  const bool known = number > 0 &&
                     static_cast<size_t>(number) <= items_.size() &&
                     items_[number - 1].number == number;
  if (!IsShown() || !known) {
    ClearCurrentItem();
    return false;
  }
  current_item_ = number;
  UpdateFollowing();
  NotifySinks();
  return true;
}

void VoiceOverlay::ClearCurrentItem() {
  if (current_item_ == 0) {
    return;
  }
  current_item_ = 0;
  if (!Showing()) {
    Clear();
  }
  UpdateFollowing();
  NotifySinks();
}

void VoiceOverlay::SetHiddenCallback(HiddenCallback callback) {
  hidden_callback_ = std::move(callback);
}

// --- The caption bar ------------------------------------------------------------

bool VoiceOverlay::SetCaption(VoiceState state,
                              std::string_view heard,
                              std::string_view status) {
  BrowserWindowInterface* window = WindowOf(web_contents());
  if (!window) {
    return false;
  }
  SetCaptionForWindow(window, state, heard, status);
  return true;
}

bool VoiceOverlay::HideCaption() {
  BrowserWindowInterface* window = WindowOf(web_contents());
  if (!window) {
    return false;
  }
  HideCaptionForWindow(window);
  return true;
}

// static
void VoiceOverlay::SetCaptionForWindow(BrowserWindowInterface* window,
                                       VoiceState state,
                                       std::string_view heard,
                                       std::string_view status) {
  SetWindowCaption(window, MakeCaptionText(state, heard, status));
}

// static
void VoiceOverlay::HideCaptionForWindow(BrowserWindowInterface* window) {
  SetWindowCaption(window, std::nullopt);
}

// --- Sinks ------------------------------------------------------------------------

// static
void VoiceOverlay::AddNumbersSink(NumbersSink* sink) {
  NumbersSinks().AddObserver(sink);
}

// static
void VoiceOverlay::RemoveNumbersSink(NumbersSink* sink) {
  NumbersSinks().RemoveObserver(sink);
}

// static
void VoiceOverlay::AddCaptionSink(CaptionSink* sink) {
  CaptionSinks().AddObserver(sink);
}

// static
void VoiceOverlay::RemoveCaptionSink(CaptionSink* sink) {
  CaptionSinks().RemoveObserver(sink);
}

// static
VoiceOverlay::NumbersSink* VoiceOverlay::FindNumbersSinkForTesting(
    content::WebContents* contents) {
  for (NumbersSink& sink : NumbersSinks()) {
    if (sink.GetShownContents() == contents) {
      return &sink;
    }
  }
  return nullptr;
}

// static
std::vector<VoiceOverlay::CaptionSink*>
VoiceOverlay::FindCaptionSinksForTesting(BrowserWindowInterface* window) {
  std::vector<CaptionSink*> found;
  for (CaptionSink& sink : CaptionSinks()) {
    if (sink.GetWindow() == window) {
      found.push_back(&sink);
    }
  }
  return found;
}

void VoiceOverlay::FollowPageNowForTesting() {
  follow_timer_.Stop();
  RefreshFromPage();
}

// --- Following the page -----------------------------------------------------------

bool VoiceOverlay::IsShown() const {
  for (NumbersSink& sink : NumbersSinks()) {
    if (sink.GetShownContents() == web_contents()) {
      return true;
    }
  }
  return false;
}

void VoiceOverlay::TakeMap(const PageMap& map) {
  items_ = OverlayItemsFromMap(map);
  if (current_item_ > 0 && !map.ItemByNumber(current_item_)) {
    current_item_ = 0;
  }
}

void VoiceOverlay::HideStale(OverlayHidden reason) {
  if (!Showing()) {
    return;
  }
  Clear();
  NotifySinks();
  if (hidden_callback_) {
    hidden_callback_.Run(reason);
  }
}

void VoiceOverlay::Clear() {
  numbers_shown_ = false;
  current_item_ = 0;
  items_.clear();
  UpdateFollowing();
}

void VoiceOverlay::UpdateFollowing() {
  if (!Showing()) {
    trees_updated_ = base::CallbackListSubscription();
    follow_timer_.Stop();
    return;
  }
  if (trees_updated_) {
    return;
  }
  VoicePageController* controller =
      web_contents() ? VoicePageController::FromWebContents(web_contents())
                     : nullptr;
  if (!controller) {
    return;  // nothing to follow: the numbers stay where they were put
  }
  trees_updated_ = controller->AddTreesUpdatedCallback(base::BindRepeating(
      &VoiceOverlay::OnTreesUpdated, base::Unretained(this)));
}

void VoiceOverlay::OnTreesUpdated() {
  if (Showing() && !follow_timer_.IsRunning()) {
    follow_timer_.Start(FROM_HERE, kFollowInterval, this,
                        &VoiceOverlay::RefreshFromPage);
  }
}

void VoiceOverlay::RefreshFromPage() {
  if (!Showing() || !web_contents()) {
    return;
  }
  VoicePageController* controller =
      VoicePageController::FromWebContents(web_contents());
  if (!controller || !controller->active()) {
    return;
  }
  const std::vector<OverlayItem> fresh =
      OverlayItemsFromMap(controller->BuildMap());
  switch (molt_ai::voice::FollowPage(items_, fresh)) {
    case FollowResult::kStale:
      HideStale(OverlayHidden::kPageChanged);
      return;
    case FollowResult::kSame:
      return;
    case FollowResult::kMoved:
      NotifySinks();
      return;
  }
}

void VoiceOverlay::NotifySinks() {
  for (NumbersSink& sink : NumbersSinks()) {
    if (sink.GetShownContents() == web_contents()) {
      sink.OnOverlayChanged(Showing() ? this : nullptr);
    }
  }
}

// --- content::WebContentsObserver ---------------------------------------------------

void VoiceOverlay::DidFinishNavigation(content::NavigationHandle* navigation) {
  if (!Showing() || !navigation->IsInPrimaryMainFrame() ||
      !navigation->HasCommitted()) {
    return;
  }
  // A jump to a fragment keeps the document, and its numbers: they follow
  // the scrolling it does.
  if (navigation->IsSameDocument() &&
      navigation->GetURL().EqualsIgnoringRef(
          navigation->GetPreviousPrimaryMainFrameURL())) {
    return;
  }
  HideStale(OverlayHidden::kNavigated);
}

void VoiceOverlay::OnVisibilityChanged(content::Visibility visibility) {
  if (visibility == content::Visibility::HIDDEN) {
    HideStale(OverlayHidden::kTabHidden);
  }
}

void VoiceOverlay::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  HideStale(OverlayHidden::kPageChanged);
}

void VoiceOverlay::OnPageScaleFactorChanged(float page_scale_factor) {
  page_scale_factor_ = page_scale_factor;
  if (Showing()) {
    NotifySinks();
  }
}

void VoiceOverlay::WebContentsDestroyed() {
  Clear();
}

}  // namespace voice
}  // namespace molt_ai
