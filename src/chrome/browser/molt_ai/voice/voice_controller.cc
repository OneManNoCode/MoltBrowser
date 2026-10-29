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
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
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
  AttachToActiveTab();
  NotifyCaption();
  return true;
}

void VoiceController::Stop() {
  CancelRun();
  DetachFromTab();
  active_ = false;
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

void VoiceController::SetNumbersShown(bool shown) {
  state_.numbers_shown = shown;
}

void VoiceController::CancelRun() {
  ++run_;
  run_weak_factory_.InvalidateWeakPtrs();
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
  watcher_ = std::make_unique<TabWatcher>(this, contents);
}

void VoiceController::DetachFromTab() {
  watcher_.reset();
  if (attached_) {
    if (VoicePageController* page =
            VoicePageController::FromWebContents(attached_.get())) {
      page->Stop();
    }
  }
  attached_.reset();
}

void VoiceController::OnActiveTabChanged(BrowserWindowInterface* browser) {
  ++page_id_;
  if (active_) {
    AttachToActiveTab();
  }
}

void VoiceController::OnTabGone() {
  watcher_.reset();
  attached_.reset();
}

void VoiceController::SetPlannerHook(PlannerHook hook) {
  planner_hook_ = std::move(hook);
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
  return !pending_.empty() || wait_ || submitting_;
}

VoiceActivity VoiceController::activity() const {
  if (busy()) {
    return VoiceActivity::kThinking;
  }
  if (speech_ && speech_->speaking()) {
    return VoiceActivity::kSpeaking;
  }
  return active_ ? VoiceActivity::kListening : VoiceActivity::kOff;
}

void VoiceController::NotifyCaption() {
  const VoiceActivity now = activity();
  for (Observer& observer : observers_) {
    observer.OnVoiceCaption(now, heard_, status_);
  }
}

void VoiceController::SetStatus(std::string status) {
  status_ = std::move(status);
  NotifyCaption();
}

void VoiceController::OnSpeakingChanged(bool speaking) {
  NotifyCaption();
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
  env.page_id = page_id_;
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
  // Barge-in: what was being said and done stops.
  CancelRun();
  commits_at_utterance_ = commits_;
  if (!active_ && !IsMoltNet()) {
    Start();
  }
  // Longer than the grammar takes: it refuses it without reading it all.
  const std::string utterance = text.substr(0, kMaxUtteranceInputBytes);
  const PageMap map = CurrentMap();
  VoiceTurn turn = PlanUtterance(utterance, map, Environment(), state_);
  heard_ = turn.heard;
  status_ = turn.status;
  RunTurn(std::move(turn));
}

void VoiceController::RunTurn(VoiceTurn turn) {
  state_ = std::move(turn.state);
  if (!turn.status.empty()) {
    status_ = std::move(turn.status);
  }
  interrupt_next_speech_ = true;
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
  if (!busy() && idle_for_testing_) {
    idle_for_testing_.Run();
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
      return page_result(page ? page->Press(step.target)
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
      for (Observer& observer : observers_) {
        observer.OnVoiceNumbers(step.flag);
      }
      return true;

    case VoiceStepKind::kSetCurrentItem:
      for (Observer& observer : observers_) {
        observer.OnVoiceCurrentItem(step.index);
      }
      return true;

    case VoiceStepKind::kSetVoiceMode:
      for (Observer& observer : observers_) {
        observer.OnVoiceModeRequested(step.flag);
      }
      return true;

    case VoiceStepKind::kWaitForPage:
      BeginWait(step);
      return false;

    case VoiceStepKind::kToPlanner:
      if (planner_hook_) {
        planner_hook_.Run(step.text, CurrentMap());
      } else {
        Say(step.preface);
      }
      return true;
  }
  return true;
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
  RunTurn(PlanFollowUp(step, map, Environment(), state_));
}

}  // namespace voice
}  // namespace molt_ai
