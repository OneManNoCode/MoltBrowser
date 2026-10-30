// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_controller.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/molt_ai/agents/voice_agent_planner.h"
#include "chrome/browser/molt_ai/security/activation_guard.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/site_skills.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/molt_ai/voice/voice_reader.h"
#include "chrome/browser/molt_ai/voice/voice_reading_settings.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "chrome/browser/molt_ai/voice/voice_speech.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_user_gesture_details.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay.h"
#include "chrome/browser/ui/views/molt_ai/voice/voice_overlay_layout.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_service.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "ui/accessibility/ax_updates_and_events.h"
#include "ui/base/base_window.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {

namespace {

using ActionResult = VoicePageController::ActionResult;

// Tab titles the browser reads, in code points, and tabs listed.
constexpr size_t kMaxTabTitleChars = 160;
constexpr size_t kMaxTabsListed = 200;
// A notice the listening side says, in code points.
constexpr size_t kMaxNoticeChars = 400;
// What the planner says, in code points.
constexpr size_t kMaxPlannerSpeechChars = 400;
// What "repeat" keeps of what the planner said.
constexpr size_t kMaxPlannerSpeechRemembered = 8;
// Said when the planner's question went unanswered.
constexpr char kPlannerNoAnswer[] = "I didn't hear yes, so I stopped.";

// After a navigation hid the numbers: how often, and how many times, to
// look for the new page's tree to number.
constexpr base::TimeDelta kNumbersRefreshDelay = base::Milliseconds(700);
constexpr int kMaxNumbersRefreshAttempts = 10;

std::optional<StepFailure> FailureFor(ActionResult result) {
  switch (result) {
    case ActionResult::kDone:
      return std::nullopt;
    case ActionResult::kNotActive:
      return StepFailure::kNotActive;
    case ActionResult::kStale:
      return StepFailure::kStale;
    case ActionResult::kNotAllowed:
      return StepFailure::kNotAllowed;
    case ActionResult::kDisabled:
      return StepFailure::kDisabled;
    case ActionResult::kNotEditable:
      return StepFailure::kNotEditable;
    case ActionResult::kCredentialField:
      return StepFailure::kCredentialField;
    case ActionResult::kTooLong:
      return StepFailure::kTooLong;
    case ActionResult::kNotMedia:
      return StepFailure::kNotMedia;
    case ActionResult::kNoMediaSession:
      return StepFailure::kNoMediaSession;
    case ActionResult::kFocusNotConfirmed:
      return StepFailure::kFocusNotConfirmed;
    case ActionResult::kCancelled:
      return StepFailure::kCancelled;
    case ActionResult::kNoSuchOption:
      return StepFailure::kUnavailable;
  }
  return StepFailure::kUnavailable;
}

std::optional<int> CommandId(BrowserCommand command) {
  switch (command) {
    case BrowserCommand::kBack:
      return IDC_BACK;
    case BrowserCommand::kForward:
      return IDC_FORWARD;
    case BrowserCommand::kReload:
      return IDC_RELOAD;
    case BrowserCommand::kStopLoading:
      return IDC_STOP;
    case BrowserCommand::kHome:
      return IDC_HOME;
    case BrowserCommand::kZoomIn:
      return IDC_ZOOM_PLUS;
    case BrowserCommand::kZoomOut:
      return IDC_ZOOM_MINUS;
    case BrowserCommand::kZoomReset:
      return IDC_ZOOM_NORMAL;
    case BrowserCommand::kToggleFullScreen:
      return IDC_FULLSCREEN;
  }
  return std::nullopt;
}

VoicePageController::PageScrollAction ScrollActionFor(PageScroll scroll) {
  switch (scroll) {
    case PageScroll::kDown:
      return VoicePageController::PageScrollAction::kDown;
    case PageScroll::kUp:
      return VoicePageController::PageScrollAction::kUp;
    case PageScroll::kLeft:
      return VoicePageController::PageScrollAction::kLeft;
    case PageScroll::kRight:
      return VoicePageController::PageScrollAction::kRight;
    case PageScroll::kTop:
      return VoicePageController::PageScrollAction::kTop;
    case PageScroll::kBottom:
      return VoicePageController::PageScrollAction::kBottom;
  }
  return VoicePageController::PageScrollAction::kDown;
}

}  // namespace

// What the active tab does, for the controller: navigations, loading,
// tree updates (the page settling), media starting, and its end.
class VoiceController::TabWatcher : public content::WebContentsObserver {
 public:
  TabWatcher(VoiceController* controller, content::WebContents* contents)
      : content::WebContentsObserver(contents), controller_(controller) {}
  TabWatcher(const TabWatcher&) = delete;
  TabWatcher& operator=(const TabWatcher&) = delete;
  ~TabWatcher() override = default;

  // content::WebContentsObserver:
  void DidStartNavigation(content::NavigationHandle* navigation) override {
    if (navigation->IsInPrimaryMainFrame()) {
      controller_->OnNavigationStarted();
    }
  }
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (!navigation->IsInPrimaryMainFrame()) {
      return;
    }
    if (navigation->HasCommitted()) {
      controller_->OnNavigationCommitted();
    } else {
      controller_->OnNavigationEnded();
    }
  }
  void DidStopLoading() override { controller_->OnLoadingStopped(); }
  void AccessibilityEventReceived(
      const ui::AXUpdatesAndEvents& details) override {
    controller_->OnTreeChanged();
  }
  void MediaStartedPlaying(const MediaPlayerInfo& video_type,
                           const content::MediaPlayerId& id) override {
    controller_->OnMediaStarted();
  }
  void WebContentsDestroyed() override { controller_->OnTabGone(); }

 private:
  raw_ptr<VoiceController> controller_;
};

// The reader's side in the browser (voice_reader.h): the voice, the
// caption, the active tab's page.
class VoiceController::ReaderHost : public VoiceReader::Host {
 public:
  explicit ReaderHost(VoiceController* controller) : controller_(controller) {}
  ReaderHost(const ReaderHost&) = delete;
  ReaderHost& operator=(const ReaderHost&) = delete;
  ~ReaderHost() override = default;

