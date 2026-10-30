// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/security/activation_guard.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "components/blocked_content/popup_blocker.h"
#include "components/blocked_content/popup_blocker_tab_helper.h"
#include "components/permissions/permission_request.h"
#include "components/permissions/request_type.h"
#include "components/permissions/resolvers/permission_prompt_options.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/frame/user_activation_state.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "ui/events/keycodes/keyboard_codes.h"

namespace molt_ai {

namespace {

using activation::Decision;
using activation::Request;
using activation::Ruling;

// The browser's word for what a prompt asks ("camera" permission); empty
// for the rest ("a permission").
std::string PermissionWord(permissions::RequestType type) {
  switch (type) {
    case permissions::RequestType::kCameraStream:
      return "camera";
    case permissions::RequestType::kMicStream:
      return "microphone";
    case permissions::RequestType::kGeolocation:
      return "location";
    case permissions::RequestType::kNotifications:
      return "notifications";
    case permissions::RequestType::kClipboard:
      return "clipboard";
    case permissions::RequestType::kMidiSysex:
      return "MIDI device";
    case permissions::RequestType::kStorageAccess:
    case permissions::RequestType::kTopLevelStorageAccess:
      return "cookies";
    case permissions::RequestType::kMultipleDownloads:
      return "downloads";
    case permissions::RequestType::kLocalNetwork:
    case permissions::RequestType::kLoopbackNetwork:
      return "local network";
    case permissions::RequestType::kSensors:
      return "motion sensors";
    case permissions::RequestType::kWindowManagement:
      return "window management";
    case permissions::RequestType::kFileSystemAccess:
      return "file editing";
    case permissions::RequestType::kIdleDetection:
      return "idle detection";
    default:
      return std::string();
  }
}

// Runs the download's callback later, as the browser's own refusals do
// (ChromeDownloadManagerDelegate::CheckDownloadAllowed()).
void PostDownloadAnswer(content::CheckDownloadAllowedCallback done,
                        bool allow) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(done), allow));
}

}  // namespace

WEB_CONTENTS_USER_DATA_KEY_IMPL(ActivationGuard);

ActivationGuard::Pending::Pending() = default;
ActivationGuard::Pending::Pending(Pending&&) = default;
ActivationGuard::Pending& ActivationGuard::Pending::operator=(Pending&&) =
    default;
ActivationGuard::Pending::~Pending() = default;

