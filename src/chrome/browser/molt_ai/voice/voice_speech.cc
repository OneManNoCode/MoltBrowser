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

// Owned by its utterance; counts the utterance out when it goes, however
// it ends (spoken, stopped, dropped from the queue, never spoken).
class VoiceSpeech::Delegate : public content::UtteranceEventDelegate {
 public:
  explicit Delegate(base::WeakPtr<VoiceSpeech> owner)
      : owner_(std::move(owner)) {}
  Delegate(const Delegate&) = delete;
  Delegate& operator=(const Delegate&) = delete;
  ~Delegate() override {
    if (owner_) {
      owner_->Finished();
    }
  }

  // content::UtteranceEventDelegate:
  void OnTtsEvent(content::TtsUtterance* utterance,
                  content::TtsEventType event_type,
                  int char_index,
                  int length,
                  const std::string& error_message) override {}

 private:
  base::WeakPtr<VoiceSpeech> owner_;
};

VoiceSpeech::VoiceSpeech(content::BrowserContext* context)
    : context_(context) {}

VoiceSpeech::~VoiceSpeech() {
  Stop();
  // Utterances still queued count out into nothing.
  weak_factory_.InvalidateWeakPtrs();
}

void VoiceSpeech::SetSpeakingChangedCallback(
    base::RepeatingCallback<void(bool)> callback) {
  speaking_changed_ = std::move(callback);
}

void VoiceSpeech::Speak(const std::string& text, double rate, bool interrupt) {
  if (text.empty() || !context_) {
    return;
  }
  std::unique_ptr<content::TtsUtterance> utterance =
      content::TtsUtterance::Create(context_.get());
  utterance->SetText(text);
  utterance->SetLang(kLanguage);
  utterance->SetContinuousParameters(std::clamp(rate, kMinTtsRate, kMaxTtsRate),
                                     /*pitch=*/1.0, /*volume=*/1.0);
  utterance->SetShouldClearQueue(interrupt);
  if (live_++ == 0) {
    NotifySpeakingChanged();
  }
  utterance->SetEventDelegate(
      std::make_unique<Delegate>(weak_factory_.GetWeakPtr()));
  content::TtsController::GetInstance()->SpeakOrEnqueue(std::move(utterance));
}

void VoiceSpeech::Stop() {
  if (!speaking()) {
    return;
  }
  // Stops the current utterance and drops the queue, finishing each with
  // an event; their delegates count them out.
  content::TtsController::GetInstance()->Stop();
}

void VoiceSpeech::Finished() {
  if (live_ > 0 && --live_ == 0) {
    NotifySpeakingChanged();
  }
}

void VoiceSpeech::NotifySpeakingChanged() {
  if (!speaking_changed_) {
    return;
  }
  // Posted: this runs inside TtsController's own calls.
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
