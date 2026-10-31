// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_session.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_language.h"
#include "chrome/browser/molt_ai/voice/voice_learning.h"
#include "chrome/browser/molt_ai/voice/voice_listener_protocol.h"
#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai {
namespace voice {

namespace {

// The window that listens: one microphone, one helper.
VoiceSession* g_listening = nullptr;

// The wake phrase (TOUCHLESS_BROWSING.md §11, decision 2: "Hey Molt" until
// the owner chooses).
constexpr char kWakePhrase[] = "hey molt";

// How often the session looks at the tabs' sound and the speech engine
// (well within kOtherSpeechTailMs).
constexpr base::TimeDelta kPollInterval = base::Milliseconds(250);

// Where the microphone has no echo canceller, nothing said over the
// browser's speech gives a command but "stop" (voice_session_logic.h).
constexpr char kStopFirstHint[] =
    "To interrupt me, say stop, then what you want.";

ListeningSettings ToListening(const VoiceModeSettings& settings) {
  ListeningSettings listening;
  listening.always_listening = settings.always_listening;
  listening.wake_phrases = {kWakePhrase};
  listening.vad_silence_ms = kDefaultVadSilenceMs;
  return listening;
}

BrowserActivity ToBrowserActivity(VoiceActivity activity) {
  switch (activity) {
    case VoiceActivity::kThinking:
      return BrowserActivity::kThinking;
    case VoiceActivity::kSpeaking:
      return BrowserActivity::kSpeaking;
    case VoiceActivity::kOff:
    case VoiceActivity::kListening:
      return BrowserActivity::kIdle;
  }
  return BrowserActivity::kIdle;
}

std::string ProblemText(VoiceAudio::Problem problem) {
  switch (problem) {
    case VoiceAudio::Problem::kPermissionDenied:
      return "I can't use the microphone: the system's privacy settings "
             "don't let MoltBrowser use it. Allow it there, then turn voice "
             "mode on again.";
    case VoiceAudio::Problem::kNoMicrophone:
      return "I can't find a microphone. Connect one, then turn voice mode "
             "on again.";
    case VoiceAudio::Problem::kFailed:
      return "The microphone stopped working. Turn voice mode on again to "
             "try once more.";
  }
  return "I can't use the microphone.";
}

struct FoundHelper {
  std::optional<VoiceListener::Paths> paths;
  std::string why;
};

FoundHelper FindHelper() {
  FoundHelper found;
  found.paths = VoiceListener::FindBundled(&found.why);
  return found;
}

// The page's names, then the personal vocabulary's (TOUCHLESS_BROWSING.md
// §6.8): at most kMaxPersonalHotwords of CONFIG's kMaxHotwords, while
// learning is on. Neither holds an answer to a question.
std::vector<std::string> HotwordsFor(const PageMap& map, Profile* profile) {
  std::vector<std::string> personal;
  VoiceLearning* learning = VoiceLearning::Get(profile);
  if (learning && learning->enabled()) {
    for (const std::string& phrase :
         learning->vocabulary().HotwordPhrases(kMaxPersonalHotwords)) {
      std::string reduced = ReduceSpeechPhrase(phrase);
      if (!reduced.empty()) {
        personal.push_back(std::move(reduced));
      }
    }
  }
  std::vector<std::string> hotwords =
      HotwordsFromPageMap(map, kMaxHotwords - personal.size());
  auto lower = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return text;
  };
  std::vector<std::string> seen;
  for (const std::string& phrase : hotwords) {
    seen.push_back(lower(phrase));
  }
  for (std::string& phrase : personal) {
    std::string key = lower(phrase);
    if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
      seen.push_back(std::move(key));
      hotwords.push_back(std::move(phrase));
    }
  }
  return hotwords;
}

}  // namespace

DEFINE_USER_DATA(VoiceSession);

// static
VoiceSession* VoiceSession::FromBrowser(BrowserWindowInterface* browser) {
  return browser ? Get(browser->GetUnownedUserDataHost()) : nullptr;
}

// static
VoiceSession* VoiceSession::GetOrCreateForBrowser(
    BrowserWindowInterface* browser) {
  if (!browser || browser->IsDeleteScheduled() ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !VoiceModeEnabled()) {
    return nullptr;
  }
  if (VoiceSession* existing = FromBrowser(browser)) {
    return existing;
  }
  // Owned by itself: deleted when the window closes (OnBrowserDidClose()).
  return new VoiceSession(browser);
}