ActivationGuard::ActivationGuard(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<ActivationGuard>(*contents) {
  // Blink's lifespan of transient activation, and the browser's (the same
  // constant), plus the requests still on their way.
  record_.window_ms = blink::kActivationLifespan.InMilliseconds() +
                      activation::kInFlightMs;
  ObserveTabHelpers();
}

ActivationGuard::~ActivationGuard() {
  StopObservingTabHelpers();
  // A held download is answered: nothing is left waiting on this tab.
  Resolve(false);
}

// ---- The record --------------------------------------------------------------

// static
void ActivationGuard::NoteAction(content::WebContents* contents,
                                 activation::Intent intent) {
  if (!contents) {
    return;
  }
  ActivationGuard::CreateForWebContents(contents);
  ActivationGuard* guard = ActivationGuard::FromWebContents(contents);
  activation::NoteAction(guard->record_, guard->NowMs(), intent);
  guard->told_.clear();
}

void ActivationGuard::ExpectOwnKeyDown(int windows_key_code) {
  own_key_code_ = windows_key_code;
  own_key_until_ = Now() + kOwnKeyWindow;
}

bool ActivationGuard::IsActing() const {
  return activation::IsActing(record_, NowMs());
}

void ActivationGuard::SetClockForTesting(
    base::RepeatingCallback<base::TimeTicks()> clock) {
  clock_for_testing_ = std::move(clock);
}

base::TimeTicks ActivationGuard::Now() const {
  return clock_for_testing_ ? clock_for_testing_.Run() : base::TimeTicks::Now();
}

int64_t ActivationGuard::NowMs() const {
  return (Now() - base::TimeTicks()).InMilliseconds();
}

void ActivationGuard::DidGetUserInteraction(const blink::WebInputEvent& event) {
  // Only input that gives the page activation of its own ends the state
  // (HTML's activation-triggering events: a key other than Esc, a mouse
  // button, a touch). A scroll or Esc gives none, so the page would still
  // be using the action's.
  if (event.GetType() == blink::WebInputEvent::Type::kGestureScrollBegin) {
    return;
  }
  if (event.GetType() == blink::WebInputEvent::Type::kRawKeyDown) {
    const int key =
        static_cast<const blink::WebKeyboardEvent&>(event).windows_key_code;
    if (key == ui::VKEY_ESCAPE) {
      return;
    }
    // The Enter voice mode sent (VoicePageController::Submit()) comes back
    // here as the tab's input: it is the action's own, not the user's.
    if (own_key_code_ != 0 && key == own_key_code_ && Now() < own_key_until_) {
      own_key_code_ = 0;
      return;
    }
  }
  activation::NoteUserInput(record_);
}

void ActivationGuard::FrameReceivedUserActivation(
    content::RenderFrameHost* render_frame_host) {
  activation::NoteActivation(record_, NowMs());
}

void ActivationGuard::DidFinishNavigation(
    content::NavigationHandle* navigation) {
  if (!navigation->IsInPrimaryMainFrame() || !navigation->HasCommitted() ||
      navigation->IsSameDocument()) {
    return;
  }
  // The popup blocker forgets the page's blocked windows
  // (PopupBlockerTabHelper::DidFinishNavigation()): there is nothing left
  // to open.
  withheld_windows_.clear();
  if (pending_ && pending_->request == Request::kNewWindow) {
    Resolve(false, /*withdrawn=*/true);
  }
}

void ActivationGuard::WebContentsDestroyed() {
  StopObservingTabHelpers();
  Resolve(false);
}

// ---- The askers -------------------------------------------------------------

// static
void ActivationGuard::SetAsker(content::WebContents* contents,
                               activation::Asker who,
                               Asker* asker) {
  if (!contents || !asker) {
    return;
  }
  ActivationGuard::CreateForWebContents(contents);
  ActivationGuard* guard = ActivationGuard::FromWebContents(contents);
  switch (who) {
    case activation::Asker::kVoiceMode:
      guard->voice_asker_ = asker;
      return;
    case activation::Asker::kWebAgent:
      std::erase(guard->agent_askers_, asker);
      guard->agent_askers_.push_back(asker);
      return;
    case activation::Asker::kNone:
      return;
  }
}

// static
void ActivationGuard::RemoveAsker(content::WebContents* contents,
                                  Asker* asker) {
  ActivationGuard* guard =
      contents ? ActivationGuard::FromWebContents(contents) : nullptr;
  if (!guard || !asker) {
    return;
  }
  if (guard->voice_asker_ == asker) {
    guard->voice_asker_ = nullptr;
  }
  std::erase(guard->agent_askers_, asker);
  if (guard->pending_ && guard->pending_->asker == asker) {
    guard->Resolve(false);
  }
}

activation::Asker ActivationGuard::CurrentAskerKind() const {
  if (voice_asker_) {
    return activation::Asker::kVoiceMode;
  }
  return agent_askers_.empty() ? activation::Asker::kNone
                               : activation::Asker::kWebAgent;
}

ActivationGuard::Asker* ActivationGuard::CurrentAsker() const {
  if (voice_asker_) {
    return voice_asker_;
  }
  return agent_askers_.empty() ? nullptr : agent_askers_.back().get();
}

Ruling ActivationGuard::Decide(Request request) const {
  return activation::Decide(record_, NowMs(), request, CurrentAskerKind());
}

bool ActivationGuard::Ask(const activation::Question& question,
                          base::OnceCallback<void(bool)> resolve) {
  Asker* asker = CurrentAsker();
  if (!asker || pending_) {
    // One question at a time; with no one to ask, it is a "no".
    std::move(resolve).Run(false);
    return false;
  }
  const int id = ++next_question_id_;
  pending_.emplace();
  pending_->id = id;
  pending_->request = question.request;
  pending_->asker = asker;
  pending_->resolve = std::move(resolve);
  question_timer_.Start(FROM_HERE, kQuestionTimeout,
                        base::BindOnce(&ActivationGuard::OnQuestionTimeout,
                                       weak_factory_.GetWeakPtr()));
  if (!asker->AskAboutPageRequest(id, question)) {
    if (pending_ && pending_->id == id) {
      Resolve(false);
    }
    return false;
  }
  return true;
}

void ActivationGuard::Answer(int id, bool allow) {
  if (pending_ && id != 0 && pending_->id == id) {
    Resolve(allow);
  }
}

int ActivationGuard::pending_question() const {
  return pending_ ? pending_->id : 0;
}

void ActivationGuard::Resolve(bool allow, bool withdrawn) {
  question_timer_.Stop();
  if (!pending_) {
    return;
  }
  const int id = pending_->id;
  Asker* asker = pending_->asker;
  base::OnceCallback<void(bool)> resolve = std::move(pending_->resolve);
  pending_.reset();
  base::WeakPtr<ActivationGuard> self = weak_factory_.GetWeakPtr();
  if (resolve) {
    std::move(resolve).Run(allow);
  }
  // Askers stay registered until they RemoveAsker() themselves.
  if (self && withdrawn && asker &&
      (asker == voice_asker_ ||
       std::find(agent_askers_.begin(), agent_askers_.end(), asker) !=
           agent_askers_.end())) {
    asker->PageRequestQuestionWithdrawn(id);
  }
}

void ActivationGuard::OnQuestionTimeout() {
  Resolve(false, /*withdrawn=*/true);
}

void ActivationGuard::Tell(Request request, const std::string& what) {
  // Once per kind of request and action: a page that keeps trying isn't
  // read out each time.
  if (std::find(told_.begin(), told_.end(), request) != told_.end()) {
    return;
  }
  Asker* asker = CurrentAsker();
  if (!asker) {
    return;
  }
  told_.push_back(request);
  asker->TellAboutPageRequest(activation::RefusalNotice(request, what));
}

// ---- New tabs and windows ---------------------------------------------------

// static
bool ActivationGuard::GestureForNewWindow(content::RenderFrameHost* opener,
                                          const GURL& url,
                                          WindowOpenDisposition disposition,
                                          bool user_gesture) {
  if (!user_gesture || !opener ||
      !blocked_content::ConsiderForPopupBlocking(disposition)) {
    return user_gesture;
  }
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(opener);
  ActivationGuard* guard =
      contents ? ActivationGuard::FromWebContents(contents) : nullptr;
  if (!guard) {
    return user_gesture;
  }
  const Ruling ruling = guard->Decide(Request::kNewWindow);
  if (ruling.decision == Decision::kProceed) {
    return user_gesture;
  }
  // Without the gesture, the popup blocker holds the window (unless the
  // user lets this site open pop-ups at any time); BlockedUrlAdded() asks.
  if (guard->withheld_windows_.size() >= kMaxWithheldWindows) {
    guard->withheld_windows_.erase(guard->withheld_windows_.begin());
  }
  guard->withheld_windows_.push_back(
      Withheld{url, guard->Now(), ruling.decision});
  return false;
}

// static
content::OpenURLParams ActivationGuard::ParamsForNewTab(
    content::WebContents* source,
    const content::OpenURLParams& params) {
  if (!source || !params.is_renderer_initiated) {
    return params;
  }
  content::RenderFrameHost* opener = content::RenderFrameHost::FromID(
      params.source_render_process_id, params.source_render_frame_id);
  if (!opener || content::WebContents::FromRenderFrameHost(opener) != source) {
    opener = source->GetPrimaryMainFrame();
  }
  content::OpenURLParams guarded = params;
  guarded.user_gesture = GestureForNewWindow(
      opener, params.url, params.disposition, params.user_gesture);
  return guarded;
}

void ActivationGuard::BlockedUrlAdded(int32_t id, const GURL& url) {
  // Only the windows this guard withheld (a page's own no-gesture popups
  // are the popup blocker's, as always), and only just withheld.
  const base::TimeTicks now = Now();
  auto it = std::find_if(withheld_windows_.begin(), withheld_windows_.end(),
                         [&](const Withheld& w) {
                           return w.url == url &&
                                  now - w.at < kWithheldWindow;
                         });
  if (it == withheld_windows_.end()) {
    return;
  }
  const Decision decision = it->decision;
  withheld_windows_.erase(it);
  if (decision != Decision::kAsk) {
    return;
  }
  Ask(activation::QuestionFor(Request::kNewWindow, url.host(), url.scheme()),
      base::BindOnce(
          [](base::WeakPtr<ActivationGuard> guard, int32_t popup_id,
             bool allow) {
            if (!allow || !guard || !guard->web_contents()) {
              return;  // it stays in the blocked pop-ups' list
            }
            if (auto* popups =
                    blocked_content::PopupBlockerTabHelper::FromWebContents(
                        guard->web_contents())) {
              popups->ShowBlockedPopup(
                  popup_id, WindowOpenDisposition::NEW_FOREGROUND_TAB);
            }
          },
          weak_factory_.GetWeakPtr(), id));
}

// ---- Downloads --------------------------------------------------------------

// static
content::CheckDownloadAllowedCallback ActivationGuard::GuardDownload(
    content::WebContents* contents,
    const GURL& url,
    const std::optional<url::Origin>& initiator,
    content::CheckDownloadAllowedCallback done) {
  ActivationGuard* guard =
      contents ? ActivationGuard::FromWebContents(contents) : nullptr;
  // The browser's own downloads (an address typed or said) aren't the
  // page's.
  if (!guard || !initiator) {
    return done;
  }
  switch (guard->Decide(Request::kDownload).decision) {
    case Decision::kProceed:
      return done;
    case Decision::kRefuse:
      return base::BindOnce(
          [](content::CheckDownloadAllowedCallback done, bool) {
            std::move(done).Run(false);
          },
          std::move(done));
    case Decision::kAsk:
      break;
  }
  // Decided now, asked once the browser's own checks have allowed it.
  return base::BindOnce(&ActivationGuard::OnDownloadChecked,
                        guard->weak_factory_.GetWeakPtr(), url,
                        std::move(done));
}

// static
void ActivationGuard::OnDownloadChecked(
    base::WeakPtr<ActivationGuard> guard,
    const GURL& url,
    content::CheckDownloadAllowedCallback done,
    bool allowed) {
  if (!allowed || !guard) {
    // The browser refused it, or the tab went while it checked.
    std::move(done).Run(false);
    return;
  }
  // Held until the answer; a "no", the timeout or the tab going cancels it.
  guard->Ask(
      activation::QuestionFor(Request::kDownload, url.host(), url.scheme()),
      base::BindOnce(&PostDownloadAnswer, std::move(done)));
}

// ---- Other apps -------------------------------------------------------------

// static
bool ActivationGuard::HoldsExternalProtocol(
    content::WebContents* contents,
    const GURL& url,
    const std::optional<url::Origin>& initiating_origin,
    base::OnceClosure launch) {
  ActivationGuard* guard =
      contents ? ActivationGuard::FromWebContents(contents) : nullptr;
  if (!guard || !initiating_origin) {
    return false;
  }
  switch (guard->Decide(Request::kExternalApp).decision) {
    case Decision::kProceed:
      return false;
    case Decision::kRefuse:
      return true;
    case Decision::kAsk:
      break;
  }
  guard->Ask(
      activation::QuestionFor(Request::kExternalApp, initiating_origin->host(),
                              url.scheme()),
      base::BindOnce(
          [](base::OnceClosure launch, bool allow) {
            if (allow) {
              std::move(launch).Run();
            }
          },
          std::move(launch)));
  return true;
}

// ---- Full screen and the clipboard ------------------------------------------

// static
bool ActivationGuard::MayEnterFullscreen(
    content::RenderFrameHost* requesting_frame) {
  content::WebContents* contents =
      requesting_frame
          ? content::WebContents::FromRenderFrameHost(requesting_frame)
          : nullptr;
  ActivationGuard* guard =
      contents ? ActivationGuard::FromWebContents(contents) : nullptr;
  if (!guard) {
    return true;
  }
  const Ruling ruling = guard->Decide(Request::kFullScreen);
  if (ruling.decision == Decision::kProceed) {
    return true;
  }
  if (ruling.tell) {
    guard->Tell(Request::kFullScreen, std::string());
  }
  return false;
}

// static
bool ActivationGuard::RefusesClipboardWrite(content::WebContents* contents) {
  ActivationGuard* guard =
      contents ? ActivationGuard::FromWebContents(contents) : nullptr;
  if (!guard) {
    return false;
  }
  const Ruling ruling = guard->Decide(Request::kClipboardWrite);
  if (ruling.decision == Decision::kProceed) {
    return false;
  }
  if (ruling.tell) {
    guard->Tell(Request::kClipboardWrite, std::string());
  }
  return true;
}

// ---- Permission prompts -----------------------------------------------------

void ActivationGuard::ObserveTabHelpers() {
  if (auto* popups =
          blocked_content::PopupBlockerTabHelper::FromWebContents(
              web_contents())) {
    popup_list_ = popups->manager();
    popup_list_->AddObserver(this);
  }
  if (auto* prompts =
          permissions::PermissionRequestManager::FromWebContents(
              web_contents())) {
    prompts_ = prompts;
    prompts_->AddObserver(this);
  }
}

void ActivationGuard::StopObservingTabHelpers() {
  if (popup_list_) {
    popup_list_->RemoveObserver(this);
    popup_list_ = nullptr;
  }
  if (prompts_) {
    prompts_->RemoveObserver(this);
    prompts_ = nullptr;
  }
}

void ActivationGuard::OnPromptAdded() {
  prompt_shown_ = true;
  if (!prompts_ || prompts_->Requests().empty()) {
    return;
  }
  // Never accepted for the user. In voice mode, whose user may not see the
  // prompt and can't answer it by voice yet, it is dismissed (not denied:
  // nothing is remembered for the site) and said. Not from inside the
  // manager's own notification.
  if (Decide(Request::kPermissionPrompt).decision != Decision::kRefuse) {
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&ActivationGuard::RefusePrompt,
                     weak_factory_.GetWeakPtr(),
                     reinterpret_cast<uintptr_t>(
                         prompts_->Requests().front().get())));
}

void ActivationGuard::OnPromptRemoved() {
  prompt_shown_ = false;
}

void ActivationGuard::OnPermissionRequestManagerDestructed() {
  if (prompts_) {
    prompts_->RemoveObserver(this);
    prompts_ = nullptr;
  }
  prompt_shown_ = false;
}

void ActivationGuard::RefusePrompt(uintptr_t request) {
  // Still the prompt that was shown, and still on screen.
  if (!prompts_ || !prompt_shown_ || prompts_->Requests().empty() ||
      reinterpret_cast<uintptr_t>(prompts_->Requests().front().get()) !=
          request) {
    return;
  }
  const std::string what =
      PermissionWord(prompts_->Requests().front()->request_type());
  prompts_->Dismiss(PromptOptions(std::monostate()));
  Tell(Request::kPermissionPrompt, what);
}

}  // namespace molt_ai
