// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_browser_prompts.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/download/bubble/download_bubble_ui_controller.h"
#include "chrome/browser/download/download_commands.h"
#include "chrome/browser/download/download_item_model.h"
#include "chrome/browser/download/download_item_warning_data.h"
#include "chrome/browser/download/download_ui_model.h"
#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/ui/passwords/manage_passwords_ui_controller.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_item.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/common/password_manager_ui.h"
#include "components/permissions/permission_request.h"
#include "components/permissions/permission_util.h"
#include "components/permissions/request_type.h"
#include "components/permissions/resolvers/permission_prompt_options.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai {
namespace voice {

namespace {

// The binding for keeping download |item|.
GestureBinding KeepBinding(download::DownloadItem* item) {
  return GestureBinding{
      GestureAction::kKeepDangerousDownload,
      base::NumberToString(item->GetId()) + " " + item->GetURL().spec()};
}

// A download that is still being scanned, or that the bubble offers to scan
// rather than to keep or delete: left to the bubble, not read.
bool IsScanning(download::DownloadDangerType danger) {
  switch (danger) {
    case download::DOWNLOAD_DANGER_TYPE_PROMPT_FOR_SCANNING:
    case download::DOWNLOAD_DANGER_TYPE_PROMPT_FOR_LOCAL_PASSWORD_SCANNING:
    case download::DOWNLOAD_DANGER_TYPE_ASYNC_SCANNING:
    case download::DOWNLOAD_DANGER_TYPE_ASYNC_LOCAL_PASSWORD_SCANNING:
      return true;
    default:
      return false;
  }
}

// What may be kept by voice: what the bubble shows in yellow and keeps with
// one click on its warning ("Download suspicious file", "Download
// unverified file": DownloadBubbleSecurityViewInfo::PopulateForSuspiciousUi()
// at 156; the downloads page keeps these without its second prompt too).
// That is a file type's warning; everything else that might be malicious
// is deleted by voice only.
bool KeepableByVoice(const DownloadItemModel& model) {
  return model.GetDangerUiPattern() ==
             DownloadUIModel::DangerUiPattern::kSuspicious &&
         !model.MightBeMalicious() &&
         model.GetDangerType() == download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE;
}

// Keeps or deletes |model|'s download as the bubble's warning does when its
// button is clicked: DownloadBubbleUIController::ProcessDownloadButtonPress()
// records the action on the warning (DownloadItemWarningData) and runs what
// the bubble runs for it, then DownloadCommands sends the download report
// and validates or deletes the file. Without a bubble for the download, the
// same record and command.
void ProcessAsTheBubbleDoes(download::DownloadItem* item,
                            DownloadItemModel* model,
                            DownloadCommands::Command command) {
  if (DownloadBubbleUIController* bubble =
          DownloadBubbleUIController::GetForDownload(item)) {
    bubble->ProcessDownloadButtonPress(model->GetWeakPtr(), command,
                                       /*is_main_view=*/false);
    return;
  }
  DownloadItemWarningData::AddWarningActionEvent(
      item, DownloadItemWarningData::WarningSurface::BUBBLE_SUBPAGE,
      command == DownloadCommands::KEEP
          ? DownloadItemWarningData::WarningAction::PROCEED
          : DownloadItemWarningData::WarningAction::DISCARD);
  DownloadCommands(model->GetWeakPtr()).ExecuteCommand(command);
}

std::string Reply(PromptKind kind, PromptAnswer answer) {
  PendingPrompt pending;
  pending.kind = kind;
  PromptReply reply;
  reply.answer = answer;
  return ReplySpeech(pending, reply);
}

// What a permission prompt asks, in the browser's words ("use your
// camera"); empty for what has none.
std::string PermissionPhrase(permissions::RequestType type) {
  switch (type) {
    case permissions::RequestType::kCameraStream:
      return "use your camera";
    case permissions::RequestType::kMicStream:
      return "use your microphone";
    case permissions::RequestType::kGeolocation:
      return "know your location";
    case permissions::RequestType::kNotifications:
      return "show notifications";
    case permissions::RequestType::kClipboard:
      return "see what you copy to the clipboard";
    case permissions::RequestType::kMidiSysex:
      return "control your MIDI devices";
    case permissions::RequestType::kStorageAccess:
    case permissions::RequestType::kTopLevelStorageAccess:
      return "use its cookies on other sites";
    case permissions::RequestType::kMultipleDownloads:
      return "download several files";
    case permissions::RequestType::kLocalNetwork:
    case permissions::RequestType::kLoopbackNetwork:
      return "reach devices on your local network";
    case permissions::RequestType::kSensors:
      return "use your motion sensors";
    case permissions::RequestType::kWindowManagement:
      return "manage windows on all your screens";
    case permissions::RequestType::kFileSystemAccess:
      return "edit files";
    case permissions::RequestType::kIdleDetection:
      return "know when you're using your device";
    default:
      return std::string();
  }
}

}  // namespace

VoiceBrowserPrompts::Current::Current() = default;
VoiceBrowserPrompts::Current::~Current() = default;

VoiceBrowserPrompts::VoiceBrowserPrompts(Delegate* delegate)
    : delegate_(delegate), gestures_([this] {
        return random_for_testing_ ? random_for_testing_.Run()
                                   : base::RandUint64();
      }) {}

VoiceBrowserPrompts::~VoiceBrowserPrompts() {
  Detach();
}

void VoiceBrowserPrompts::SetClockForTesting(
    base::RepeatingCallback<base::TimeTicks()> clock) {
  clock_for_testing_ = std::move(clock);
}

void VoiceBrowserPrompts::SetRandomForTesting(
    base::RepeatingCallback<uint64_t()> random) {
  random_for_testing_ = std::move(random);
}

std::string VoiceBrowserPrompts::current_word_for_testing() const {
  return current_ ? current_->word : std::string();
}

bool VoiceBrowserPrompts::gesture_locked_out_for_testing() {
  return gestures_.LockedOut(NowMs());
}

int64_t VoiceBrowserPrompts::NowMs() const {
  const base::TimeTicks now =
      clock_for_testing_ ? clock_for_testing_.Run() : base::TimeTicks::Now();
  return (now - base::TimeTicks()).InMilliseconds();
}

// ---- The tab ----------------------------------------------------------------

void VoiceBrowserPrompts::Attach(content::WebContents* contents) {
  if (contents == web_contents()) {
    return;
  }
  Detach();
  if (!contents) {
    return;
  }
  Observe(contents);
  if (auto* manager =
          permissions::PermissionRequestManager::FromWebContents(contents)) {
    permissions_ = manager;
    permissions_->AddObserver(this);
  }
  if (content::DownloadManager* manager =
          contents->GetBrowserContext()->GetDownloadManager()) {
    downloads_ =
        std::make_unique<download::AllDownloadItemNotifier>(manager, this);
  }
  VoiceDialogManager::SetListener(contents, this);
}

void VoiceBrowserPrompts::Detach() {
  // Quietly: the delegate forgets the prompt as it leaves the tab (and may
  // be going away itself).
  if (current_) {
    if (current_->gesture_id) {
      gestures_.Cancel(current_->gesture_id);
    }
    if (current_->done) {
      std::move(current_->done).Run(false);
    }
    current_.reset();
  }
  password_timer_.Stop();
  password_read_ = false;
  if (permissions_) {
    permissions_->RemoveObserver(this);
    permissions_ = nullptr;
  }
  downloads_.reset();
  if (web_contents()) {
    VoiceDialogManager::RemoveListener(web_contents(), this);
  }
  Observe(nullptr);
}

void VoiceBrowserPrompts::WebContentsDestroyed() {
  Detach();
}

bool VoiceBrowserPrompts::TakesPromptsByVoice() {
  return web_contents() && delegate_->ListensForPrompts();
}

// ---- One prompt at a time ---------------------------------------------------

int VoiceBrowserPrompts::Begin(Source source,
                               BrowserPrompt prompt,
                               std::optional<GestureBinding> binding) {
  if (!delegate_->ListensForPrompts()) {
    return 0;
  }
  // A newer prompt takes the place of the one waiting; that one's window
  // or dialog stays as it is.
  Drop();
  auto current = std::make_unique<Current>();
  current->id = ++next_id_;
  current->source = source;
  current->kind = prompt.kind;
  if (binding) {
    // Locked out: no word; voice can only refuse.
    if (std::optional<GesturePrompt> gesture =
            gestures_.Begin(*binding, NowMs())) {
      current->gesture_id = gesture->id;
      current->word = gesture->word;
    }
    current->binding = std::move(*binding);
  }
  const int id = current->id;
  current_ = std::move(current);
  if (!delegate_->ReadBrowserPrompt(
          id, prompt, current_ ? current_->word : std::string())) {
    Drop();
    return 0;
  }
  if (!current_ || current_->id != id) {
    return 0;
  }
  current_->timer.Start(FROM_HERE,
                        base::Milliseconds(PromptWindowMs(prompt.kind)),
                        base::BindOnce(&VoiceBrowserPrompts::OnTimeout,
                                       weak_factory_.GetWeakPtr(), id));
  return id;
}

void VoiceBrowserPrompts::Finish(std::unique_ptr<Current> prompt,
                                 const std::string& said) {
  if (!prompt) {
    return;
  }
  if (prompt->gesture_id) {
    gestures_.Cancel(prompt->gesture_id);
  }
  if (prompt->done) {
    std::move(prompt->done).Run(false);
  }
  delegate_->EndBrowserPrompt(prompt->id, said);
}

void VoiceBrowserPrompts::Drop() {
  Finish(std::move(current_), std::string());
}

void VoiceBrowserPrompts::OnTimeout(int id) {
  if (!current_ || current_->id != id) {
    return;
  }
  std::unique_ptr<Current> prompt = std::move(current_);
  // No answer: a permission prompt is dismissed as the guard does (nothing
  // is remembered for the site); the rest stay as they are.
  if (prompt->source == Source::kPermission) {
    AnswerPermission(*prompt, PromptAnswer::kDecline, nullptr);
  }
  const PromptKind kind = prompt->kind;
  Finish(std::move(prompt), PromptTimeoutSpeech(kind));
}

void VoiceBrowserPrompts::Answer(const VoiceStep& step,
                                 const HeardEvidence& evidence) {
  if (!current_ || step.index != current_->id) {
    return;  // a prompt that went meanwhile
  }
  switch (step.prompt_answer) {
    case PromptAnswer::kAllow:
    case PromptAnswer::kWrongWord:
      AnswerWithWord(step, evidence);
      return;
    case PromptAnswer::kRefuse:
    case PromptAnswer::kAccept:
    case PromptAnswer::kDecline:
      break;
    case PromptAnswer::kNone:
    case PromptAnswer::kDictate:
    case PromptAnswer::kNeedWord:
    case PromptAnswer::kCantAllow:
      return;
  }
  // The loop said the answer already and forgot the prompt: it ends here
  // without words.
  std::unique_ptr<Current> prompt = std::move(current_);
  if (prompt->gesture_id) {
    gestures_.Cancel(prompt->gesture_id);
    prompt->gesture_id = 0;
  }
  switch (prompt->source) {
    case Source::kPermission:
      AnswerPermission(*prompt, step.prompt_answer, nullptr);
      break;
    case Source::kDialog:
      AnswerDialog(*prompt, step.prompt_answer, step.text);
      break;
    case Source::kDownload:
      AnswerDownload(*prompt, step.prompt_answer, nullptr);
      break;
    case Source::kPassword:
      AnswerPassword(step.prompt_answer);
      break;
    case Source::kSites:
      break;
  }
  if (prompt->done) {
    std::move(prompt->done).Run(false);
  }
}

void VoiceBrowserPrompts::AnswerWithWord(const VoiceStep& step,
                                         const HeardEvidence& evidence) {
  if (!current_->gesture_id) {
    Drop();
    return;
  }
  VoiceGesture gesture;
  const HearResult heard = gestures_.Hear(current_->gesture_id, step.text,
                                          evidence, NowMs(), &gesture);
  if (heard == HearResult::kNotAWord) {
    return;
  }
  if (heard == HearResult::kNotClean) {
    // It keeps waiting.
    delegate_->SayAboutBrowserPrompt(NotCleanSpeech(current_->word));
    return;
  }
  // The gesture's prompt is closed either way.
  std::unique_ptr<Current> prompt = std::move(current_);
  prompt->gesture_id = 0;
  switch (heard) {
    case HearResult::kMinted: {
      std::string said;
      switch (prompt->source) {
        case Source::kPermission:
          said = AnswerPermission(*prompt, PromptAnswer::kAllow, &gesture);
          break;
        case Source::kDownload:
          said = AnswerDownload(*prompt, PromptAnswer::kAllow, &gesture);
          break;
        case Source::kSites: {
          const bool confirmed =
              gestures_.Consume(gesture, prompt->binding, NowMs());
          base::OnceCallback<void(bool)> done = std::move(prompt->done);
          Finish(std::move(prompt),
                 confirmed
                     ? Reply(PromptKind::kConfirmSites, PromptAnswer::kAllow)
                     : std::string("I couldn't confirm them."));
          if (done) {
            std::move(done).Run(confirmed);
          }
          return;
        }
        case Source::kDialog:
        case Source::kPassword:
          break;
      }
      Finish(std::move(prompt), said);
      return;
    }
    case HearResult::kWrongWord: {
      std::string said = Reply(prompt->kind, PromptAnswer::kWrongWord);
      if (gestures_.LockedOut(NowMs())) {
        said +=
            " That's twice: for the next ten minutes, allow things with the "
            "mouse or keyboard.";
      }
      if (prompt->source == Source::kPermission) {
        AnswerPermission(*prompt, PromptAnswer::kDecline, nullptr);
      }
      Finish(std::move(prompt), said);
      return;
    }
    case HearResult::kNotClean:
    case HearResult::kNotAWord:
    case HearResult::kNoPrompt:
    case HearResult::kLockedOut:
      Finish(std::move(prompt),
             "I can't take the word now. Use the mouse or keyboard.");
      return;
  }
}

// ---- Permission prompts -----------------------------------------------------

void VoiceBrowserPrompts::OnPromptAdded() {
  // Not from inside the manager's own notification.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&VoiceBrowserPrompts::ReadPermissionPrompt,
                                weak_factory_.GetWeakPtr()));
}

