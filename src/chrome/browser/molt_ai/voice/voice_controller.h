// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode for one browser window (docs/design/TOUCHLESS_BROWSING.md §2,
// §5, §6): what the user said (typed for now; the speech service comes
// next) becomes what they meant, done on the page, with a spoken answer.
//
//   HandleUtterance("search for Bohemian Rhapsody")
//     -> the page map of the active tab (VoicePageController),
//        what the window knows (VoiceEnvironment),
//        PlanUtterance() (voice_controller_logic.h): steps and words,
//     -> the steps, in order: accessibility actions through the tab's
//        VoicePageController, browser commands, the media session, the
//        tab's mute; speech through VoiceSpeech.
//
// Navigation and submits are asynchronous: a kWaitForPage step waits until
// the navigation commits, the page stops loading and its accessibility
// tree has been quiet for a moment (or a deadline passes), then plans the
// follow-up ("20 results. 1: ..."). A page opened to play waits briefly
// for the site's own autoplay before it clicks the video.
//
//   - Stop: a new utterance, "stop" and StopActivity() cancel the steps
//     still to run, a wait, a pending submit, and voice mode's speech.
//   - The active tab only: voice mode holds the accessibility mode on the
//     active tab while it is on (VoicePageController::Start()), and moves
//     it when another tab becomes active.
//   - MoltNet windows: Start() refuses (§7.4), no tree is ever taken, and
//     an utterance only hears "Voice mode is off in MoltNet windows."
//   - Every URL is checked with agent_safety::IsAllowedNavigationURL()
//     before it loads; every page action names a node of the map, which the
//     page controller finds again (and refuses when the page changed).
//
// The planner fallback (voice_planner.h, TOUCHLESS_BROWSING.md §6.2): an
// utterance the grammar has no command for goes to the web agent's
// on-device planner (agents/voice_agent_planner.h), which works in the
// active tab: each planned step is said before it runs, its questions wait
// for a spoken "yes" through the loop's confirmation (30 s at most), and
// anything else the user says, "stop" included, ends its run. What it says
// is cleaned with SpeechSafeText().
//
// The activation guard (molt_ai/security/activation_guard.h,
// TOUCHLESS_BROWSING.md §7.5): voice mode is the asker on the tab it is
// attached to. What a page tries with the activation voice mode's actions
// gave it (a new window, a download, another app) is asked about through
// the loop's confirmation ("The page tried to open a new window to
// ads.example. Say yes to open it, or no to cancel."); what the guard
// refused (full screen, the clipboard, a permission prompt) is said.
//
// Reading aloud (voice_reader.h, TOUCHLESS_BROWSING.md §5.4): the loop's
// kRead steps go to the window's reader, which reads the active tab's text
// a chunk at a time through VoiceSpeech; every chunk is said to observers
// (OnVoiceSpeech) as it starts. A new utterance or "stop" stops it,
// keeping its place (VoiceEnvironment::reading). The speech rate is kept
// as a setting (voice_reading_settings.h).
//
// The window owns nothing of it: the controller is created on first use
// and deletes itself when the window closes. The agent side panel's voice
// field and the window's listening (voice_session.h) listen through
// Observer.
//
// The overlay (chrome/browser/ui/views/molt_ai/voice/voice_overlay.h,
// TOUCHLESS_BROWSING.md §5.3, §5.5, §6.6) is driven from here:
//   - the caption bar is up for as long as voice mode is on, with the state
//     (the listening side's waiting or listening, else thinking or
//     speaking), what was heard (live while it is being said) and what is
//     happening;
//   - "show numbers" and "hide numbers" show and hide the numbers on the
//     active tab, and each step that acts on or reads an item rings it
//     (kSetCurrentItem);
//   - when the numbers hide by themselves (the hidden callback), they are
//     shown again from a new map: at once when the page's numbering
//     changed, once the new page's tree has come after a navigation, and on
//     the newly active tab after a tab switch.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/security/page_request_asker.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"
#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;
class GURL;

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

class VoicePageController;
class VoiceReader;
class VoiceSpeech;