  // VoiceReader::Host:
  uint64_t Speak(const std::string& text, double rate) override {
    return controller_->speech_
               ? controller_->speech_->Speak(text, rate, /*interrupt=*/false)
               : 0;
  }
  void Said(const std::string& text) override {
    for (Observer& observer : controller_->observers_) {
      observer.OnVoiceSpeech(text);
    }
  }
  void Status(const std::string& status) override {
    controller_->SetStatus(status);
  }
  void ScrollTo(const PageNodeHandle& node) override {
    if (VoicePageController* page = controller_->ActivePage()) {
      // A node gone since is left alone: reading goes on.
      page->ScrollIntoView(node);
    }
  }
  PageTree Tree() override {
    VoicePageController* page = controller_->ActivePage();
    content::WebContents* contents = controller_->ActiveContents();
    // Only http(s) pages are read (as every action is only taken there).
    if (!page || !page->active() || !contents ||
        !agent_safety::IsAllowedNavigationURL(
            contents->GetLastCommittedURL())) {
      return PageTree();
    }
    return page->BuildTree(VoicePageController::kMaxReadingStringBytes);
  }
  const SiteSkill* Skill() override {
    content::WebContents* contents = controller_->ActiveContents();
    if (!contents) {
      return nullptr;
    }
    const GURL& url = contents->GetLastCommittedURL();
    return url.SchemeIsHTTPOrHTTPS() ? BuiltInSkillForHost(url.host())
                                     : nullptr;
  }

 private:
  raw_ptr<VoiceController> controller_;
};

struct VoiceController::PageWait {
  VoiceStep step;
  base::TimeTicks started;
  base::TimeTicks committed;
  bool navigation_started = false;
  bool navigated = false;
  // The page's tree was asked for again (a page back from the back-forward
  // cache may not send it by itself).
  bool tree_asked = false;
  // Waiting for the site's own autoplay (kPlayOpened).
  bool autoplay = false;
};

DEFINE_USER_DATA(VoiceController);

// static
VoiceController* VoiceController::FromBrowser(BrowserWindowInterface* browser) {
  return browser ? Get(browser->GetUnownedUserDataHost()) : nullptr;
}

// static
VoiceController* VoiceController::GetOrCreateForBrowser(
    BrowserWindowInterface* browser) {
  if (!browser || browser->IsDeleteScheduled() ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
    return nullptr;
  }
  if (VoiceController* existing = FromBrowser(browser)) {
    return existing;
  }
  // Owned by itself: deleted when the window closes (OnBrowserDidClose()).
  return new VoiceController(browser);
}

VoiceController::VoiceController(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this) {
  did_close_subscription_ =
      browser->RegisterBrowserDidClose(base::BindRepeating(
          &VoiceController::OnBrowserDidClose, base::Unretained(this)));
  active_tab_subscription_ =
      browser->RegisterActiveTabDidChange(base::BindRepeating(
          &VoiceController::OnActiveTabChanged, base::Unretained(this)));
  speech_ = std::make_unique<VoiceSpeech>(browser->GetProfile());
  speech_->SetSpeakingChangedCallback(base::BindRepeating(
      &VoiceController::OnSpeakingChanged, weak_factory_.GetWeakPtr()));
  reader_host_ = std::make_unique<ReaderHost>(this);
  reader_ = std::make_unique<VoiceReader>(reader_host_.get());
  speech_->SetUtteranceDoneCallback(base::BindRepeating(
      [](base::WeakPtr<VoiceController> self, uint64_t id, bool spoken) {
        if (self) {
          self->reader_->OnUtteranceDone(id, spoken);
        }
      },
      weak_factory_.GetWeakPtr()));
  // The speech rate the user set (a setting).
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadVoiceSpeechRate),
      base::BindOnce(
          [](base::WeakPtr<VoiceController> self, double rate) {
            if (self) {
              self->state_.speech_rate = rate;
            }
          },
          weak_factory_.GetWeakPtr()));
  // The web agent's on-device planner, for what the grammar can't do.
  planner_ = CreateVoiceAgentPlanner();
}

VoiceController::~VoiceController() {
  for (Observer& observer : observers_) {
    observer.OnVoiceControllerDestroying(this);
  }
  CancelRun();
  DetachFromTab();
}

void VoiceController::OnBrowserDidClose(BrowserWindowInterface* browser) {
  delete this;
}

bool VoiceController::IsMoltNet() {
  return browser_ && tor::IsMoltNetProfile(browser_->GetProfile());
}

content::WebContents* VoiceController::ActiveContents() {
  TabStripModel* tabs = browser_ ? browser_->GetTabStripModel() : nullptr;
  return tabs ? tabs->GetActiveWebContents() : nullptr;
}

VoicePageController* VoiceController::ActivePage() {
  content::WebContents* contents = ActiveContents();
  if (!active_ || !contents) {
    return nullptr;
  }
  return VoicePageController::FromWebContents(contents);
}

bool VoiceController::Start() {
  if (active_) {
    return true;
  }
  if (!browser_ || IsMoltNet() || !VoiceModeEnabled()) {
    return false;
  }
  active_ = true;
  // Voice mode's own voice runs while voice mode is on.
  if (speech_) {
    speech_->SetActive(true);
  }
  AttachToActiveTab();
  NotifyCaption();
  return true;
}

void VoiceController::Stop() {
  CancelRun();
  reader_->Clear();
  stop_when_quiet_ = false;
  refresh_numbers_timer_.Stop();
  DetachFromTab();
  active_ = false;
  if (speech_) {
    speech_->SetActive(false);
  }
  // The user's preferences stay; the conversation doesn't.
  ConversationState fresh;
  fresh.speech_rate = state_.speech_rate;
  fresh.verbosity = state_.verbosity;
  const bool had_numbers = state_.numbers_shown;
  state_ = std::move(fresh);
  heard_.clear();
  status_.clear();
  if (had_numbers) {
    for (Observer& observer : observers_) {
      observer.OnVoiceNumbers(false);
    }
  }
  NotifyCaption();
}

void VoiceController::StopActivity() {
  CancelRun();
  SetStatus("Stopped");
}

void VoiceController::StopSpeaking() {
  if (speech_) {
    speech_->Stop();
  }
}

bool VoiceController::OthersSpeaking() const {
  return speech_ && speech_->OthersSpeaking();
}

bool VoiceController::SpeechInEchoReference() const {
  return speech_ && speech_->InEchoReference();
}