// static
void VoiceSession::OnWindowReady(BrowserWindowInterface* browser) {
  // Once per run of the browser, in the first normal window that isn't a
  // MoltNet one.
  static bool checked = false;
  VoiceSession* session = GetOrCreateForBrowser(browser);
  if (!session || checked || session->IsMoltNet()) {
    return;
  }
  checked = true;
  // On settings.json's own sequence (common/molt_settings_file.h).
  MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadVoiceModeSettings),
      base::BindOnce(
          [](base::WeakPtr<VoiceSession> self,
             const VoiceModeSettings& settings) {
            if (self && settings.start_with_browser) {
              self->Start();
            }
          },
          session->weak_factory_.GetWeakPtr()));
}

// static
void VoiceSession::OnSettingsChanged(const VoiceModeSettings& settings) {
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    VoiceSession* session = FromBrowser(window);
    if (!session) {
      continue;
    }
    session->settings_ = settings;
    if (session->listening()) {
      session->Apply(session->logic_.OnSettingsChanged(ToListening(settings),
                                                       session->NowMs()));
    }
  }
}

// static
VoiceSession* VoiceSession::Listening() {
  return g_listening;
}

VoiceSession::VoiceSession(BrowserWindowInterface* browser)
    : browser_(browser),
      scoped_data_holder_(browser->GetUnownedUserDataHost(), *this),
      logic_(ToListening(VoiceModeSettings())) {
  did_close_subscription_ =
      browser->RegisterBrowserDidClose(base::BindRepeating(
          &VoiceSession::OnBrowserDidClose, base::Unretained(this)));
  controller_ = VoiceController::GetOrCreateForBrowser(browser);
  if (controller_) {
    controller_->AddObserver(this);
  }
}

VoiceSession::~VoiceSession() {
  for (Observer& observer : observers_) {
    observer.OnVoiceSessionDestroying(this);
  }
  StopListening();
  if (controller_) {
    controller_->RemoveObserver(this);
  }
  if (g_listening == this) {
    g_listening = nullptr;
  }
}

void VoiceSession::OnBrowserDidClose(BrowserWindowInterface* browser) {
  delete this;
}

bool VoiceSession::IsMoltNet() const {
  return browser_ && tor::IsMoltNetProfile(browser_->GetProfile());
}

int64_t VoiceSession::NowMs() const {
  return (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds();
}

BrowserActivity VoiceSession::ControllerActivity() const {
  return controller_ ? ToBrowserActivity(controller_->activity())
                     : BrowserActivity::kIdle;
}

void VoiceSession::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void VoiceSession::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

// ---- Starting and stopping --------------------------------------------------

bool VoiceSession::Start() {
  if (logic_.running()) {
    return true;
  }
  if (!browser_ || !VoiceModeEnabled() || !controller_) {
    return false;
  }
  // Pages in MoltNet windows never get the microphone (§7.4).
  if (IsMoltNet()) {
    controller_->SayNotice("Voice mode is off in MoltNet windows.");
    return false;
  }
  // No recognizer for voice mode's language yet (voice_language.h): it says
  // so, in that language, and the microphone stays closed.
  if (!ListeningAvailable(controller_->language())) {
    controller_->SayNotice(
        std::string(ListeningUnavailableNotice(controller_->language())));
    return false;
  }
  if (g_listening && g_listening != this) {
    g_listening->Stop();
  }
  if (!controller_->Start()) {
    return false;
  }
  g_listening = this;
  problem_.clear();
  partial_.clear();
  announced_ = false;
  if (!earcons_) {
    earcons_ = std::make_unique<VoiceEarcons>();
  }
  Apply(logic_.Start(NowMs()));
  Apply(logic_.OnBrowserActivity(ControllerActivity(), NowMs()));
  MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadVoiceModeSettings),
      base::BindOnce(&VoiceSession::OnSettingsRead,
                     weak_factory_.GetWeakPtr()));
  for (Observer& observer : observers_) {
    observer.OnVoiceSessionChanged(this);
  }
  return true;
}

void VoiceSession::OnSettingsRead(const VoiceModeSettings& settings) {
  settings_ = settings;
  if (!logic_.running()) {
    return;
  }
  Apply(logic_.OnSettingsChanged(ToListening(settings), NowMs()));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&FindHelper),
      base::BindOnce(
          [](base::WeakPtr<VoiceSession> self, FoundHelper found) {
            if (self) {
              self->OnHelperFound(std::move(found.paths), found.why);
            }
          },
          weak_factory_.GetWeakPtr()));
}

