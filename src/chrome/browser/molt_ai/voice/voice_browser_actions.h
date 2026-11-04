// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The rest of the browser, carried out for a window's voice mode
// (docs/design/TOUCHLESS_BROWSING.md §2): the kBrowserAction steps
// voice_browser_logic.h plans, through the browser's own commands and
// services wherever they have one.
//
//   - Bookmarks (BookmarkModel: AddIfNotBookmarked(), RemoveAllBookmarks()),
//     the history (HistoryService::QueryHistory()), the downloads
//     (DownloadManager; DownloadItem::OpenDownload(), never for a file that
//     could run code, a dangerous one or one not finished), the recently
//     closed tabs (TabRestoreService; IDC_RESTORE_TAB, or the very tab voice
//     mode closed for "undo"). "Show my history" and the like open the
//     browser's page for it too (chrome::ShowHistory()), and the list is read
//     to choose from.
//   - Settings (chrome::ShowSettings(), ShowSettingsSubPage(), MoltBrowser's
//     AI settings page and its sections), always listening
//     (voice_session_settings.h, on settings.json's one writer).
//   - Windows: IDC_NEW_WINDOW, IDC_NEW_INCOGNITO_WINDOW, a MoltNet window
//     (MoltNetWindow::Open(); voice mode stays where it is), closing this
//     window (voice mode moves to another of the profile's windows first),
//     and switching to another window (voice mode comes along; never to a
//     MoltNet window).
//   - The AI chat beside the page (SidePanelUI), and a question for the
//     on-device AI (voice_ai_answers.h), read as model output.
//   - The browser's own buttons and menus (voice_browser_ui.h): the app
//     menu (IDC_SHOW_APP_MENU), and a press by the view's accessibility id.
//   - The page's media element (voice_media_controls.h): its speed, volume
//     and captions, by a fixed script in the media's frame's isolated
//     world.
//
// Voice mode is off in MoltNet windows, so none of this runs there. Nothing
// is stored: the lists live in the voice loop's memory. UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_ACTIONS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_ACTIONS_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/voice/voice_browser_types.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_media_controls.h"
#include "components/sessions/core/session_id.h"

class BrowserWindowInterface;
class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

class VoiceAiAnswers;
class VoicePageController;

class VoiceBrowserActions {
 public:
  // What voice mode's listening (voice_session.h, which builds on this
  // file's target) does for these actions; installed by
  // voice_window_listening.h when a window gets its Voice button. Without
  // it, voice mode moves between windows without the microphone, and
  // always listening can't be changed by voice.
  struct WindowListening {
    WindowListening();
    ~WindowListening();
    WindowListening(const WindowListening&);
    WindowListening& operator=(const WindowListening&);

    // Whether |browser|'s window listens to the microphone now.
    base::RepeatingCallback<bool(BrowserWindowInterface*)> listening;
    // Starts listening in |browser|'s window (one window listens at a
    // time: the one that did stops).
    base::RepeatingCallback<void(BrowserWindowInterface*)> start;
    // Reads always listening from settings.
    base::RepeatingCallback<void(base::OnceCallback<void(bool)>)>
        read_always_listening;
    // Turns always listening on or off in settings, and every window's
    // listening follows; |done| hears whether it was saved.
    base::RepeatingCallback<void(bool, base::OnceCallback<void(bool)>)>
        set_always_listening;
  };
  static void SetWindowListening(WindowListening listening);

  class Delegate {
   public:
    // Says |text|, the browser's own words, already in the window's
    // language.
    virtual void BrowserActionSays(const std::string& text) = 0;
    // Reads |text|, model output, as page text is read.
    virtual void BrowserActionReads(const std::string& text) = 0;
    // A step of run |run| that went on asynchronously is done: the steps
    // after it go on.
    virtual void BrowserActionDone(int run) = 0;
    // The list a step of run |run| asked for: planned with
    // PlanBrowserItems().
    virtual void BrowserItemsReady(int run,
                                   const VoiceStep& asked,
                                   std::vector<BrowserItem> items) = 0;
    // The active tab and its page controller (null when there is none or
    // voice mode is off), and the window's language.
    virtual content::WebContents* BrowserActionContents() = 0;
    virtual VoicePageController* BrowserActionPage() = 0;
    virtual Language BrowserActionLanguage() = 0;

