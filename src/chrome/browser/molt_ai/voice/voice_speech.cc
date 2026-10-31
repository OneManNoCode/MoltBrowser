// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_speech.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/voice/voice_speech_protocol.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_utterance.h"

namespace molt_ai {
namespace voice {

namespace {

// TtsUtterance's rate range ("0.1 to 10", as the Web Speech API's).
constexpr double kMinTtsRate = 0.1;
constexpr double kMaxTtsRate = 10.0;

struct FoundVoice {
  std::optional<BundledVoice::Paths> paths;
  std::string why;
};

FoundVoice FindVoice() {
  FoundVoice found;
  found.paths = BundledVoice::FindBundled(&found.why);
  return found;
}

}  // namespace

// Owned by its utterance; tells VoiceSpeech when the utterance goes, however
// it ends (spoken, stopped, dropped from the queue, never spoken).
class VoiceSpeech::Delegate : public content::UtteranceEventDelegate {
 public:
  explicit Delegate(base::WeakPtr<VoiceSpeech> owner)
      : owner_(std::move(owner)) {}
  Delegate(const Delegate&) = delete;
  Delegate& operator=(const Delegate&) = delete;
  ~Delegate() override {
    if (owner_) {
      owner_->OnGone(this);
    }
  }

  // Whether TtsController is speaking the utterance (it started and hasn't
  // ended: an ended one is deleted with this).
  bool started() const { return started_; }
  // The platform reported it over (said to its end, or it failed).
  bool ended() const { return ended_; }
  // Stopped before it started: it is stopped as it starts.
  bool cancelled() const { return cancelled_; }
  void Cancel() { cancelled_ = true; }

  // Stops the utterance, which TtsController is speaking (started()); the
  // rest of its queue is left as it is. Deletes this.
  void StopUtterance() {
    content::TtsController::GetInstance()->RemoveUtteranceEventDelegate(this);
  }

  // content::UtteranceEventDelegate:
  void OnTtsEvent(content::TtsUtterance* utterance,
                  content::TtsEventType event_type,
                  int char_index,
                  int length,
                  const std::string& error_message) override {
    if (event_type == content::TTS_EVENT_END ||
        event_type == content::TTS_EVENT_ERROR) {
      ended_ = true;
    }
    if (event_type != content::TTS_EVENT_START) {
      return;
    }
    started_ = true;
    if (cancelled_ || !owner_) {
      // Not from inside TtsController's own call: stopping deletes this.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(&Delegate::StopUtterance, weak_factory_.GetWeakPtr()));
    }
  }

 private:
  base::WeakPtr<VoiceSpeech> owner_;
  bool started_ = false;
  bool ended_ = false;
  bool cancelled_ = false;
  base::WeakPtrFactory<Delegate> weak_factory_{this};
};

VoiceSpeech::VoiceSpeech(content::BrowserContext* context)
    : context_(context) {}

VoiceSpeech::~VoiceSpeech() {
  Stop();
  if (bundled_) {
    bundled_->Stop();
  }
  // An utterance still waiting in TtsController's queue is stopped as it
  // starts (its delegate does that without us).
  weak_factory_.InvalidateWeakPtrs();
}

void VoiceSpeech::SetSpeakingChangedCallback(
    base::RepeatingCallback<void(bool)> callback) {
  speaking_changed_ = std::move(callback);
}

void VoiceSpeech::SetUtteranceDoneCallback(UtteranceDoneCallback callback) {
  utterance_done_ = std::move(callback);
}

void VoiceSpeech::Done(uint64_t id, bool spoken) {
  if (!id || !utterance_done_) {
    return;
  }
  // Posted: this may run inside TtsController's own calls.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<VoiceSpeech> self, uint64_t id,
                        bool spoken) {
                       if (self && self->utterance_done_) {
                         self->utterance_done_.Run(id, spoken);
                       }
                     },
                     weak_factory_.GetWeakPtr(), id, spoken));
}

bool VoiceSpeech::speaking() const {
  return !waiting_.empty() || !bundled_in_flight_.empty() ||
         (in_flight_ && !in_flight_->cancelled());
}

bool VoiceSpeech::OthersSpeaking() const {
  // Ours is being spoken (it started, or nothing else was when it was
  // handed over). The bundled voice doesn't use TtsController.
  if (in_flight_ && (in_flight_->started() || !in_flight_behind_others_)) {
    return false;
  }
  return content::TtsController::GetInstance()->IsSpeaking();
}

bool VoiceSpeech::InEchoReference() const {
  return bundled_ && bundled_->ready() && !in_flight_;
}