void VoiceBrowserPrompts::OnPromptRemoved() {
  if (current_ && current_->source == Source::kPermission) {
    // Answered by a click, or gone with its page.
    Drop();
  }
}

void VoiceBrowserPrompts::OnPermissionRequestManagerDestructed() {
  if (permissions_) {
    permissions_->RemoveObserver(this);
    permissions_ = nullptr;
  }
  if (current_ && current_->source == Source::kPermission) {
    Drop();
  }
}

std::optional<GestureBinding> VoiceBrowserPrompts::PermissionBinding() {
  if (!permissions_ || permissions_->Requests().empty()) {
    return std::nullopt;
  }
  // The requesting origin and everything it asks for: a prompt that
  // changed in between isn't allowed by the word said for this one.
  GestureBinding binding;
  binding.action = GestureAction::kAllowPermission;
  binding.subject =
      url::Origin::Create(permissions_->GetRequestingOrigin()).Serialize();
  for (const auto& request : permissions_->Requests()) {
    binding.subject +=
        " " + base::NumberToString(static_cast<int>(request->request_type()));
  }
  return binding;
}

void VoiceBrowserPrompts::ReadPermissionPrompt() {
  if (!permissions_ || permissions_->Requests().empty() ||
      (current_ && current_->source == Source::kPermission &&
       current_->request == reinterpret_cast<uintptr_t>(
                                permissions_->Requests().front().get()))) {
    return;
  }
  BrowserPrompt prompt;
  prompt.kind = PromptKind::kPermission;
  prompt.site = permissions_->GetRequestingOrigin().host();
  std::vector<std::string> phrases;
  for (const auto& request : permissions_->Requests()) {
    std::string phrase = PermissionPhrase(request->request_type());
    if (!phrase.empty() &&
        std::find(phrases.begin(), phrases.end(), phrase) == phrases.end()) {
      phrases.push_back(std::move(phrase));
    }
  }
  // "use your camera and microphone".
  for (size_t i = 0; i < phrases.size(); ++i) {
    std::string phrase = phrases[i];
    if (i > 0 && phrase.rfind("use your ", 0) == 0 &&
        phrases[i - 1].rfind("use your ", 0) == 0) {
      phrase.erase(0, 9);
    }
    prompt.what += (i == 0 ? "" : " and ") + phrase;
  }
  const uintptr_t request =
      reinterpret_cast<uintptr_t>(permissions_->Requests().front().get());
  if (Begin(Source::kPermission, std::move(prompt), PermissionBinding()) &&
      current_) {
    current_->request = request;
  }
}

