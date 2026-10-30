// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_speech.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_utterance.h"

namespace molt_ai {
namespace voice {

namespace {

// TtsUtterance's rate range ("0.1 to 10", as the Web Speech API's).
constexpr double kMinTtsRate = 0.1;
constexpr double kMaxTtsRate = 10.0;

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
  bool cancelled_ = false;
  base::WeakPtrFactory<Delegate> weak_factory_{this};
};

VoiceSpeech::VoiceSpeech(content::BrowserContext* context)
    : context_(context) {}

VoiceSpeech::~VoiceSpeech() {
  Stop();
  // An utterance still waiting in TtsController's queue is stopped as it
  // starts (its delegate does that without us).
  weak_factory_.InvalidateWeakPtrs();
}

void VoiceSpeech::SetSpeakingChangedCallback(
    base::RepeatingCallback<void(bool)> callback) {
  speaking_changed_ = std::move(callback);
}

bool VoiceSpeech::speaking() const {
  return !waiting_.empty() || (in_flight_ && !in_flight_->cancelled());
}

bool VoiceSpeech::OthersSpeaking() const {
  // Ours is being spoken (it started, or nothing else was when it was
  // handed over).
  if (in_flight_ && (in_flight_->started() || !in_flight_behind_others_)) {
    return false;
  }
  return content::TtsController::GetInstance()->IsSpeaking();
}

void VoiceSpeech::Speak(const std::string& text, double rate, bool interrupt) {
  if (text.empty() || !context_) {
    return;
  }
  if (interrupt) {
    Stop();
  }
  waiting_.push_back(Pending{text, std::clamp(rate, kMinTtsRate, kMaxTtsRate)});
  SpeakNext();
  UpdateSpeaking();
}

void VoiceSpeech::Stop() {
  waiting_.clear();
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
  if (in_flight_ || waiting_.empty() || !context_) {
    return;
  }
  Pending next = std::move(waiting_.front());
  waiting_.pop_front();
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
  // May end it at once (no voice): OnGone() then runs inside.
  controller->SpeakOrEnqueue(std::move(utterance));
}

void VoiceSpeech::OnGone(Delegate* delegate) {
  if (delegate != in_flight_) {
    return;
  }
  in_flight_ = nullptr;
  in_flight_behind_others_ = false;
  // Posted: this runs inside TtsController's own calls.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&VoiceSpeech::SpeakNext, weak_factory_.GetWeakPtr()));
  UpdateSpeaking();
}

void VoiceSpeech::UpdateSpeaking() {
  const bool now = speaking();
  if (now == was_speaking_) {
    return;
  }
  was_speaking_ = now;
  if (!speaking_changed_) {
    return;
  }
  // Posted: this may run inside TtsController's own calls.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<VoiceSpeech> self) {
            if (self && self->speaking_changed_) {
              self->speaking_changed_.Run(self->speaking());
            }
          },
          weak_factory_.GetWeakPtr()));
}

}  // namespace voice
}  // namespace molt_ai
