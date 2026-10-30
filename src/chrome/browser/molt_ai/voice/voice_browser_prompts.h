// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's prompts and a page's dialogs answered by voice, in a window
// whose voice mode listens (docs/design/TOUCHLESS_BROWSING.md §7.3,
// Phase 3). One per VoiceController, following the tab it is attached to:
//
//   - Permission prompts (PermissionRequestManager's observer): read, and
//     allowed by the voice gesture's word (voice_gesture.h): for this
//     visit when Chromium has temporary grants for what is asked, else as
//     the prompt's "Allow"; "no" blocks, "cancel" dismisses; another word
//     of the list, or no answer within kPromptWindowMs, dismisses it, and
//     that is said.
//   - A page's dialogs (voice_dialog_manager.h): alert(), confirm(),
//     prompt() and beforeunload, read with the site they come from and
//     answered as voice_prompts.h says; past kMaxDialogsRead in
//     kDialogFloodWindowMs, their text isn't read.
//   - A dangerous download from the tab (the profile's downloads): read;
//     kept with the word as the download bubble's warning keeps it,
//     deleted on "discard"; a file known to be malicious is only deleted by
//     voice.
//   - The password manager's save bubble: read when its page action (the
//     key icon) changes on the tab, whenever that is, and once a page of
//     the tab has loaded. "save", "never" or "not now". Nothing of the
//     password, nor the user name, is said.
//   - A workflow draft's sites (ConfirmDraftSites()): confirmed with the
//     word.
//
// One prompt is read at a time: a newer one takes the place of the one
// waiting (whose own window or dialog stays, for the mouse and keyboard).
//
// The voice gesture: GestureBook mints it only from an utterance the
// listening session heard cleanly (HeardEvidence, voice_session_logic.h),
// for the prompt it was asked for; it is used here at once, for that
// prompt's requests only (checked again as they are then), and never
// outlives kGestureLifetimeMs. Typed text never mints one. A page can't
// reach any of this: it only ever sees the prompt answered.
//
// MoltNet windows: voice mode is off there, so nothing is read (the
// delegate says the window doesn't listen).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_PROMPTS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_PROMPTS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_dialog_manager.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "chrome/browser/molt_ai/voice/voice_prompts.h"
#include "chrome/browser/ui/page_action/page_action_model.h"
#include "chrome/browser/ui/page_action/page_action_model_observer.h"
#include "components/download/content/public/all_download_item_notifier.h"
#include "components/permissions/permission_request_manager.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