std::string VoiceBrowserPrompts::AnswerPermission(const Current& prompt,
                                                  PromptAnswer answer,
                                                  const VoiceGesture* gesture) {
  // Still the prompt that was read: another one's answer is no one's.
  if (!permissions_ || permissions_->Requests().empty() ||
      prompt.request !=
          reinterpret_cast<uintptr_t>(permissions_->Requests().front().get())) {
    return std::string();
  }
  const PromptOptions options(std::monostate{});
  switch (answer) {
    case PromptAnswer::kAllow: {
      // The gesture is used for the prompt as it is now; a changed prompt
      // (other requests, another origin) isn't allowed by it.
      const std::optional<GestureBinding> binding = PermissionBinding();
      if (!gesture || !binding ||
          !gestures_.Consume(*gesture, *binding, NowMs())) {
        permissions_->Dismiss(options);
        return "I couldn't allow it.";
      }
      // For this visit where Chromium has temporary grants for all of it,
      // as the prompt's "Allow this time".
      bool temporary = true;
      for (const auto& request : permissions_->Requests()) {
        temporary = temporary &&
                    permissions::PermissionUtil::DoesSupportTemporaryGrants(
                        request->GetContentSettingsType());
      }
      if (temporary) {
        permissions_->AcceptThisTime(options);
        return "Allowed for this visit.";
      }
      permissions_->Accept(options);
      return Reply(PromptKind::kPermission, PromptAnswer::kAllow);
    }
    case PromptAnswer::kRefuse:
      permissions_->Deny(options);
      return std::string();
    case PromptAnswer::kDecline:
    case PromptAnswer::kWrongWord:
      permissions_->Dismiss(options);
      return std::string();
    case PromptAnswer::kNone:
    case PromptAnswer::kAccept:
    case PromptAnswer::kDictate:
    case PromptAnswer::kNeedWord:
    case PromptAnswer::kCantAllow:
      return std::string();
  }
  return std::string();
}