   protected:
    virtual ~Delegate() = default;
  };

  // How a step went.
  enum class Outcome {
    kDone,     // the steps after it go on now
    kPending,  // the delegate hears when it is done
    kFailed,   // |failure|: the steps after it are dropped
  };
  struct Result {
    Outcome outcome = Outcome::kDone;
    StepFailure failure = StepFailure::kUnavailable;
  };

  VoiceBrowserActions(BrowserWindowInterface* browser, Delegate* delegate);
  VoiceBrowserActions(const VoiceBrowserActions&) = delete;
  VoiceBrowserActions& operator=(const VoiceBrowserActions&) = delete;
  ~VoiceBrowserActions();

  // Carries out |step| (a kBrowserAction step) of run |run|.
  Result Run(const VoiceStep& step, int run);

  // What the window can do now (VoiceEnvironment::browser).
  BrowserEnvironment Environment();

  // Voice mode just closed a tab (kCloseTab): "undo" reopens that one.
  void NoteTabClosed();

  // A new utterance or "stop": what runs is dropped (its answer never
  // comes), the AI's question ends.
  void Cancel();

  // Something runs asynchronously.
  bool busy() const;

 private:
  Profile* GetProfile();
  void Finish(int run);

  // The page's media element.
  Result RunMedia(const VoiceStep& step, int run);
  void OnMediaQueried(int run, VoiceStep step, base::Value result);
  void OnMediaDone(int run, MediaOp op, int asked, base::Value result);
  bool RunMediaScript(const VoiceStep& step,
                      MediaOp op,
                      int value,
                      base::OnceCallback<void(base::Value)> done);

  // The browser's lists.
  void ListBookmarks(const VoiceStep& step, int run);
  void ListHistory(const VoiceStep& step, int run);
  void QueryHistory(int run, VoiceStep step, bool recent);
  void OnHistory(int run,
                 VoiceStep step,
                 bool recent,
                 std::vector<BrowserItem> items);
  void ListDownloads(const VoiceStep& step, int run);
  void ListBrowserUi(const VoiceStep& step, int run);
  void Deliver(int run, VoiceStep step, std::vector<BrowserItem> items);

  Result OpenUrl(const VoiceStep& step);
  Result OpenDownload(const VoiceStep& step);
  Result ReopenTab(const VoiceStep& step);
  Result Bookmark(const VoiceStep& step, bool add);
  Result OpenSettings(const VoiceStep& step);
  void SetAlwaysListening(bool on);
  Result NewWindow(const VoiceStep& step);
  Result CloseWindow();
  Result SwitchWindow();
  Result AskAI(const VoiceStep& step, int run);
  Result PressBrowserUi(const VoiceStep& step);

  // The profile's other windows voice mode can move to: normal tabbed
  // windows of the same person's profile, not MoltNet's, not closing.
  std::vector<BrowserWindowInterface*> OtherWindows();
  // Voice mode, listening or not as it is here, in |other| now.
  void MoveVoiceModeTo(BrowserWindowInterface* other);

  raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<Delegate> delegate_;
  // The run the asynchronous step belongs to (0: none).
  int pending_run_ = 0;
  // The tab voice mode closed last, in the recently closed tabs.
  std::optional<SessionID> closed_by_voice_;
  // Always listening, as settings last said.
  bool always_listening_ = false;
  std::unique_ptr<VoiceAiAnswers> ai_answers_;
  base::CancelableTaskTracker history_tracker_;
  base::WeakPtrFactory<VoiceBrowserActions> run_weak_factory_{this};
  base::WeakPtrFactory<VoiceBrowserActions> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_ACTIONS_H_