class VoiceBrowserPrompts
    : public content::WebContentsObserver,
      public permissions::PermissionRequestManager::Observer,
      public download::AllDownloadItemNotifier::Observer,
      public VoiceDialogManager::Listener,
      public page_actions::PageActionModelObserver {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // Voice mode is on and listens to the microphone in this window (not
    // only typed text, not a MoltNet window): prompts are read.
    virtual bool ListensForPrompts() = 0;
    // Reads |prompt| as prompt |id| (PlanBrowserPrompt()), |word| the
    // voice gesture's word or empty. False if it can't now.
    virtual bool ReadBrowserPrompt(int id,
                                   const BrowserPrompt& prompt,
                                   const std::string& word) = 0;
    // Prompt |id| no longer waits; |said| (the browser's words) is said
    // if not empty (PlanBrowserPromptGone()).
    virtual void EndBrowserPrompt(int id, const std::string& said) = 0;
    // Says |text| while the prompt keeps waiting.
    virtual void SayAboutBrowserPrompt(const std::string& text) = 0;
  };

  explicit VoiceBrowserPrompts(Delegate* delegate);
  VoiceBrowserPrompts(const VoiceBrowserPrompts&) = delete;
  VoiceBrowserPrompts& operator=(const VoiceBrowserPrompts&) = delete;
  ~VoiceBrowserPrompts() override;

  // The tab voice mode is attached to (one at a time), or none. Detaching
  // forgets the prompt read there without telling the delegate.
  void Attach(content::WebContents* contents);
  void Detach();

  // A kPromptAnswer step for prompt |step.index|; |evidence| is how its
  // utterance was heard.
  void Answer(const VoiceStep& step, const HeardEvidence& evidence);

  // Whether prompts on the attached tab are read and answered by voice
  // now (the activation guard leaves prompts raised while acting to it).
  bool TakesPromptsByVoice();

  // The learning workstream's "confirm these sites" seam: asks the user to
  // confirm workflow draft |workflow_id|'s |sites| (hosts) by voice; |done|
  // runs once, with true only when the word was heard cleanly and the
  // gesture was used for exactly those sites, in that draft
  // (DraftSitesBinding()). The caller then confirms them as the Agent
  // studio's click does (AutomationSecurity::ConfirmDraftSites()) and saves.
  // False (and |done| not run) when prompts aren't read now.
  bool ConfirmDraftSites(const std::string& workflow_id,
                         const std::vector<std::string>& sites,
                         base::OnceCallback<void(bool)> done);

  // For tests.
  void SetClockForTesting(base::RepeatingCallback<base::TimeTicks()> clock);
  void SetRandomForTesting(base::RepeatingCallback<uint64_t()> random);
  int current_prompt_for_testing() const { return current_ ? current_->id : 0; }
  std::string current_word_for_testing() const;
  bool gesture_locked_out_for_testing();

 private:
  enum class Source : uint8_t {
    kPermission,
    kDialog,
    kDownload,
    kPassword,
    kSites,
  };

  struct Current {
    Current();
    ~Current();

    int id = 0;
    Source source = Source::kDialog;
    PromptKind kind = PromptKind::kAlert;
    // The voice gesture's prompt and what it allows, when allowing needs
    // one.
    int gesture_id = 0;
    std::string word;
    GestureBinding binding;
    // kPermission: the requests asked about (the first one's address,
    // compared only).
    uintptr_t request = 0;
    // kDownload: the download.
    uint32_t download_id = 0;
    // kDialog prompt(): the field's default.
    std::u16string default_text;
    // kSites.
    base::OnceCallback<void(bool)> done;
    base::OneShotTimer timer;
  };

  int64_t NowMs() const;
  // Reads |prompt| as the current one, from |source|; |binding| when
  // allowing needs the gesture. Returns its id, 0 if it isn't read.
  int Begin(Source source,
            BrowserPrompt prompt,
            std::optional<GestureBinding> binding);
  // |prompt| (taken out of current_) no longer waits: |said| is said.
  void Finish(std::unique_ptr<Current> prompt, const std::string& said);
  // The current prompt went by other means (closed, answered by a click):
  // nothing is said.
  void Drop();
  void OnTimeout(int id);

  // Carry out |answer| to |prompt|; what to say, if anything.
  std::string AnswerPermission(const Current& prompt,
                               PromptAnswer answer,
                               const VoiceGesture* gesture);
  std::string AnswerDownload(const Current& prompt,
                             PromptAnswer answer,
                             const VoiceGesture* gesture);
  void AnswerPassword(PromptAnswer answer);
  void AnswerDialog(const Current& prompt,
                    PromptAnswer answer,
                    const std::string& text);

  void ReadPermissionPrompt();
  std::optional<GestureBinding> PermissionBinding();
  void MaybeReadDownload(download::DownloadItem* item);
  // Reads the save bubble if it is pending and wasn't read yet.
  void CheckPassword();
  // Watches the tab's password page action (ManagePasswordsUIController
  // updates it whenever its state changes; 156 has no observer of the
  // controller itself).
  void ObservePasswordAction(content::WebContents* contents);

  // The word (kAllow or kWrongWord) to the current prompt.
  void AnswerWithWord(const VoiceStep& step, const HeardEvidence& evidence);

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void DidStopLoading() override;
  void WebContentsDestroyed() override;

  // permissions::PermissionRequestManager::Observer:
  void OnPromptAdded() override;
  void OnPromptRemoved() override;
  void OnPermissionRequestManagerDestructed() override;

  // download::AllDownloadItemNotifier::Observer:
  void OnDownloadUpdated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;

  // VoiceDialogManager::Listener:
  void OnDialogShown(content::WebContents* contents,
                     const VoiceDialogManager::Dialog& dialog) override;
  void OnDialogClosed(content::WebContents* contents) override;

  // page_actions::PageActionModelObserver:
  void OnPageActionModelChanged(
      const page_actions::PageActionModelInterface& model) override;
  void OnPageActionModelWillBeDeleted(
      const page_actions::PageActionModelInterface& model) override;

  raw_ptr<Delegate> delegate_;
  GestureBook gestures_;
  base::RepeatingCallback<base::TimeTicks()> clock_for_testing_;
  base::RepeatingCallback<uint64_t()> random_for_testing_;

  raw_ptr<permissions::PermissionRequestManager> permissions_ = nullptr;
  std::unique_ptr<download::AllDownloadItemNotifier> downloads_;
  // Downloads already read out.
  std::set<uint32_t> downloads_read_;
  DialogReadLimiter dialogs_read_;
  base::ScopedObservation<page_actions::PageActionModelInterface,
                          page_actions::PageActionModelObserver>
      password_action_{this};
  // The save bubble pending was already read.
  bool password_read_ = false;

  std::unique_ptr<Current> current_;
  int next_id_ = 0;

  base::WeakPtrFactory<VoiceBrowserPrompts> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_BROWSER_PROMPTS_H_