void VoiceSession::OnHelperFound(std::optional<VoiceListener::Paths> paths,
                                 const std::string& why) {
  if (!logic_.running() || listener_) {
    return;
  }
  if (!paths) {
    Fail("I can't listen: " + why + ".");
    return;
  }
  listener_ = std::make_unique<VoiceListener>(this, std::move(*paths));
  listener_->Start();
  // Frames reach the helper once it is ready.
  audio_ = std::make_unique<VoiceAudio>(this, listener_->audio_sink());
  audio_->Start();
  poll_timer_.Start(
      FROM_HERE, kPollInterval,
      base::BindRepeating(&VoiceSession::OnPoll, base::Unretained(this)));
}

void VoiceSession::Stop() {
  StopSession(/*stop_voice_mode=*/true);
}

void VoiceSession::StopSession(bool stop_voice_mode) {
  if (!logic_.running()) {
    if (stop_voice_mode && controller_ && !problem_.empty()) {
      // A problem left on screen goes with voice mode.
      problem_.clear();
      UpdateCaption();
      controller_->Stop();
    }
    return;
  }
  Apply(logic_.Stop(NowMs()));
  StopListening();
  problem_.clear();
  UpdateCaption();
  if (g_listening == this) {
    g_listening = nullptr;
  }
  if (stop_voice_mode && controller_) {
    controller_->Stop();
  }
  for (Observer& observer : observers_) {
    observer.OnVoiceSessionChanged(this);
  }
}

void VoiceSession::Toggle() {
  if (listening()) {
    Stop();
  } else {
    Start();
  }
}

void VoiceSession::StopListening() {
  poll_timer_.Stop();
  tick_timer_.Stop();
  // The microphone first (no more frames), then the helper. Both stop now;
  // they go once whatever of theirs called here has returned.
  if (audio_) {
    audio_->Stop();
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(audio_));
  }
  if (listener_) {
    listener_->Stop();
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(listener_));
  }
  SetDucking(false);
  partial_.clear();
  speaking_hint_.clear();
  hotwords_.clear();
  microphone_on_ = false;
  echo_ = EchoCancellation::kNone;
}

void VoiceSession::Fail(const std::string& message) {
  LOG(WARNING) << "[MoltVoice] " << message;
  Apply(logic_.Stop(NowMs()));
  StopListening();
  problem_ = message;
  if (g_listening == this) {
    g_listening = nullptr;
  }
  if (controller_) {
    controller_->SayNotice(message);
  }
  UpdateCaption();
  for (Observer& observer : observers_) {
    observer.OnVoiceSessionChanged(this);
  }
}

// ---- The helper -------------------------------------------------------------

void VoiceSession::OnListenerReady(const HelperEvent& ready) {
  LOG(INFO) << "[MoltVoice] speech helper ready: " << ready.engine
            << (listener_ ? ", " + listener_->confinement() : std::string());
  const bool first = !announced_;
  announced_ = true;
  problem_.clear();
  Apply(logic_.OnHelperReady(NowMs()));
  if (first && controller_) {
    std::string notice =
        settings_.always_listening
            ? "Voice mode is on. I'm listening."
            : "Voice mode is on. Say Hey Molt, then what you want.";
    if (microphone_on_ && echo_ == EchoCancellation::kNone) {
      // Without an echo canceller, only "stop" interrupts the browser.
      notice += " " + std::string(kStopFirstHint);
    }
    controller_->SayNotice(notice);
  }
}

void VoiceSession::OnListenerEvent(const HelperEvent& event) {
  const int64_t now = NowMs();
  // What the helper heard may be another source's speech, or a tab's
  // sound: look now too, not only at the next poll.
  if (event.type != HelperEventType::kError &&
      event.type != HelperEventType::kReady) {
    Apply(logic_.OnOtherSpeech(OthersSpeaking(), now));
    Apply(logic_.OnPageAudible(AnyTabAudible(), now));
    Apply(logic_.OnOwnSpeechInReference(OwnSpeechInReference(), now));
    Apply(logic_.OnOwnVoiceUnfiltered(OwnVoiceUnfiltered(), now));
  }
  // The event's lag behind the microphone (the listener's audio clock).
  const std::optional<int64_t> lag = event.lag_ms;
  switch (event.type) {
    case HelperEventType::kWake:
      Apply(logic_.OnWake(now, lag));
      return;
    case HelperEventType::kSpeechStart:
      Apply(logic_.OnSpeechStart(now, lag));
      return;
    case HelperEventType::kPartial:
      Apply(logic_.OnPartial(event.text, now, lag));
      return;
    case HelperEventType::kFinal:
      Apply(logic_.OnFinal(event.text, event.confidence, now, lag));
      return;
    case HelperEventType::kSpeechEnd:
      Apply(logic_.OnSpeechEnd(now, lag));
      return;
    case HelperEventType::kError:
      LOG(WARNING) << "[MoltVoice] speech helper: " << event.code << ": "
                   << event.message;
      return;
    case HelperEventType::kReady:
      return;
  }
}