void VoiceController::SayNotice(const std::string& text) {
  const std::string notice = SpeechSafeText(text, kMaxNoticeChars);
  if (notice.empty() || !speech_) {
    return;
  }
  for (Observer& observer : observers_) {
    observer.OnVoiceSpeech(notice);
  }
  speech_->Speak(notice, state_.speech_rate, /*interrupt=*/true);
  SetStatus(notice);
}

void VoiceController::SetListening(ListeningCaption caption) {
  if (caption == listening_) {
    return;
  }
  listening_ = std::move(caption);
  NotifyCaption();
}

void VoiceController::SetNumbersShown(bool shown) {
  state_.numbers_shown = shown;
}

void VoiceController::CancelRun(bool keep_planner) {
  if (!keep_planner) {
    CancelPlanner();
  }
  ++run_;
  run_weak_factory_.InvalidateWeakPtrs();
  // Reading stops too, keeping its place (before the voice stops).
  reader_->Interrupt();
  pending_.clear();
  wait_.reset();
  quiet_timer_.Stop();
  deadline_timer_.Stop();
  autoplay_timer_.Stop();
  submitting_ = false;
  if (speech_) {
    speech_->Stop();
  }
}

void VoiceController::AttachToActiveTab() {
  content::WebContents* contents = ActiveContents();
  if (attached_ && attached_.get() == contents) {
    return;
  }
  DetachFromTab();
  if (!contents) {
    return;
  }
  VoicePageController::CreateForWebContents(contents);
  VoicePageController::FromWebContents(contents)->Start();
  attached_ = contents->GetWeakPtr();
  // What the page tries with the activation voice mode's actions give it is
  // asked about, or said, here (activation_guard.h).
  ActivationGuard::SetAsker(contents, activation::Asker::kVoiceMode, this);
  watcher_ = std::make_unique<TabWatcher>(this, contents);
  // The numbers and the ring follow the page; when they hide by
  // themselves, they are shown again from a new map.
  VoiceOverlay::CreateForWebContents(contents);
  VoiceOverlay::FromWebContents(contents)->SetHiddenCallback(
      base::BindRepeating(
          [](base::WeakPtr<VoiceController> self, int run,
             OverlayHidden reason) {
            if (self) {
              self->OnOverlayHidden(run, static_cast<int>(reason));
            }
          },
          weak_factory_.GetWeakPtr(), ++tab_run_));
  if (state_.numbers_shown) {
    refresh_attempts_ = 0;
    RefreshNumbers();
  }
}

void VoiceController::DetachFromTab() {
  watcher_.reset();
  refresh_numbers_timer_.Stop();
  if (attached_) {
    // Its question, if any, is answered "no" by the guard.
    page_request_timer_.Stop();
    page_request_question_ = 0;
    ActivationGuard::RemoveAsker(attached_.get(), this);
    if (VoiceOverlay* overlay =
            VoiceOverlay::FromWebContents(attached_.get())) {
      overlay->SetHiddenCallback(VoiceOverlay::HiddenCallback());
      overlay->HideNumbers();
      overlay->ClearCurrentItem();
    }
    if (VoicePageController* page =
            VoicePageController::FromWebContents(attached_.get())) {
      page->Stop();
    }
  }
  attached_.reset();
}

void VoiceController::OnActiveTabChanged(BrowserWindowInterface* browser) {
  ++page_id_;
  reader_->PageChanged(page_id_);
  if (active_) {
    AttachToActiveTab();
  }
}

void VoiceController::OnTabGone() {
  watcher_.reset();
  attached_.reset();
  page_request_timer_.Stop();
  page_request_question_ = 0;
}

void VoiceController::SetPlannerForTesting(
    std::unique_ptr<VoicePlanner> planner) {
  CancelPlanner();
  planner_ = std::move(planner);
}

void VoiceController::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void VoiceController::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void VoiceController::SetIdleCallbackForTesting(
    base::RepeatingClosure callback) {
  idle_for_testing_ = std::move(callback);
}

bool VoiceController::busy() const {
  return !pending_.empty() || wait_ || submitting_ ||
         (planner_running_ && planner_question_ == 0);
}

VoiceActivity VoiceController::activity() const {
  if (!pending_.empty() || wait_ || submitting_) {
    return VoiceActivity::kThinking;
  }
  // While the planner works, what it says is the browser speaking (the
  // listening side treats what it hears then as the browser's own voice).
  if (speech_ && speech_->speaking()) {
    return VoiceActivity::kSpeaking;
  }
  if (busy()) {
    return VoiceActivity::kThinking;
  }
  return active_ ? VoiceActivity::kListening : VoiceActivity::kOff;
}

void VoiceController::NotifyCaption() {
  const VoiceActivity now = activity();
  for (Observer& observer : observers_) {
    observer.OnVoiceCaption(now, heard_, status_);
  }
  if (!browser_) {
    return;
  }
  // The caption bar: up for as long as voice mode is on.
  if (!active_) {
    VoiceOverlay::HideCaptionForWindow(browser_);
    return;
  }
  VoiceState state = VoiceState::kListening;
  switch (now) {
    case VoiceActivity::kThinking:
      state = VoiceState::kThinking;
      break;
    case VoiceActivity::kSpeaking:
      state = VoiceState::kSpeaking;
      break;
    case VoiceActivity::kOff:
    case VoiceActivity::kListening:
      if (!listening_.listening && !listening_.problem.empty()) {
        // It can't listen: off, and why.
        state = VoiceState::kOff;
      } else if (listening_.waiting || listening_.starting) {
        state = VoiceState::kWaiting;
      } else {
        state = VoiceState::kListening;
      }
      break;
  }
  const std::string& heard =
      listening_.heard_live.empty() ? heard_ : listening_.heard_live;
  const std::string& status =
      now == VoiceActivity::kListening && !listening_.problem.empty()
          ? listening_.problem
          : status_;
  VoiceOverlay::SetCaptionForWindow(browser_, state, heard, status);
}

void VoiceController::SetStatus(std::string status) {
  status_ = std::move(status);
  NotifyCaption();
}

void VoiceController::OnSpeakingChanged(bool speaking) {
  NotifyCaption();
  MaybeStopWhenQuiet();
}

void VoiceController::MaybeStopWhenQuiet() {
  if (stop_when_quiet_ && !busy() && !(speech_ && speech_->speaking())) {
    Stop();
  }
}