// ---- A page's dialogs -------------------------------------------------------

void VoiceBrowserPrompts::AnswerDialog(const Current& prompt,
                                       PromptAnswer answer,
                                       const std::string& text) {
  if (answer != PromptAnswer::kAccept && answer != PromptAnswer::kDecline) {
    return;
  }
  // prompt(): the dictated answer, else the field's default.
  const std::u16string answer_text =
      prompt.kind == PromptKind::kTextPrompt
          ? (text.empty() ? prompt.default_text : base::UTF8ToUTF16(text))
          : std::u16string();
  VoiceDialogManager::Answer(web_contents(), answer == PromptAnswer::kAccept,
                             answer_text);
}

void VoiceBrowserPrompts::OnDialogShown(
    content::WebContents* contents,
    const VoiceDialogManager::Dialog& dialog) {
  if (contents != web_contents()) {
    return;
  }
  BrowserPrompt prompt;
  prompt.kind = dialog.kind;
  prompt.site = dialog.origin.host();
  prompt.reload = dialog.reload;
  if (dialog.kind != PromptKind::kBeforeUnload) {
    // The page's text: cleaned and capped where it is said
    // (voice_prompts.h).
    prompt.text = base::UTF16ToUTF8(dialog.message);
    prompt.flood = !dialogs_read_.ShouldRead(NowMs());
  }
  const std::u16string default_text = dialog.default_text;
  // Not from inside the dialog manager's call.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<VoiceBrowserPrompts> self, BrowserPrompt prompt,
             std::u16string default_text) {
            if (!self) {
              return;
            }
            if (self->Begin(Source::kDialog, std::move(prompt), std::nullopt) &&
                self->current_) {
              self->current_->default_text = std::move(default_text);
            }
          },
          weak_factory_.GetWeakPtr(), std::move(prompt), default_text));
}