void VoiceSession::OnListenerLost(VoiceListener::StopReason reason,
                                  const std::string& why,
                                  bool restarting) {
  problem_ = "Speech recognition is restarting";
  Apply(logic_.OnHelperLost(NowMs()));
  UpdateCaption();
}

void VoiceSession::OnListenerGaveUp(const std::string& why) {
  Fail("Speech recognition stopped working: " + why +
       ". Turn voice mode on again with the Voice button.");
}

// ---- The microphone ---------------------------------------------------------

void VoiceSession::OnMicrophoneStarted(EchoCancellation echo) {
  microphone_on_ = true;
  echo_ = echo;
  Apply(logic_.OnOwnSpeechInReference(OwnSpeechInReference(), NowMs()));
  LOG(INFO) << "[MoltVoice] microphone on, echo cancellation: "
            << (echo == EchoCancellation::kChromeWide ? "Chrome-wide"
                : echo == EchoCancellation::kPlatform ? "the platform's"
                                                      : "none");
  // Without one, a tab's sound reaches the recognizer as it is: while a tab
  // plays sound, only the commands that always count do.
  Apply(logic_.OnEchoCancellation(echo != EchoCancellation::kNone, NowMs()));
}

void VoiceSession::OnMicrophoneProblem(VoiceAudio::Problem problem) {
  Fail(ProblemText(problem));
}

// ---- The voice loop ---------------------------------------------------------

void VoiceSession::OnVoiceCaption(VoiceActivity activity,
                                  const std::string& heard,
                                  const std::string& status) {
  if (!logic_.running() || !controller_) {
    return;
  }
  const int64_t now = NowMs();
  // Whether voice mode's own voice plays unfiltered changes with what it
  // does: looked at before the activity, so its end is seen at once.
  Apply(logic_.OnOwnVoiceUnfiltered(OwnVoiceUnfiltered(), now));
  Apply(logic_.OnBrowserActivity(ToBrowserActivity(activity), now));
  if (activity != VoiceActivity::kSpeaking && !speaking_hint_.empty()) {
    speaking_hint_.clear();
    UpdateCaption();
  }
  const ConversationState& state = controller_->state();
  // The browser's prompt waits for its answer too (voice_prompts.h).
  Apply(logic_.OnAwaitingAnswer(
      state.confirming || state.expecting_choice || state.prompt.id != 0, now));
}

void VoiceSession::OnVoiceSpeech(const std::string& text) {
  if (logic_.running()) {
    Apply(logic_.OnBrowserSaid(text, NowMs()));
  }
}

void VoiceSession::OnVoiceModeRequested(bool on) {
  if (on) {
    Start();
  } else {
    // The controller stops itself once it has said "Voice mode is off".
    StopSession(/*stop_voice_mode=*/false);
  }
}

void VoiceSession::OnVoicePageMapped(const PageMap& map) {
  if (!logic_.running()) {
    return;
  }
  std::vector<std::string> hotwords =
      HotwordsFor(map, browser_ ? browser_->GetProfile() : nullptr);
  if (hotwords != hotwords_) {
    hotwords_ = std::move(hotwords);
    SendConfig();
  }
}

void VoiceSession::OnVoiceControllerDestroying(VoiceController* controller) {
  if (controller == controller_) {
    controller->RemoveObserver(this);
    controller_ = nullptr;
  }
}

// ---- Carrying out -----------------------------------------------------------

void VoiceSession::Apply(SessionActions actions) {
  if (actions.earcon && earcons_) {
    earcons_->Play(*actions.earcon);
  }
  if (actions.duck) {
    SetDucking(*actions.duck);
  }
  if (actions.wake_required) {
    SendConfig();
  }
  if (actions.reset_helper && listener_) {
    listener_->SendReset();
  }
  if (actions.stop_speech && controller_) {
    controller_->StopSpeaking();
  }
  bool caption = actions.state.has_value();
  if (actions.said_over_voice && speaking_hint_.empty() &&
      ControllerActivity() == BrowserActivity::kSpeaking) {
    // Said over voice mode's own voice with no echo canceller: the user
    // says stop first, or waits.
    speaking_hint_ = kStopFirstHint;
    caption = true;
  }
  if (actions.partial) {
    partial_ = *actions.partial;
    caption = true;
  }
  if (actions.utterance) {
    partial_.clear();
    caption = true;
  }
  if (caption) {
    UpdateCaption();
  }
  if (actions.state) {
    for (Observer& observer : observers_) {
      observer.OnVoiceSessionChanged(this);
    }
  }
  if (actions.utterance && controller_) {
    // As if typed. The controller's caption calls report what it does.
    base::WeakPtr<VoiceSession> self = weak_factory_.GetWeakPtr();
    controller_->HandleHeardUtterance(*actions.utterance,
                                      actions.heard_cleanly);
    if (!self) {
      return;
    }
  }
  ScheduleTick();
}