bool VoiceSpeech::CanSpeakPrivately() const {
  return bundled_ && bundled_->started() && bundled_->ready();
}

void VoiceSpeech::SetActive(bool active) {
  active_ = active;
  if (!active) {
    // Ends once nothing more is to be said (UpdateSpeaking()).
    UpdateSpeaking();
    return;
  }
  if (!looked_for_bundled_) {
    looked_for_bundled_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(&FindVoice),
        base::BindOnce(
            [](base::WeakPtr<VoiceSpeech> self, FoundVoice found) {
              if (!self) {
                return;
              }
              if (!found.paths) {
                LOG(INFO) << "[MoltVoice] the platform's voices speak: "
                          << found.why;
              }
              self->OnBundledFound(std::move(found.paths));
            },
            weak_factory_.GetWeakPtr()));
    return;
  }
  if (bundled_ && !bundled_->started()) {
    bundled_->Start();
  }
}

void VoiceSpeech::OnBundledFound(std::optional<BundledVoice::Paths> paths) {
  if (!paths || bundled_) {
    return;
  }
  bundled_ = std::make_unique<BundledVoice>(this, std::move(*paths));
  if (active_) {
    bundled_->Start();
  }
}

uint64_t VoiceSpeech::Speak(const std::string& text,
                            double rate,
                            bool interrupt) {
  if (text.empty() || !context_) {
    return 0;
  }
  if (interrupt) {
    Stop();
  }
  Pending pending;
  pending.text = text;
  pending.rate = std::clamp(rate, kMinTtsRate, kMaxTtsRate);
  pending.id = next_id_++;
  const uint64_t id = pending.id;
  waiting_.push_back(std::move(pending));
  SpeakNext();
  UpdateSpeaking();
  return id;
}

uint64_t VoiceSpeech::SpeakPrivately(const std::string& text,
                                     const std::string& fallback,
                                     double rate,
                                     bool interrupt) {
  if (text.empty() || !context_) {
    return 0;
  }
  if (interrupt) {
    Stop();
  }
  Pending pending;
  pending.text = text;
  pending.fallback = fallback;
  pending.privately = true;
  pending.rate = std::clamp(rate, kMinTtsRate, kMaxTtsRate);
  pending.id = next_id_++;
  const uint64_t id = pending.id;
  waiting_.push_back(std::move(pending));
  SpeakNext();
  UpdateSpeaking();
  return id;
}

void VoiceSpeech::Stop() {
  for (const Pending& pending : bundled_in_flight_) {
    Done(pending.id, false);
  }
  for (const Pending& pending : waiting_) {
    Done(pending.id, false);
  }
  waiting_.clear();
  bundled_in_flight_.clear();
  ready_wait_timer_.Stop();
  if (bundled_) {
    bundled_->Cancel();
  }
  if (Delegate* delegate = in_flight_.get()) {
    if (delegate->started()) {
      delegate->StopUtterance();  // OnGone() clears |in_flight_|
    } else {
      delegate->Cancel();
    }
  }
  UpdateSpeaking();
}

void VoiceSpeech::SpeakNext() {
  if (waiting_.empty() || !context_) {
    return;
  }
  const bool bundled_running = bundled_ && bundled_->started();
  if (bundled_running && bundled_->ready()) {
    platform_until_ready_ = false;
    ready_wait_timer_.Stop();
    // A platform utterance of ours finishes first: never two voices at
    // once.
    if (in_flight_) {
      return;
    }
    while (!waiting_.empty() && bundled_->unfinished() < kBundledAhead) {
      Pending next = std::move(waiting_.front());
      waiting_.pop_front();
      next.bundled_id = bundled_->Speak(next.text, VoiceRatePercent(next.rate));
      bundled_in_flight_.push_back(std::move(next));
    }
    return;
  }
  if (bundled_running && !platform_until_ready_) {
    // Starting: the answer waits for it a little.
    if (!ready_wait_timer_.IsRunning()) {
      ready_wait_timer_.Start(FROM_HERE, kBundledReadyWait,
                              base::BindOnce(&VoiceSpeech::OnReadyWaitOver,
                                             base::Unretained(this)));
    }
    return;
  }
  // What the bundled voice still plays finishes first.
  if (!bundled_in_flight_.empty()) {
    return;
  }
  SpeakWithPlatform();
}