void VoiceBrowserPrompts::OnDialogClosed(content::WebContents* contents) {
  if (contents == web_contents() && current_ &&
      current_->source == Source::kDialog) {
    Drop();
  }
}

// ---- Dangerous downloads ----------------------------------------------------

void VoiceBrowserPrompts::OnDownloadUpdated(content::DownloadManager* manager,
                                            download::DownloadItem* item) {
  if (current_ && current_->source == Source::kDownload &&
      current_->download_id == item->GetId() &&
      (!item->IsDangerous() ||
       item->GetState() != download::DownloadItem::IN_PROGRESS)) {
    // Decided on the downloads page or in the bubble.
    Drop();
    return;
  }
  MaybeReadDownload(item);
}

void VoiceBrowserPrompts::MaybeReadDownload(download::DownloadItem* item) {
  if (!item || !web_contents() ||
      content::DownloadItemUtils::GetWebContents(item) != web_contents() ||
      !item->IsDangerous() ||
      item->GetState() != download::DownloadItem::IN_PROGRESS ||
      downloads_read_.contains(item->GetId())) {
    return;
  }
  DownloadItemModel model(item);
  const DownloadUIModel::DangerUiPattern pattern = model.GetDangerUiPattern();
  if ((pattern != DownloadUIModel::DangerUiPattern::kDangerous &&
       pattern != DownloadUIModel::DangerUiPattern::kSuspicious) ||
      IsScanning(model.GetDangerType())) {
    return;  // still being scanned, or an error
  }
  if (downloads_read_.size() >= 1000) {
    downloads_read_.clear();
  }
  downloads_read_.insert(item->GetId());
  BrowserPrompt prompt;
  prompt.kind = PromptKind::kDangerousDownload;
  prompt.site = item->GetURL().host();
  // The file's name, as the page or server named it: page text.
  prompt.text = item->GetFileNameToReportUser().AsUTF8Unsafe();
  // What the bubble shows in red (malicious, or might be) is deleted by
  // voice only; keeping it is left to the downloads page.
  prompt.malicious = pattern == DownloadUIModel::DangerUiPattern::kDangerous ||
                     model.MightBeMalicious();
  std::optional<GestureBinding> binding;
  if (!prompt.malicious && KeepableByVoice(model)) {
    binding = KeepBinding(item);
  }
  const uint32_t id = item->GetId();
  if (Begin(Source::kDownload, std::move(prompt), std::move(binding)) &&
      current_) {
    current_->download_id = id;
    // Said and shown: the warning was shown, as the bubble records it when
    // it shows one (the first time only).
    DownloadItemWarningData::AddWarningActionEvent(
        item, DownloadItemWarningData::WarningSurface::BUBBLE_SUBPAGE,
        DownloadItemWarningData::WarningAction::SHOWN);
  }
}