void VoiceSession::SendConfig() {
  if (!listener_ || !listener_->ready()) {
    return;
  }
  HelperConfig config;
  config.wake_phrases = logic_.settings().wake_phrases;
  config.wake_required = logic_.wake_required();
  config.hotwords = hotwords_;
  config.vad_silence_ms = logic_.settings().vad_silence_ms;
  listener_->SendConfig(config);
}

void VoiceSession::SetDucking(bool duck) {
  if (!duck) {
    for (const base::WeakPtr<content::WebContents>& contents : ducked_) {
      if (!contents) {
        continue;
      }
      if (content::MediaSession* session =
              content::MediaSession::GetIfExists(contents.get())) {
        session->StopDucking();
      }
    }
    ducked_.clear();
    return;
  }
  if (!ducked_.empty()) {
    return;
  }
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    // Not MoltNet windows: a page there could notice its sound ducked
    // while the user speaks.
    if (tor::IsMoltNetProfile(window->GetProfile())) {
      continue;
    }
    TabStripModel* tabs = window->GetTabStripModel();
    if (!tabs) {
      continue;
    }
    for (int i = 0; i < tabs->count(); ++i) {
      content::WebContents* contents = tabs->GetWebContentsAt(i);
      content::MediaSession* session =
          contents ? content::MediaSession::GetIfExists(contents) : nullptr;
      if (session) {
        session->StartDucking();
        ducked_.push_back(contents->GetWeakPtr());
      }
    }
  }
}

void VoiceSession::UpdateCaption() {
  if (!controller_) {
    return;
  }
  VoiceController::ListeningCaption caption;
  caption.listening = logic_.running();
  caption.waiting = logic_.state() == SessionState::kWaiting;
  caption.starting = logic_.state() == SessionState::kStarting;
  caption.heard_live = partial_;
  caption.problem = problem_;
  caption.speaking_hint = speaking_hint_;
  controller_->SetListening(std::move(caption));
}

void VoiceSession::ScheduleTick() {
  const std::optional<int64_t> next = logic_.NextDeadline();
  if (!next) {
    tick_timer_.Stop();
    return;
  }
  const base::TimeDelta delay =
      base::Milliseconds(std::max<int64_t>(*next - NowMs(), 0));
  tick_timer_.Start(
      FROM_HERE, delay,
      base::BindOnce(&VoiceSession::OnTick, base::Unretained(this)));
}

void VoiceSession::OnTick() {
  Apply(logic_.OnTick(NowMs()));
}

bool VoiceSession::OthersSpeaking() const {
  return controller_ && controller_->OthersSpeaking();
}

bool VoiceSession::OwnSpeechInReference() const {
  return microphone_on_ && echo_ != EchoCancellation::kNone && controller_ &&
         controller_->SpeechInEchoReference();
}

bool VoiceSession::OwnVoiceUnfiltered() const {
  // SpeechInEchoReference(): voice mode speaks with its own voice, the
  // audio its helper sends; with no echo canceller the microphone hears it
  // as it is.
  return microphone_on_ && echo_ == EchoCancellation::kNone && controller_ &&
         controller_->activity() == VoiceActivity::kSpeaking &&
         controller_->SpeechInEchoReference();
}

bool VoiceSession::AnyTabAudible() const {
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    TabStripModel* tabs = window->GetTabStripModel();
    for (int i = 0; tabs && i < tabs->count(); ++i) {
      content::WebContents* contents = tabs->GetWebContentsAt(i);
      if (contents && contents->IsCurrentlyAudible()) {
        return true;
      }
    }
  }
  return false;
}

void VoiceSession::OnPoll() {
  Apply(logic_.OnOtherSpeech(OthersSpeaking(), NowMs()));
  Apply(logic_.OnOwnSpeechInReference(OwnSpeechInReference(), NowMs()));
  Apply(logic_.OnOwnVoiceUnfiltered(OwnVoiceUnfiltered(), NowMs()));
  Apply(logic_.OnPageAudible(AnyTabAudible(), NowMs()));
}

}  // namespace voice
}  // namespace molt_ai