// ---- The overlay ------------------------------------------------------------

void VoiceController::ShowNumbersOnPage(bool shown) {
  content::WebContents* contents = active_ ? ActiveContents() : nullptr;
  if (!contents) {
    return;
  }
  VoiceOverlay::CreateForWebContents(contents);
  VoiceOverlay* overlay = VoiceOverlay::FromWebContents(contents);
  if (shown) {
    overlay->ShowNumbers(CurrentMap());
  } else {
    overlay->HideNumbers();
  }
}

void VoiceController::SetCurrentItemOnPage(int number) {
  content::WebContents* contents = active_ ? ActiveContents() : nullptr;
  if (!contents) {
    return;
  }
  VoiceOverlay::CreateForWebContents(contents);
  VoiceOverlay* overlay = VoiceOverlay::FromWebContents(contents);
  if (number <= 0) {
    overlay->ClearCurrentItem();
  } else {
    overlay->SetCurrentItem(CurrentMap(), number);
  }
}

bool VoiceController::NumberStillOnScreen(const VoiceCommand& command,
                                          const PageMap& map) {
  // A number in a list of tabs or headings is a position, not one shown.
  if (!state_.numbers_shown || command.kind != VoiceCommandKind::kChoose ||
      command.choice.kind != ChoiceRef::Kind::kNumber ||
      state_.list == ListContext::kTabs ||
      state_.list == ListContext::kHeadings) {
    return true;
  }
  content::WebContents* contents = active_ ? ActiveContents() : nullptr;
  VoiceOverlay* overlay =
      contents ? VoiceOverlay::FromWebContents(contents) : nullptr;
  const int number = command.choice.number;
  const PageItem* now = map.ItemByNumber(number);
  if (overlay && overlay->numbers_shown()) {
    const OverlayItem* drawn = nullptr;
    for (const OverlayItem& item : overlay->items()) {
      if (item.number == number) {
        drawn = &item;
        break;
      }
    }
    // No such number at all: the planner says so.
    if (!drawn && !now) {
      return true;
    }
    if (drawn && now && drawn->handle == now->handle) {
      return true;
    }
  }
  // The numbers on screen are gone (the page changed or navigated) or no
  // longer match the page: nothing is done with a stale number.
  heard_ = SpeechSafeText(command.heard, kMaxNoticeChars);
  ShowNumbersOnPage(true);
  SayNotice(
      "The page changed, so its numbers moved. Say the number you see now.");
  return false;
}

void VoiceController::OnOverlayHidden(int run_of_tab, int reason) {
  if (run_of_tab != tab_run_ || !active_ || !state_.numbers_shown) {
    return;
  }
  switch (static_cast<OverlayHidden>(reason)) {
    case OverlayHidden::kPageChanged:
      // The numbering moved: number the page again now (in this task, so
      // nothing flickers).
      ShowNumbersOnPage(true);
      return;
    case OverlayHidden::kNavigated:
      // Another page: number it once its tree has come.
      refresh_attempts_ = 0;
      refresh_numbers_timer_.Start(
          FROM_HERE, kNumbersRefreshDelay,
          base::BindOnce(&VoiceController::RefreshNumbers,
                         base::Unretained(this)));
      return;
    case OverlayHidden::kTabHidden:
      // The newly active tab gets them (AttachToActiveTab()).
      return;
  }
}

void VoiceController::RefreshNumbers() {
  if (!active_ || !state_.numbers_shown) {
    return;
  }
  VoicePageController* page = ActivePage();
  content::WebContents* contents = ActiveContents();
  const bool ready =
      page && page->HasPageTree() && contents && !contents->IsLoading();
  if (!ready && ++refresh_attempts_ < kMaxNumbersRefreshAttempts) {
    refresh_numbers_timer_.Start(
        FROM_HERE, kNumbersRefreshDelay,
        base::BindOnce(&VoiceController::RefreshNumbers,
                       base::Unretained(this)));
    return;
  }
  ShowNumbersOnPage(true);
}

// ---- Context --------------------------------------------------------------

PageMap VoiceController::CurrentMap() {
  VoicePageController* page = ActivePage();
  if (!page || !page->active()) {
    return PageMap();
  }
  return page->BuildMap();
}