void VoiceSpeech::OnReadyWaitOver() {
  if (bundled_ && bundled_->ready()) {
    return;
  }
  LOG(INFO) << "[MoltVoice] the voice helper isn't ready yet: the "
               "platform's voice speaks meanwhile";
  platform_until_ready_ = true;
  SpeakNext();
  UpdateSpeaking();
}

void VoiceSpeech::SpeakWithPlatform() {
  // What only the bundled voice may say: the platform's voice says its
  // fallback, or nothing.
  while (!waiting_.empty() && waiting_.front().privately &&
         waiting_.front().fallback.empty()) {
    Done(waiting_.front().id, false);
    waiting_.pop_front();
  }
  if (in_flight_ || waiting_.empty() || !context_) {
    return;
  }
  Pending next = std::move(waiting_.front());
  waiting_.pop_front();
  if (next.privately) {
    next.text = std::move(next.fallback);
    next.privately = false;
  }
  std::unique_ptr<content::TtsUtterance> utterance =
      content::TtsUtterance::Create(context_.get());
  utterance->SetText(next.text);
  utterance->SetLang(kLanguage);
  utterance->SetContinuousParameters(next.rate, /*pitch=*/1.0,
                                     /*volume=*/1.0);
  // Queued after whatever is being said: never clears others' speech.
  utterance->SetShouldClearQueue(false);
  auto delegate = std::make_unique<Delegate>(weak_factory_.GetWeakPtr());
  in_flight_ = delegate.get();
  utterance->SetEventDelegate(std::move(delegate));
  content::TtsController* controller = content::TtsController::GetInstance();
  in_flight_behind_others_ = controller->IsSpeaking();
  in_flight_id_ = next.id;
  // May end it at once (no voice): OnGone() then runs inside.
  controller->SpeakOrEnqueue(std::move(utterance));
}

void VoiceSpeech::OnBundledVoiceReady() {
  SpeakNext();
  UpdateSpeaking();
}

void VoiceSpeech::OnBundledVoiceFinished(uint32_t id) {
  // Finished in order; an id not at the front was dropped by Stop().
  if (!bundled_in_flight_.empty() &&
      bundled_in_flight_.front().bundled_id == id) {
    Done(bundled_in_flight_.front().id, true);
    bundled_in_flight_.pop_front();
  }
  SpeakNext();
  UpdateSpeaking();
}

void VoiceSpeech::OnBundledVoiceLost(std::vector<uint32_t> unfinished,
                                     bool gave_up,
                                     const std::string& why) {
  if (gave_up) {
    LOG(WARNING) << "[MoltVoice] voice mode's own voice stopped working ("
                 << why << "): the platform's voices speak from now on";
  }
  // What it hadn't finished is said again, first, by whichever voice
  // speaks next.
  std::deque<Pending> again;
  for (Pending& pending : bundled_in_flight_) {
    if (std::find(unfinished.begin(), unfinished.end(), pending.bundled_id) !=
        unfinished.end()) {
      pending.bundled_id = 0;
      again.push_back(std::move(pending));
    } else {
      // Not said again: over, as far as voice mode knows.
      Done(pending.id, true);
    }
  }
  bundled_in_flight_.clear();
  for (auto it = again.rbegin(); it != again.rend(); ++it) {
    waiting_.push_front(std::move(*it));
  }
  if (!gave_up) {
    // Restarting: the platform's voice speaks until it is ready again.
    platform_until_ready_ = true;
  }
  SpeakNext();
  UpdateSpeaking();
}

void VoiceSpeech::OnGone(Delegate* delegate) {
  if (delegate != in_flight_) {
    return;
  }
  // Said to its end unless voice mode stopped it.
  Done(in_flight_id_, delegate->ended() && !delegate->cancelled());
  in_flight_ = nullptr;
  in_flight_behind_others_ = false;
  in_flight_id_ = 0;
  // Posted: this runs inside TtsController's own calls.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&VoiceSpeech::SpeakNext, weak_factory_.GetWeakPtr()));
  UpdateSpeaking();
}

void VoiceSpeech::UpdateSpeaking() {
  const bool now = speaking();
  // Voice mode is off and nothing more is to be said: the helper ends.
  if (!active_ && !now && bundled_ && bundled_->started()) {
    bundled_->Stop();
  }
  if (now == was_speaking_) {
    return;
  }
  was_speaking_ = now;
  if (!speaking_changed_) {
    return;
  }
  // Posted: this may run inside TtsController's own calls.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<VoiceSpeech> self) {
                       if (self && self->speaking_changed_) {
                         self->speaking_changed_.Run(self->speaking());
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

}  // namespace voice
}  // namespace molt_ai