std::string VoiceBrowserPrompts::AnswerDownload(const Current& prompt,
                                                PromptAnswer answer,
                                                const VoiceGesture* gesture) {
  content::DownloadManager* manager =
      web_contents() ? web_contents()->GetBrowserContext()->GetDownloadManager()
                     : nullptr;
  download::DownloadItem* item =
      manager ? manager->GetDownload(prompt.download_id) : nullptr;
  if (!item || !item->IsDangerous() ||
      item->GetState() != download::DownloadItem::IN_PROGRESS) {
    return std::string();
  }
  auto model = std::make_unique<DownloadItemModel>(item);
  switch (answer) {
    case PromptAnswer::kAllow:
      // Never a file the bubble shows in red, nor one it doesn't keep with
      // one click, whatever was minted.
      if (!gesture || !KeepableByVoice(*model) ||
          !gestures_.Consume(*gesture, KeepBinding(item), NowMs())) {
        return "I couldn't keep it. You can decide on the downloads page.";
      }
      ProcessAsTheBubbleDoes(item, model.get(), DownloadCommands::KEEP);
      return Reply(PromptKind::kDangerousDownload, PromptAnswer::kAllow);
    case PromptAnswer::kRefuse:
      ProcessAsTheBubbleDoes(item, model.get(), DownloadCommands::DISCARD);
      return std::string();
    case PromptAnswer::kNone:
    case PromptAnswer::kWrongWord:
    case PromptAnswer::kAccept:
    case PromptAnswer::kDecline:
    case PromptAnswer::kDictate:
    case PromptAnswer::kNeedWord:
    case PromptAnswer::kCantAllow:
      return std::string();
  }
  return std::string();
}