VoiceEnvironment VoiceController::Environment() {
  VoiceEnvironment env;
  env.moltnet = IsMoltNet();
  env.listening = listening_.listening;
  env.page_id = page_id_;
  env.reading = reader_->StateFor(page_id_);
  env.now_ms = (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds();
  if (!browser_) {
    return env;
  }
  if (ui::BaseWindow* window = browser_->GetWindow()) {
    env.fullscreen = window->IsFullscreen();
  }
  if (TabStripModel* tabs = browser_->GetTabStripModel()) {
    const int count = tabs->count();
    for (int i = 0; i < count && env.tabs.size() < kMaxTabsListed; ++i) {
      content::WebContents* tab = tabs->GetWebContentsAt(i);
      if (!tab) {
        continue;
      }
      TabInfo info;
      info.title =
          SpeechSafeText(base::UTF16ToUTF8(tab->GetTitle()), kMaxTabTitleChars);
      info.host = std::string(tab->GetLastCommittedURL().host());
      env.tabs.push_back(std::move(info));
    }
    env.active_tab = std::max(tabs->active_index(), 0);
  }
  content::WebContents* contents = ActiveContents();
  if (!contents) {
    env.web_page = false;
    return env;
  }
  const GURL& url = contents->GetLastCommittedURL();
  env.web_page = agent_safety::IsAllowedNavigationURL(url);
  env.host = std::string(url.host());
  env.can_go_back = contents->GetController().CanGoBack();
  env.can_go_forward = contents->GetController().CanGoForward();
  env.muted = contents->IsAudioMuted();
  env.media_playing = contents->GetCurrentlyPlayingVideoCount() > 0;
  if (content::MediaSession* session =
          content::MediaSession::GetIfExists(contents)) {
    media_session::mojom::MediaSessionInfoPtr info =
        session->GetMediaSessionInfoSync();
    if (info) {
      env.media_session =
          info->state !=
          media_session::mojom::MediaSessionInfo::SessionState::kInactive;
      env.media_playing =
          env.media_playing ||
          (env.media_session &&
           info->playback_state ==
               media_session::mojom::MediaPlaybackState::kPlaying);
    }
  }
  return env;
}

// ---- Running --------------------------------------------------------------

void VoiceController::HandleUtterance(const std::string& text) {
  if (!browser_ || !VoiceModeEnabled()) {
    return;
  }
  // Barge-in: what was being said and done stops (the planner's run too,
  // unless this answers its question: below).
  CancelRun(/*keep_planner=*/true);
  commits_at_utterance_ = commits_;
  if (!active_ && !IsMoltNet()) {
    Start();
  }
  // Longer than the grammar takes: it refuses it without reading it all.
  const std::string utterance = text.substr(0, kMaxUtteranceInputBytes);
  const PageMap map = CurrentMap();
  for (Observer& observer : observers_) {
    observer.OnVoicePageMapped(map);
  }
  const VoiceEnvironment env = Environment();
  // As PlanUtterance() does, with the numbers on screen checked between.
  const VoiceCommand command =
      ParseVoiceCommand(utterance, ContextFor(state_, map, env));
  // The guard's question, asked during a planner run, is answered without
  // ending the run.
  if (!AnswersPlannerQuestion(command, map, env) &&
      !AnswersPageRequestQuestion(command, map, env)) {
    CancelPlanner();
  }
  if (!NumberStillOnScreen(command, map)) {
    return;
  }
  VoiceTurn turn = PlanCommand(command, map, env, state_);
  heard_ = turn.heard;
  status_ = turn.status;
  RunTurn(std::move(turn));
  // Any other command drops the guard's question: a "no" for the page.
  DropPageRequestQuestionIfGone();
}

void VoiceController::RunTurn(VoiceTurn turn) {
  // A new speech rate is kept as a setting.
  if (turn.state.speech_rate != state_.speech_rate) {
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
         base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::BindOnce(base::IgnoreResult(&WriteVoiceSpeechRate),
                       turn.state.speech_rate));
  }
  state_ = std::move(turn.state);
  if (!turn.status.empty()) {
    status_ = std::move(turn.status);
  }
  // The activation guard's question, asked while a page loaded, is heard to
  // its end: what follows is said after it.
  interrupt_next_speech_ = page_request_question_ == 0;
  for (VoiceStep& step : turn.steps) {
    pending_.push_back(std::move(step));
  }
  NotifyCaption();
  RunSteps();
}

void VoiceController::RunSteps() {
  base::WeakPtr<VoiceController> self = weak_factory_.GetWeakPtr();
  while (!pending_.empty()) {
    const VoiceStep step = std::move(pending_.front());
    pending_.pop_front();
    const bool go_on = RunStep(step);
    if (!self) {
      return;  // the window closed
    }
    if (!go_on) {
      if (!busy()) {
        Idle();
      }
      return;
    }
  }
  Idle();
}

void VoiceController::Idle() {
  NotifyCaption();
  base::WeakPtr<VoiceController> self = weak_factory_.GetWeakPtr();
  if (!busy() && idle_for_testing_) {
    idle_for_testing_.Run();
  }
  if (self) {
    MaybeStopWhenQuiet();
  }
}

void VoiceController::Fail(StepFailure failure) {
  pending_.clear();
  const std::string_view text = FailureText(failure);
  if (!text.empty()) {
    interrupt_next_speech_ = true;
    Say(std::string(text));
  }
}

void VoiceController::Say(const std::string& text) {
  for (Observer& observer : observers_) {
    observer.OnVoiceSpeech(text);
  }
  speech_->Speak(text, state_.speech_rate, interrupt_next_speech_);
  interrupt_next_speech_ = false;
}

GURL VoiceController::DefaultSearchURL(const std::string& query) {
  Profile* profile = browser_ ? browser_->GetProfile() : nullptr;
  TemplateURLService* service =
      profile ? TemplateURLServiceFactory::GetForProfile(profile) : nullptr;
  const TemplateURL* engine =
      service ? service->GetDefaultSearchProvider() : nullptr;
  if (engine) {
    const GURL url(engine->url_ref().ReplaceSearchTerms(
        TemplateURLRef::SearchTermsArgs(base::UTF8ToUTF16(query)),
        service->search_terms_data()));
    if (agent_safety::IsAllowedNavigationURL(url)) {
      return url;
    }
  }
  // No usable engine: the web agent's (Brave Search).
  const KnownSite* fallback = FindKnownSite("bravesearch");
  return fallback ? GURL(SiteSearchURL(*fallback, query)) : GURL();
}

GURL VoiceController::SearchURLForTesting(const std::string& query) {
  return DefaultSearchURL(query);
}

void VoiceController::Navigate(const GURL& url, bool new_tab) {
  content::WebContents* contents = ActiveContents();
  if (new_tab || !contents) {
    browser_->OpenGURL(url, WindowOpenDisposition::NEW_FOREGROUND_TAB);
    return;
  }
  // What the user asked for, as if typed in the address bar.
  content::NavigationController::LoadURLParams params(url);
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  contents->GetController().LoadURLWithParams(params);
}

