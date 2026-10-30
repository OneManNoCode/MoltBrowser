// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's side of the activation guard (activation_policy.h,
// docs/design/TOUCHLESS_BROWSING.md §7.5): one per tab, made when the tab's
// VoicePageController first acts on its page (so never in MoltNet windows,
// where nothing holds that controller).
//
//   - The record: VoicePageController notes every action it sends (an
//     accessibility action, voice mode's Enter key) with NoteAction(); the
//     frames' user activation (FrameReceivedUserActivation()) extends the
//     state as Blink's 5 s start late; the user's own input on the page
//     (DidGetUserInteraction()) ends it, except a scroll or Esc (which give
//     the page no activation of their own) and the key event voice mode
//     itself sends (ExpectOwnKeyDown()).
//   - The hooks: the few upstream places that let user activation unlock
//     something call the static functions below. A tab without a guard, or
//     not acting, is never changed.
//       new tabs and windows   ChromeContentBrowserClient::CanCreateWindow()
//                              and BrowserWebContentsDelegate::
//                              OpenURLFromTab(): the request goes to the
//                              popup blocker as one without a gesture
//                              (GestureForNewWindow(), ParamsForNewTab());
//                              the blocked popup is asked about
//                              (BlockedUrlAdded()) and opened on "yes"
//                              (PopupBlockerTabHelper::ShowBlockedPopup()).
//       downloads              ChromeDownloadManagerDelegate::
//                              CheckDownloadAllowed(): held after the
//                              browser's own checks, until the answer
//                              (GuardDownload()).
//       other apps             ChromeContentBrowserClient::
//                              HandleExternalProtocol(): refused, launched on
//                              "yes" (HoldsExternalProtocol()).
//       full screen            BrowserWebContentsDelegate::
//                              CanEnterFullscreenModeForTab()
//                              (MayEnterFullscreen()): the requesting tab's
//                              guard, and every acting tab whose page can
//                              reach the requesting frame (the same
//                              browsing context group: what postMessage's
//                              capability delegation can hand full screen
//                              to).
//       clipboard writes       ChromeContentBrowserClient::
//                              IsClipboardCopyAllowedByPolicy()
//                              (RefusesClipboardWrite()).
//       a video's picture-     BrowserWebContentsDelegate::
//       in-picture window      EnterPictureInPicture()
//                              (MayEnterPictureInPicture()): refused (a
//                              document's goes through new windows above).
//       permission prompts     PermissionRequestManager's observer
//                              (OnPromptAdded()): left to voice mode when
//                              it reads prompts aloud (it asks, and the
//                              confirmation word allows it); dismissed
//                              and said when it doesn't; never accepted
//                              by the guard.
//       session history        the entries a page adds with an action's
//                              sticky activation (DidFinishNavigation()):
//                              voice mode's "go back" skips them
//                              (VoiceBackOffset()).
//   - The askers: voice mode on the tab (voice_controller.h) and web agent
//     runs on it (agents/web_agent.h) register with SetAsker(). Voice mode
//     asks when it is on the tab (its user hears; an agent run in voice
//     mode's tab included), else the latest agent run (a chip in its run).
//     One question at a time: what comes while one is pending is refused.
//     And one per action (activation::MayAsk()): once one was asked, the
//     page's next requests are refused, and said, until the next action.
//     A question unanswered within kQuestionTimeout, or whose asker goes,
//     counts as "no".

#ifndef CHROME_BROWSER_MOLT_AI_SECURITY_ACTIVATION_GUARD_H_
#define CHROME_BROWSER_MOLT_AI_SECURITY_ACTIVATION_GUARD_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/molt_ai/security/page_request_asker.h"
#include "components/blocked_content/url_list_manager.h"
#include "components/permissions/permission_request_manager.h"
#include "content/public/browser/download_manager_delegate.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai {

class ActivationGuard
    : public content::WebContentsObserver,
      public content::WebContentsUserData<ActivationGuard>,
      public blocked_content::UrlListManager::Observer,
      public permissions::PermissionRequestManager::Observer {
 public:
  // How long a question waits for its answer.
  static constexpr base::TimeDelta kQuestionTimeout = base::Minutes(2);
  // How long the key event voice mode sends may take to come back as input.
  static constexpr base::TimeDelta kOwnKeyWindow = base::Seconds(2);
  // New windows withheld and not yet seen blocked: kept at most, and for
  // how long (the popup blocker sees them in the same task).
  static constexpr size_t kMaxWithheldWindows = 8;
  static constexpr base::TimeDelta kWithheldWindow = base::Seconds(2);

  // Who asks the user (page_request_asker.h).
  using Asker = activation::PageRequestAsker;

  ActivationGuard(const ActivationGuard&) = delete;
  ActivationGuard& operator=(const ActivationGuard&) = delete;
  ~ActivationGuard() override;

  // ---- The record (VoicePageController) ----------------------------------

  // An action was sent to |contents|' page; |intent| is what the user asked
  // it for. Makes the tab's guard.
  static void NoteAction(content::WebContents* contents,
                         activation::Intent intent);
  // The next key-down of |windows_key_code| the tab sees within
  // kOwnKeyWindow is the browser's own (voice mode's Enter), not the
  // user's.
  void ExpectOwnKeyDown(int windows_key_code);

  bool IsActing() const;

  // ---- The askers -----------------------------------------------------------

  // |asker| asks for |who| on this tab (kVoiceMode or kWebAgent), until
  // RemoveAsker(). Makes the tab's guard.
  static void SetAsker(content::WebContents* contents,
                       activation::Asker who,
                       Asker* asker);
  static void RemoveAsker(content::WebContents* contents, Asker* asker);
  // The user's answer to question |id|; stale or unknown ids are ignored.
  void Answer(int id, bool allow);
  // The question being asked, or 0.
  int pending_question() const;

  // ---- The hooks ------------------------------------------------------------

  // CanCreateWindow(): |user_gesture|, or false when the activation that
  // gave it is an action's and the page wasn't asked for a new tab (the
  // popup blocker then holds the window; it is asked about).
  static bool GestureForNewWindow(content::RenderFrameHost* opener,
                                  const GURL& url,
                                  WindowOpenDisposition disposition,
                                  bool user_gesture);
  // OpenURLFromTab(): |params|, with user_gesture false in the same case
  // (a renderer's request for a new tab or window).
  static content::OpenURLParams ParamsForNewTab(
      content::WebContents* source,
      const content::OpenURLParams& params);
  // CheckDownloadAllowed(): |done|, or a callback that, when the browser's
  // own checks allow a download the page started while acting, first asks
  // the user (holding the download) and then runs |done| with the answer.
  static content::CheckDownloadAllowedCallback GuardDownload(
      content::WebContents* contents,
      const GURL& url,
      const std::optional<url::Origin>& initiator,
      content::CheckDownloadAllowedCallback done);
  // HandleExternalProtocol(): true when the launch of |url| the page asked
  // for (|initiating_origin|: none for the browser's own) is held: the
  // caller doesn't launch, and |launch| runs on "yes".
  static bool HoldsExternalProtocol(
      content::WebContents* contents,
      const GURL& url,
      const std::optional<url::Origin>& initiating_origin,
      base::OnceClosure launch);
  // CanEnterFullscreenModeForTab(): false when the page's full screen comes
  // from an action's activation and the user didn't ask for it.
  static bool MayEnterFullscreen(content::RenderFrameHost* requesting_frame);
  // IsClipboardCopyAllowedByPolicy(): true when a page's clipboard write
  // comes from an action's activation and wasn't a copy the user asked for.
  static bool RefusesClipboardWrite(content::WebContents* contents);
  // EnterPictureInPicture(): false when a video's picture-in-picture window
  // comes from an action's activation (no command asks for one).
  static bool MayEnterPictureInPicture(content::WebContents* contents);

  // Voice mode's "go back": the offset (-1, or further back) that skips the
  // entries the page added with an action's sticky activation
  // (activation::NoteHistoryEntry()); -1 when there are none.
  static int VoiceBackOffset(content::WebContents* contents);
  // The most entries remembered as skippable per tab.
  static constexpr size_t kMaxSkippableEntries = 64;
  // The most navigations in flight remembered per tab.
  static constexpr size_t kMaxNavigationsInFlight = 64;

  // For tests: the clock, and the record.
  void SetClockForTesting(base::RepeatingCallback<base::TimeTicks()> clock);
  const activation::TabRecord& record_for_testing() const { return record_; }

 private:
  friend class content::WebContentsUserData<ActivationGuard>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();

  struct Withheld {
    GURL url;
    base::TimeTicks at;
    activation::Decision decision = activation::Decision::kAsk;
  };
  struct Pending {
    Pending();
    Pending(Pending&&);
    Pending& operator=(Pending&&);
    ~Pending();

    int id = 0;
    activation::Request request = activation::Request::kNewWindow;
    raw_ptr<Asker> asker = nullptr;
    // Runs with the answer (false on timeout, the asker going, the tab
    // going).
    base::OnceCallback<void(bool)> resolve;
  };

  explicit ActivationGuard(content::WebContents* contents);

  base::TimeTicks Now() const;
  int64_t NowMs() const;
  // Who asks, and as what.
  activation::Asker CurrentAskerKind() const;
  Asker* CurrentAsker() const;
  activation::Ruling Decide(activation::Request request) const;

  // Asks |question| through the current asker; |resolve| runs with the
  // answer. False (and |resolve| not run) when there is no one to ask or a
  // question is pending.
  bool Ask(const activation::Question& question,
           base::OnceCallback<void(bool)> resolve);
  // Ends the pending question with |allow|; |withdrawn| tells its asker it
  // no longer waits (it timed out, or the page it was about went).
  void Resolve(bool allow, bool withdrawn = false);
  void OnQuestionTimeout();
  // Says what was refused (voice mode), once per kind of request and
  // action.
  void Tell(activation::Request request, const std::string& what);
  void ObserveTabHelpers();
  void StopObservingTabHelpers();

  // Holding a download until the answer (GuardDownload()): the browser's
  // own checks gave |allowed|.
  static void OnDownloadChecked(base::WeakPtr<ActivationGuard> guard,
                                const GURL& url,
                                content::CheckDownloadAllowedCallback done,
                                bool allowed);
  // Dismissing the prompt for |request| (its address, compared only),
  // if it is still the one shown (OnPromptAdded()).
  void RefusePrompt(uintptr_t request);
  // Whether voice mode, the asker here, takes prompts by voice.
  bool PromptsByVoice() const;
  // Notes a session history entry the page added (DidFinishNavigation()).
  void NoteHistoryEntry(content::NavigationHandle* navigation);

  // content::WebContentsObserver:
  void DidGetUserInteraction(const blink::WebInputEvent& event) override;
  void FrameReceivedUserActivation(
      content::RenderFrameHost* render_frame_host) override;
  void DidStartNavigation(content::NavigationHandle* navigation) override;
  void DidFinishNavigation(content::NavigationHandle* navigation) override;
  void WebContentsDestroyed() override;

  // blocked_content::UrlListManager::Observer:
  void BlockedUrlAdded(int32_t id, const GURL& url) override;

  // permissions::PermissionRequestManager::Observer:
  void OnPromptAdded() override;
  void OnPromptRemoved() override;
  void OnPermissionRequestManagerDestructed() override;

  activation::TabRecord record_;
  base::RepeatingCallback<base::TimeTicks()> clock_for_testing_;
  // The kinds of request already said since the last action.
  std::vector<activation::Request> told_;

  raw_ptr<Asker> voice_asker_ = nullptr;
  // Agent runs on this tab, latest last.
  std::vector<raw_ptr<Asker>> agent_askers_;

  std::optional<Pending> pending_;
  int next_question_id_ = 0;
  base::OneShotTimer question_timer_;

  std::vector<Withheld> withheld_windows_;

  // The last committed session history entry, and the entries voice mode's
  // "go back" skips (their unique ids, oldest first).
  int last_entry_id_ = 0;
  std::vector<int> skippable_entries_;
  // When each of the page's navigations in flight began (NowMs(), by
  // navigation id): the page an action led to is the one whose navigation
  // began in the action's window, however late it commits.
  std::map<int64_t, int64_t> navigation_started_ms_;

  int own_key_code_ = 0;
  base::TimeTicks own_key_until_;

  raw_ptr<blocked_content::UrlListManager> popup_list_ = nullptr;
  raw_ptr<permissions::PermissionRequestManager> prompts_ = nullptr;
  // A prompt is on screen (between OnPromptAdded() and OnPromptRemoved()).
  bool prompt_shown_ = false;

  base::WeakPtrFactory<ActivationGuard> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SECURITY_ACTIVATION_GUARD_H_