// What voice mode is doing, for the caption bar (§5.1).
enum class VoiceActivity {
  kOff,
  kListening,  // on, waiting for the user
  kThinking,   // running the steps of what the user said
  kSpeaking,
};

class VoiceController : public VoicePlanner::Delegate,
                        public activation::PageRequestAsker {
 public:
  DECLARE_USER_DATA(VoiceController);

  class Observer : public base::CheckedObserver {
   public:
    // The caption bar: the state, what was heard ("play the live one by
    // Queen") and what is happening ("Playing: ..."), each cleaned.
    virtual void OnVoiceCaption(VoiceActivity activity,
                                const std::string& heard,
                                const std::string& status) {}
    // What the browser says, as it starts saying it.
    virtual void OnVoiceSpeech(const std::string& text) {}
    // "Show numbers" / "hide numbers".
    virtual void OnVoiceNumbers(bool shown) {}
    // The item (its number in the page map) the browser acts on or reads;
    // 0 for none.
    virtual void OnVoiceCurrentItem(int item_number) {}
    // "Turn on voice mode" / "turn off voice mode" (after its "yes"): the
    // window's listening (voice_session.h) starts or stops.
    virtual void OnVoiceModeRequested(bool on) {}
    // The page map an utterance or a follow-up was planned on (hotwords for
    // the recognizer, voice_session.h).
    virtual void OnVoicePageMapped(const PageMap& map) {}
    // The window is closing: stop observing |controller|.
    virtual void OnVoiceControllerDestroying(VoiceController* controller) {}
  };

  // What the listening side (voice_session.h) shows in the caption bar.
  struct ListeningCaption {
    // The microphone is on (or starting).
    bool listening = false;
    // Only the wake phrase counts now ("Waiting"), or it is getting ready.
    bool waiting = false;
    bool starting = false;
    // What is being said, live (the recognizer's partial text).
    std::string heard_live;
    // Why it can't listen ("Microphone access is off"), shown while idle.
    std::string problem;

    friend bool operator==(const ListeningCaption&,
                           const ListeningCaption&) = default;
  };

  // How long a wait for a page lasts at most, how quiet its tree must be,
  // how long it waits for a navigation to start, and for a site's autoplay.
  static constexpr base::TimeDelta kMaxPageWait = base::Seconds(15);
  static constexpr base::TimeDelta kQuietTree = base::Milliseconds(500);
  static constexpr base::TimeDelta kMaxSettle = base::Seconds(4);
  static constexpr base::TimeDelta kNavigationGrace = base::Milliseconds(2500);
  static constexpr base::TimeDelta kAutoplayGrace = base::Milliseconds(1500);
  // An utterance longer than this is refused before it is parsed (the
  // grammar refuses past kMaxUtteranceBytes too).
  static constexpr size_t kMaxUtteranceInputBytes = 4096;

  // The window's controller, created on first use; null for a window that
  // isn't a normal tabbed window, or is closing.
  static VoiceController* GetOrCreateForBrowser(
      BrowserWindowInterface* browser);
  static VoiceController* FromBrowser(BrowserWindowInterface* browser);

  VoiceController(const VoiceController&) = delete;
  VoiceController& operator=(const VoiceController&) = delete;
  ~VoiceController() override;

  // Voice mode on and off for the window. Start() returns false in a
  // MoltNet window (§7.4) and while #molt-voice-mode is off. Stop() cancels
  // what runs, stops the speech and releases the page.
  bool Start();
  void Stop();
  bool active() const { return active_; }

  // One utterance, as typed or recognized. Starts voice mode when it is
  // off (except in MoltNet windows), stops whatever runs, then plans and
  // runs the new one. Does nothing while #molt-voice-mode is off.
  void HandleUtterance(const std::string& text);

  // Stops the speech and whatever runs, as "stop" does, staying on.
  void StopActivity();

  // Stops voice mode's speech only (barge-in: the user started to speak).
  // A page's speech is left alone (voice_speech.h).
  void StopSpeaking();

  // Whether the speech engine is speaking for someone else (a page's
  // speechSynthesis): the microphone may hear it (voice_speech.h).
  bool OthersSpeaking() const;
  // Whether voice mode speaks through its own voice now, whose audio is in
  // the echo canceller's reference (voice_speech.h).
  bool SpeechInEchoReference() const;

  // Says |text| (what the listening side has to tell: "Voice mode is on",
  // "I can't use the microphone") and shows it as the status.
  void SayNotice(const std::string& text);

  // The listening side's part of the caption, and whether it listens
  // (VoiceEnvironment::listening).
  void SetListening(ListeningCaption caption);

  // The numbers overlay shows numbers without "show numbers" being said
  // (on by default in voice mode, §5.3): lists are then read, and numbers
  // chosen, as they are shown.
  void SetNumbersShown(bool shown);

  // The planner open-ended requests go to (§6.2, layer 3): the web agent's
  // (agents/voice_agent_planner.h) unless replaced; null, the browser says
  // it can't do that yet.
  void SetPlannerForTesting(std::unique_ptr<VoicePlanner> planner);
  // A planner run is in progress (it may be waiting for an answer).
  bool planner_running() const { return planner_running_; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  VoiceActivity activity() const;
  const ConversationState& state() const { return state_; }
  // Steps, a wait for a page, a submit or a planner run (not while it waits
  // for the user's answer) are still running.
  bool busy() const;

  // Runs whenever the controller becomes idle (no step, wait or submit
  // left), for tests.
  void SetIdleCallbackForTesting(base::RepeatingClosure callback);
  // The default search engine's results page for |query| (the browser's
  // engine, else the web agent's).
  GURL SearchURLForTesting(const std::string& query);

 private:
  class ReaderHost;
  class TabWatcher;
  struct PageWait;

  explicit VoiceController(BrowserWindowInterface* browser);

  void OnBrowserDidClose(BrowserWindowInterface* browser);
  void OnActiveTabChanged(BrowserWindowInterface* browser);

  // The active tab's contents and page controller (null when there is
  // none or voice mode is off).
  content::WebContents* ActiveContents();
  VoicePageController* ActivePage();
  bool IsMoltNet();
  void AttachToActiveTab();
  void DetachFromTab();

  PageMap CurrentMap();
  VoiceEnvironment Environment();

  // Runs |turn|'s steps after what is queued.
  void RunTurn(VoiceTurn turn);
  void RunSteps();
  // Carries out one step; false when it continues asynchronously (the
  // steps resume from its callback) or failed (the rest are dropped).
  bool RunStep(const VoiceStep& step);
  void Fail(StepFailure failure);
  void Say(const std::string& text);
  void Navigate(const GURL& url, bool new_tab);
  GURL DefaultSearchURL(const std::string& query);
  void OnSubmitted(int run, int result);

  // Waiting for a page (kWaitForPage).
  void BeginWait(const VoiceStep& step);
  void RestartQuietTimer();
  void OnQuiet();
  void OnWaitDeadline();
  void FinishWait();
  void OnAutoplayWaitOver();
  void PlanFollowUpNow(const VoiceStep& step);

  // From the active tab (TabWatcher).
  void OnNavigationStarted();
  void OnNavigationCommitted();
  void OnNavigationEnded();
  void OnLoadingStopped();
  void OnTreeChanged();
  void OnMediaStarted();
  void OnTabGone();

  void OnSpeakingChanged(bool speaking);

  // The overlay (voice_overlay.h).
  void ShowNumbersOnPage(bool shown);
  void SetCurrentItemOnPage(int number);
  // "Number 5" while numbers are on screen names the item the user sees
  // as 5. |map| was made just now; a page that renumbered since the
  // overlay drew (a frame changed, within its follow interval) would make
  // it name another item. False, having said so and drawn the new numbers,
  // when the item drawn as |command|'s number isn't that number in |map|.
  bool NumberStillOnScreen(const VoiceCommand& command, const PageMap& map);
  void OnOverlayHidden(int run_of_tab, int reason);
  void RefreshNumbers();
  // Voice mode turned itself off ("turn off voice mode"): once it has said
  // so.
  void MaybeStopWhenQuiet();
  // Cancels the steps still to run, a wait, a submit and the speech, and
  // the planner's run unless |keep_planner|.
  void CancelRun(bool keep_planner = false);

  // activation::PageRequestAsker (the tab's ActivationGuard asks):
  bool AskAboutPageRequest(int id,
                           const activation::Question& question) override;
  void TellAboutPageRequest(const std::string& text) override;
  void PageRequestQuestionWithdrawn(int id) override;
  // Whether |command| answers the activation guard's pending question.
  bool AnswersPageRequestQuestion(const VoiceCommand& command,
                                  const PageMap& map,
                                  const VoiceEnvironment& env) const;
  // Tells the guard |allow| for its question, which no longer waits here.
  void AnswerPageRequest(bool allow);
  // The question went unanswered, or another command dropped it.
  void OnPageRequestTimeout();
  void DropPageRequestQuestionIfGone();

  // VoicePlanner::Delegate:
  void PlannerSays(const std::string& text) override;
  void PlannerAsks(int question, const std::string& what) override;
  void PlannerFinished() override;
  // Ends the planner's run, if one is in progress.
  void CancelPlanner();
  // The planner's question went unanswered.
  void OnPlannerQuestionTimeout();
  // Whether |command| answers the planner's pending question (a yes or no
  // the loop takes as the answer).
  bool AnswersPlannerQuestion(const VoiceCommand& command,
                              const PageMap& map,
                              const VoiceEnvironment& env) const;
  void Idle();
  void NotifyCaption();
  void SetStatus(std::string status);

  raw_ptr<BrowserWindowInterface> browser_;
  ui::ScopedUnownedUserData<VoiceController> scoped_data_holder_;
  base::CallbackListSubscription did_close_subscription_;
  base::CallbackListSubscription active_tab_subscription_;

  bool active_ = false;
  std::unique_ptr<VoiceSpeech> speech_;
  // Reading aloud: the reader and its side in the browser (the host
  // outlives the reader).
  std::unique_ptr<ReaderHost> reader_host_;
  std::unique_ptr<VoiceReader> reader_;
  std::unique_ptr<TabWatcher> watcher_;
  // The tab whose page controller voice mode started.
  base::WeakPtr<content::WebContents> attached_;
  // Changes with every committed navigation of the active tab and every
  // change of the active tab.
  int64_t page_id_ = 1;
  // Committed navigations of the active tab, in all and when the current
  // utterance came, and when the last one committed: a wait that starts
  // after its navigation already committed (a quick submit) knows it.
  int64_t commits_ = 0;
  int64_t commits_at_utterance_ = 0;
  base::TimeTicks last_commit_;

  ConversationState state_;
  std::deque<VoiceStep> pending_;
  // The run the steps belong to: a new utterance or a stop starts another,
  // and callbacks of an earlier run are dropped.
  int run_ = 0;
  bool submitting_ = false;
  bool interrupt_next_speech_ = true;
  std::unique_ptr<PageWait> wait_;
  base::OneShotTimer quiet_timer_;
  base::OneShotTimer deadline_timer_;
  base::OneShotTimer autoplay_timer_;

  std::string heard_;
  std::string status_;
  ListeningCaption listening_;
  bool stop_when_quiet_ = false;
  // Changes with every tab voice mode attaches to: the hidden callback of
  // an earlier tab's overlay is dropped.
  int tab_run_ = 0;
  // Showing the numbers again once a new page's tree has come.
  base::OneShotTimer refresh_numbers_timer_;
  int refresh_attempts_ = 0;
  // The planner (voice_planner.h), its run, and the question it waits on
  // (0 for none).
  std::unique_ptr<VoicePlanner> planner_;
  bool planner_running_ = false;
  int planner_question_ = 0;
  base::OneShotTimer planner_question_timer_;
  // The activation guard's question this controller asks (0 for none), and
  // the tab it is about.
  int page_request_question_ = 0;
  base::WeakPtr<content::WebContents> page_request_tab_;
  base::OneShotTimer page_request_timer_;
  base::ObserverList<Observer> observers_;
  base::RepeatingClosure idle_for_testing_;
  base::WeakPtrFactory<VoiceController> run_weak_factory_{this};
  base::WeakPtrFactory<VoiceController> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CONTROLLER_H_