bool VoiceController::RunStep(const VoiceStep& step) {
  content::WebContents* contents = ActiveContents();
  VoicePageController* page = ActivePage();
  // A page action without the page: voice mode is off in this tab.
  auto page_result = [&](ActionResult result) {
    if (std::optional<StepFailure> failure = FailureFor(result)) {
      Fail(*failure);
      return false;
    }
    return true;
  };

  switch (step.kind) {
    case VoiceStepKind::kSpeak:
      Say(step.text);
      return true;

    case VoiceStepKind::kNavigate:
    case VoiceStepKind::kSearchWeb: {
      const GURL url = step.kind == VoiceStepKind::kNavigate
                           ? GURL(step.url)
                           : DefaultSearchURL(step.text);
      // Only http(s), as for every agent (agent_safety.h).
      if (!agent_safety::IsAllowedNavigationURL(url)) {
        Fail(StepFailure::kBlockedURL);
        return false;
      }
      Navigate(url, step.new_tab);
      return true;
    }

    case VoiceStepKind::kNewTab:
      if (!chrome::ExecuteCommand(browser_, IDC_NEW_TAB)) {
        Fail(StepFailure::kUnavailable);
        return false;
      }
      return true;

    case VoiceStepKind::kSetText:
      return page_result(page ? page->SetText(step.target, step.text)
                              : ActionResult::kNotActive);

    case VoiceStepKind::kSubmit: {
      if (!page) {
        Fail(StepFailure::kNotActive);
        return false;
      }
      submitting_ = true;
      page->Submit(step.target,
                   base::BindOnce(
                       [](base::WeakPtr<VoiceController> self, int run,
                          ActionResult result) {
                         if (self) {
                           self->OnSubmitted(run, static_cast<int>(result));
                         }
                       },
                       run_weak_factory_.GetWeakPtr(), run_));
      return false;
    }

    case VoiceStepKind::kPress:
      // A press for full screen or to play lets the page go full screen
      // (activation_policy.h).
      return page_result(page ? page->Press(step.target, step.intent)
                              : ActionResult::kNotActive);
    case VoiceStepKind::kPlayMedia:
      return page_result(page ? page->PlayMedia(step.target)
                              : ActionResult::kNotActive);
    case VoiceStepKind::kFocus:
      return page_result(page ? page->Focus(step.target)
                              : ActionResult::kNotActive);
    case VoiceStepKind::kScrollIntoView:
      return page_result(page ? page->ScrollIntoView(step.target)
                              : ActionResult::kNotActive);
    case VoiceStepKind::kIncrement:
    case VoiceStepKind::kDecrement:
      for (int i = 0; i < std::max(step.count, 1); ++i) {
        const ActionResult result =
            !page ? ActionResult::kNotActive
            : step.kind == VoiceStepKind::kIncrement
                ? page->Increment(step.target)
                : page->Decrement(step.target);
        if (!page_result(result)) {
          return false;
        }
      }
      return true;
    case VoiceStepKind::kScrollPage:
      for (int i = 0; i < std::max(step.count, 1); ++i) {
        if (!page_result(page ? page->ScrollPage(ScrollActionFor(step.scroll))
                              : ActionResult::kNotActive)) {
          return false;
        }
        if (step.scroll == PageScroll::kTop ||
            step.scroll == PageScroll::kBottom) {
          break;
        }
      }
      return true;

    case VoiceStepKind::kBrowserCommand: {
      const std::optional<int> id = CommandId(step.command);
      if (!id || !chrome::ExecuteCommand(browser_, *id)) {
        Fail(StepFailure::kUnavailable);
        return false;
      }
      return true;
    }

    case VoiceStepKind::kMedia: {
      switch (step.media) {
        case MediaCommand::kPause:
          return page_result(page ? page->PauseMedia()
                                  : ActionResult::kNotActive);
        case MediaCommand::kResume:
          return page_result(page ? page->ResumeMedia()
                                  : ActionResult::kNotActive);
        case MediaCommand::kSeek:
        case MediaCommand::kSeekToStart:
        case MediaCommand::kNextTrack:
        case MediaCommand::kPreviousTrack:
          break;
      }
      content::MediaSession* session =
          contents && active_ ? content::MediaSession::GetIfExists(contents)
                              : nullptr;
      if (!session) {
        Fail(StepFailure::kNoMediaSession);
        return false;
      }
      switch (step.media) {
        case MediaCommand::kSeek:
          if (step.seconds != 0) {
            session->Seek(base::Seconds(step.seconds));
          }
          break;
        case MediaCommand::kSeekToStart:
          session->SeekTo(base::TimeDelta());
          break;
        case MediaCommand::kNextTrack:
          session->NextTrack();
          break;
        case MediaCommand::kPreviousTrack:
          session->PreviousTrack();
          break;
        case MediaCommand::kPause:
        case MediaCommand::kResume:
          break;
      }
      return true;
    }

    case VoiceStepKind::kMute:
      if (!contents) {
        Fail(StepFailure::kNoTab);
        return false;
      }
      SetTabAudioMuted(contents, step.flag, TabMutedReason::kAudioIndicator,
                       std::string());
      return true;

    case VoiceStepKind::kSelectTab:
    case VoiceStepKind::kCloseTab: {
      TabStripModel* tabs = browser_->GetTabStripModel();
      if (step.kind == VoiceStepKind::kCloseTab && step.index < 0) {
        if (!chrome::ExecuteCommand(browser_, IDC_CLOSE_TAB)) {
          Fail(StepFailure::kNoTab);
          return false;
        }
        return true;
      }
      if (!tabs || step.index < 0 || step.index >= tabs->count()) {
        Fail(StepFailure::kNoTab);
        return false;
      }
      if (step.kind == VoiceStepKind::kSelectTab) {
        tabs->ActivateTabAt(
            step.index, TabStripUserGestureDetails(
                            TabStripUserGestureDetails::GestureType::kOther));
      } else {
        tabs->CloseWebContentsAt(
            step.index, TabCloseTypes::CLOSE_USER_GESTURE |
                            TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
      }
      return true;
    }

    case VoiceStepKind::kShowNumbers:
      ShowNumbersOnPage(step.flag);
      for (Observer& observer : observers_) {
        observer.OnVoiceNumbers(step.flag);
      }
      return true;

    case VoiceStepKind::kSetCurrentItem:
      SetCurrentItemOnPage(step.index);
      for (Observer& observer : observers_) {
        observer.OnVoiceCurrentItem(step.index);
      }
      return true;

    case VoiceStepKind::kSetVoiceMode:
      // Off: the microphone closes now (the listening side); voice mode
      // itself once it has said so.
      stop_when_quiet_ = !step.flag;
      for (Observer& observer : observers_) {
        observer.OnVoiceModeRequested(step.flag);
      }
      return true;

    case VoiceStepKind::kWaitForPage:
      BeginWait(step);
      return false;

    case VoiceStepKind::kToPlanner: {
      if (!planner_) {
        Say(step.preface);
        return true;
      }
      CancelPlanner();
      planner_running_ = true;
      planner_question_ = 0;
      // What the user said is the run's goal; the planner works in the
      // active tab, on the page this map is of.
      planner_->Start(contents, step.text, CurrentMap(), this);
      return true;
    }

    case VoiceStepKind::kRead:
      // The reader says what it reads and does (voice_reader.h).
      reader_->Run(step.read, step.text, page_id_, state_.speech_rate);
      return true;

    case VoiceStepKind::kPageRequestAnswer:
      // Only the guard's question pending now, on the tab it was about.
      if (page_request_question_ != 0 &&
          step.index == page_request_question_) {
        AnswerPageRequest(step.flag);
      }
      return true;

    case VoiceStepKind::kPlannerAnswer:
      // Only the question pending now; an older one's answer is dropped.
      if (planner_ && planner_running_ && planner_question_ != 0 &&
          step.index == planner_question_) {
        planner_question_ = 0;
        planner_question_timer_.Stop();
        planner_->Answer(step.index, step.flag);
      }
      return true;
  }
  return true;
}

// ---- The activation guard ---------------------------------------------------

bool VoiceController::AskAboutPageRequest(
    int id,
    const activation::Question& question) {
  if (!active_ || !speech_ || IsMoltNet() || id <= 0) {
    return false;
  }
  // A question of the loop's own (a purchase, the planner's) keeps its
  // place: the page's request is refused instead.
  const VoiceEnvironment env = Environment();
  if (state_.confirming && env.now_ms < state_.confirm_expires_ms &&
      state_.confirm_page_id == env.page_id) {
    return false;
  }
  VoiceTurn turn = PlanPageRequestQuestion(id, question.said, question.verb,
                                           env, state_);
  if (PageRequestQuestionHeld(turn.state) != id) {
    return false;
  }
  page_request_question_ = id;
  page_request_tab_ = attached_;
  page_request_timer_.Start(
      FROM_HERE, base::Milliseconds(kConfirmationTimeoutMs),
      base::BindOnce(&VoiceController::OnPageRequestTimeout,
                     base::Unretained(this)));
  state_ = std::move(turn.state);
  if (!turn.status.empty()) {
    status_ = std::move(turn.status);
  }
  // Said after what is being said now; not through the steps, which may be
  // waiting for a page.
  interrupt_next_speech_ = false;
  for (const VoiceStep& step : turn.steps) {
    if (step.kind == VoiceStepKind::kSpeak) {
      Say(step.text);
    }
  }
  NotifyCaption();
  return true;
}

void VoiceController::TellAboutPageRequest(const std::string& text) {
  if (!active_ || !speech_) {
    return;
  }
  const std::string said = SpeechSafeText(text, kMaxNoticeChars);
  if (said.empty()) {
    return;
  }
  for (Observer& observer : observers_) {
    observer.OnVoiceSpeech(said);
  }
  speech_->Speak(said, state_.speech_rate, /*interrupt=*/false);
  SetStatus(said);
}

void VoiceController::PageRequestQuestionWithdrawn(int id) {
  if (id == 0 || id != page_request_question_) {
    return;
  }
  page_request_timer_.Stop();
  page_request_question_ = 0;
  if (PageRequestQuestionHeld(state_) == id) {
    state_.confirming = false;
    state_.confirm_steps.clear();
    state_.confirm_what.clear();
  }
}

bool VoiceController::AnswersPageRequestQuestion(
    const VoiceCommand& command,
    const PageMap& map,
    const VoiceEnvironment& env) const {
  return page_request_question_ != 0 &&
         (command.kind == VoiceCommandKind::kYes ||
          command.kind == VoiceCommandKind::kNo) &&
         ContextFor(state_, map, env).expecting_yes_no &&
         PageRequestQuestionHeld(state_) == page_request_question_;
}

void VoiceController::AnswerPageRequest(bool allow) {
  const int id = page_request_question_;
  page_request_question_ = 0;
  page_request_timer_.Stop();
  content::WebContents* tab = page_request_tab_.get();
  ActivationGuard* guard = tab ? ActivationGuard::FromWebContents(tab) : nullptr;
  if (guard && id != 0) {
    guard->Answer(id, allow);
  }
}

void VoiceController::OnPageRequestTimeout() {
  if (page_request_question_ == 0) {
    return;
  }
  if (PageRequestQuestionHeld(state_) == page_request_question_) {
    state_.confirming = false;
    state_.confirm_steps.clear();
    state_.confirm_what.clear();
  }
  AnswerPageRequest(false);
}

void VoiceController::DropPageRequestQuestionIfGone() {
  if (page_request_question_ != 0 &&
      PageRequestQuestionHeld(state_) != page_request_question_) {
    AnswerPageRequest(false);
  }
}

// ---- The planner ------------------------------------------------------------

bool VoiceController::AnswersPlannerQuestion(
    const VoiceCommand& command,
    const PageMap& map,
    const VoiceEnvironment& env) const {
  return planner_running_ && planner_question_ != 0 &&
         (command.kind == VoiceCommandKind::kYes ||
          command.kind == VoiceCommandKind::kNo) &&
         ContextFor(state_, map, env).expecting_yes_no &&
         PlannerQuestionHeld(state_) == planner_question_;
}

void VoiceController::CancelPlanner() {
  planner_question_timer_.Stop();
  planner_question_ = 0;
  if (!planner_running_) {
    return;
  }
  planner_running_ = false;
  if (planner_) {
    planner_->Cancel();
  }
}

void VoiceController::PlannerSays(const std::string& text) {
  if (!planner_running_ || !speech_) {
    return;
  }
  // Model output and page text: cleaned and capped, as page text is.
  const std::string said = SpeechSafeText(text, kMaxPlannerSpeechChars);
  if (said.empty()) {
    return;
  }
  for (Observer& observer : observers_) {
    observer.OnVoiceSpeech(said);
  }
  // In turn: each step after the one before.
  speech_->Speak(said, state_.speech_rate, /*interrupt=*/false);
  std::vector<std::string>& last = state_.last_spoken;
  last.push_back(said);
  if (last.size() > kMaxPlannerSpeechRemembered) {
    last.erase(last.begin());
  }
  SetStatus(said);
}

void VoiceController::PlannerAsks(int question, const std::string& what) {
  if (!planner_running_ || question <= 0) {
    return;
  }
  // Asked as the loop asks before pressing Buy, and answered only by a
  // "yes" or "no" heard in that context.
  VoiceTurn turn =
      PlanPlannerQuestion(question, what, Environment(), state_);
  if (PlannerQuestionHeld(turn.state) != question) {
    CancelPlanner();
    Idle();
    return;
  }
  planner_question_ = question;
  planner_question_timer_.Start(
      FROM_HERE, base::Milliseconds(kConfirmationTimeoutMs),
      base::BindOnce(&VoiceController::OnPlannerQuestionTimeout,
                     base::Unretained(this)));
  interrupt_next_speech_ = false;
  state_ = std::move(turn.state);
  if (!turn.status.empty()) {
    status_ = std::move(turn.status);
  }
  for (VoiceStep& step : turn.steps) {
    pending_.push_back(std::move(step));
  }
  NotifyCaption();
  RunSteps();
}

void VoiceController::OnPlannerQuestionTimeout() {
  if (!planner_running_ || planner_question_ == 0) {
    return;
  }
  CancelPlanner();
  Say(kPlannerNoAnswer);
  Idle();
}

void VoiceController::PlannerFinished() {
  if (!planner_running_) {
    return;
  }
  planner_running_ = false;
  planner_question_ = 0;
  planner_question_timer_.Stop();
  if (!busy()) {
    Idle();
  } else {
    NotifyCaption();
  }
}

void VoiceController::OnSubmitted(int run, int result) {
  if (run != run_) {
    return;
  }
  submitting_ = false;
  if (std::optional<StepFailure> failure =
          FailureFor(static_cast<ActionResult>(result))) {
    Fail(*failure);
    Idle();
    return;
  }
  // Submit() may answer before it returns: the steps go on from a task.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&VoiceController::RunSteps,
                                run_weak_factory_.GetWeakPtr()));
}