// ---- The password bubble ----------------------------------------------------

void VoiceBrowserPrompts::DidFinishNavigation(
    content::NavigationHandle* navigation) {
  if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted() &&
      !navigation->IsSameDocument()) {
    password_timer_.Start(FROM_HERE, kPasswordCheckDelay,
                          base::BindOnce(&VoiceBrowserPrompts::CheckPassword,
                                         weak_factory_.GetWeakPtr()));
  }
}

void VoiceBrowserPrompts::DidStopLoading() {
  password_timer_.Start(FROM_HERE, kPasswordCheckDelay,
                        base::BindOnce(&VoiceBrowserPrompts::CheckPassword,
                                       weak_factory_.GetWeakPtr()));
}

void VoiceBrowserPrompts::CheckPassword() {
  ManagePasswordsUIController* passwords =
      web_contents()
          ? ManagePasswordsUIController::FromWebContents(web_contents())
          : nullptr;
  if (!passwords ||
      passwords->GetState() != password_manager::ui::PENDING_PASSWORD_STATE) {
    password_read_ = false;
    return;
  }
  if (password_read_) {
    return;
  }
  password_read_ = true;
  BrowserPrompt prompt;
  prompt.kind = PromptKind::kSavePassword;
  // The site only: nothing of the form is said.
  prompt.site = passwords->GetOrigin().host();
  Begin(Source::kPassword, std::move(prompt), std::nullopt);
}

void VoiceBrowserPrompts::AnswerPassword(PromptAnswer answer) {
  ManagePasswordsUIController* passwords =
      web_contents()
          ? ManagePasswordsUIController::FromWebContents(web_contents())
          : nullptr;
  if (!passwords ||
      passwords->GetState() != password_manager::ui::PENDING_PASSWORD_STATE) {
    return;
  }
  switch (answer) {
    case PromptAnswer::kAccept: {
      // As the bubble's Save does, with what the form held.
      const password_manager::PasswordForm& form =
          passwords->GetPendingPassword();
      passwords->SavePassword(form.username_value, form.password_value);
      return;
    }
    case PromptAnswer::kRefuse:
      passwords->NeverSavePassword();
      return;
    case PromptAnswer::kDecline:
      passwords->OnNotNowClicked();
      return;
    case PromptAnswer::kNone:
    case PromptAnswer::kAllow:
    case PromptAnswer::kWrongWord:
    case PromptAnswer::kDictate:
    case PromptAnswer::kNeedWord:
    case PromptAnswer::kCantAllow:
      return;
  }
}

// ---- A workflow draft's sites -----------------------------------------------

bool VoiceBrowserPrompts::ConfirmDraftSites(
    const std::string& workflow_id,
    const std::vector<std::string>& sites,
    base::OnceCallback<void(bool)> done) {
  if (sites.empty() || workflow_id.empty() || !delegate_->ListensForPrompts()) {
    return false;
  }
  BrowserPrompt prompt;
  prompt.kind = PromptKind::kConfirmSites;
  std::vector<std::string> clean;
  for (const std::string& site : sites) {
    std::string spoken = activation::SiteForSpeech(site);
    if (!spoken.empty()) {
      clean.push_back(std::move(spoken));
    }
  }
  for (size_t i = 0; i < clean.size(); ++i) {
    prompt.text += (i == 0                  ? ""
                    : i + 1 == clean.size() ? " and "
                                            : ", ") +
                   clean[i];
  }
  const int id = Begin(Source::kSites, std::move(prompt),
                       DraftSitesBinding(workflow_id, sites));
  if (!id || !current_ || current_->id != id) {
    return false;
  }
  current_->done = std::move(done);
  return true;
}

}  // namespace voice
}  // namespace molt_ai