// ---- Waiting for a page ---------------------------------------------------

void VoiceController::BeginWait(const VoiceStep& step) {
  wait_ = std::make_unique<PageWait>();
  wait_->step = step;
  wait_->started = base::TimeTicks::Now();
  // The navigation may have committed already (a submit answers once the
  // page has its Enter), or be on its way.
  if (commits_ > commits_at_utterance_) {
    wait_->navigated = true;
    wait_->committed = last_commit_;
  }
  content::WebContents* contents = ActiveContents();
  wait_->navigation_started = contents && contents->IsLoading();
  deadline_timer_.Start(FROM_HERE, kMaxPageWait,
                        base::BindOnce(&VoiceController::OnWaitDeadline,
                                       run_weak_factory_.GetWeakPtr()));
  RestartQuietTimer();
  NotifyCaption();
}

void VoiceController::RestartQuietTimer() {
  if (!wait_ || wait_->autoplay) {
    return;
  }
  quiet_timer_.Start(FROM_HERE, kQuietTree,
                     base::BindOnce(&VoiceController::OnQuiet,
                                    run_weak_factory_.GetWeakPtr()));
}

void VoiceController::OnNavigationStarted() {
  if (wait_) {
    wait_->navigation_started = true;
  }
}

void VoiceController::OnNavigationCommitted() {
  // Another page: lists, fields and questions of the last one are gone
  // (same-document navigations included: the page the user sees changed).
  ++page_id_;
  reader_->PageChanged(page_id_);
  ++commits_;
  last_commit_ = base::TimeTicks::Now();
  if (wait_) {
    wait_->navigated = true;
    wait_->navigation_started = false;
    wait_->committed = base::TimeTicks::Now();
  }
  RestartQuietTimer();
}

void VoiceController::OnNavigationEnded() {
  if (wait_) {
    wait_->navigation_started = false;
  }
  RestartQuietTimer();
}

void VoiceController::OnLoadingStopped() {
  RestartQuietTimer();
}

void VoiceController::OnTreeChanged() {
  if (!wait_ || wait_->autoplay) {
    return;
  }
  // A page whose tree never rests (a clock, a playing video) is read once
  // it has had a few seconds.
  if (wait_->navigated &&
      base::TimeTicks::Now() - wait_->committed > kMaxSettle) {
    OnQuiet();
    return;
  }
  RestartQuietTimer();
}

void VoiceController::OnQuiet() {
  if (!wait_ || wait_->autoplay) {
    return;
  }
  const base::TimeDelta waited = base::TimeTicks::Now() - wait_->started;
  // Still expecting the navigation the steps started.
  if (!wait_->navigated &&
      (wait_->navigation_started || waited < kNavigationGrace)) {
    RestartQuietTimer();
    return;
  }
  content::WebContents* contents = ActiveContents();
  VoicePageController* page = ActivePage();
  if (contents && page && !page->HasPageTree() && !wait_->tree_asked &&
      !contents->IsLoading()) {
    // Loaded, but no tree came: ask the renderers for their trees again.
    wait_->tree_asked = true;
    contents->ResetAccessibility();
  }
  if ((contents && contents->IsLoading()) || (page && !page->HasPageTree())) {
    RestartQuietTimer();
    return;
  }
  FinishWait();
}

void VoiceController::OnWaitDeadline() {
  FinishWait();
}

void VoiceController::FinishWait() {
  if (!wait_) {
    return;
  }
  quiet_timer_.Stop();
  deadline_timer_.Stop();
  const VoiceStep step = wait_->step;
  if (step.follow_up == FollowUpKind::kPlayOpened && !wait_->autoplay) {
    // A site that plays what it opens (YouTube does) gets a moment to;
    // clicking its video then would pause it.
    content::WebContents* contents = ActiveContents();
    if (contents && contents->GetCurrentlyPlayingVideoCount() == 0 &&
        !CurrentMap().media.empty()) {
      wait_->autoplay = true;
      autoplay_timer_.Start(FROM_HERE, kAutoplayGrace,
                            base::BindOnce(&VoiceController::OnAutoplayWaitOver,
                                           run_weak_factory_.GetWeakPtr()));
      return;
    }
  }
  wait_.reset();
  PlanFollowUpNow(step);
}

void VoiceController::OnMediaStarted() {
  if (wait_ && wait_->autoplay) {
    OnAutoplayWaitOver();
  }
}

void VoiceController::OnAutoplayWaitOver() {
  autoplay_timer_.Stop();
  if (!wait_) {
    return;
  }
  const VoiceStep step = wait_->step;
  wait_.reset();
  PlanFollowUpNow(step);
}

void VoiceController::PlanFollowUpNow(const VoiceStep& step) {
  const PageMap map = CurrentMap();
  for (Observer& observer : observers_) {
    observer.OnVoicePageMapped(map);
  }
  RunTurn(PlanFollowUp(step, map, Environment(), state_));
}

}  // namespace voice
}  // namespace molt_ai
