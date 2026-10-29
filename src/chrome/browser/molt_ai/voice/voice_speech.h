// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's spoken answers (docs/design/TOUCHLESS_BROWSING.md §5.4,
// §6.6): through content::TtsController with the platform's voices
// (Windows SAPI and OneCore, macOS AVSpeechSynthesizer, speech-dispatcher
// on Linux), at the user's rate.
//
//   - Interruptible: Speak() with |interrupt| stops whatever is being said
//     before it starts (the user just asked for something); Stop() silences
//     the speech while voice mode is speaking (current and queued, as
//     TtsController::Stop() does), and leaves a page's speech alone
//     otherwise. A new command and "stop" use them.
//   - Utterances belong to the profile (TtsUtterance::Create(context)),
//     not to the tab: a navigation doesn't cut "YouTube is open" short.
//   - The text is what the voice loop decided to say (voice_controller_
//     logic.h): page text only after SpeechSafeText(), and nothing from a
//     credential field.
//
// Platform notes for the build machine: on Linux, Chromium's TTS needs
// libspeechd.so.2 (speech-dispatcher) at run time and the
// --enable-speech-dispatcher switch (TtsPlatformImplLinux); without them
// every utterance ends at once with an error and voice mode is silent
// (the caption still shows what it would say). Windows and macOS need
// nothing extra.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace molt_ai {
namespace voice {

class VoiceSpeech {
 public:
  // The language voice mode speaks.
  static constexpr char kLanguage[] = "en-US";

  explicit VoiceSpeech(content::BrowserContext* context);
  VoiceSpeech(const VoiceSpeech&) = delete;
  VoiceSpeech& operator=(const VoiceSpeech&) = delete;
  ~VoiceSpeech();

  // Says |text| at |rate| (1 is the voice's normal rate, clamped to
  // 0.1-10), after what voice mode is saying, or at once with |interrupt|.
  void Speak(const std::string& text, double rate, bool interrupt);

  // Stops the speech if voice mode is speaking.
  void Stop();

  // Whether voice mode's speech is queued or being spoken.
  bool speaking() const { return live_ > 0; }

  // Runs (posted) when voice mode starts or stops speaking.
  void SetSpeakingChangedCallback(base::RepeatingCallback<void(bool)> callback);

 private:
  class Delegate;

  void Finished();
  void NotifySpeakingChanged();

  raw_ptr<content::BrowserContext> context_;
  // Utterances queued or being spoken: each one's delegate counts itself
  // out when its utterance ends or is dropped.
  int live_ = 0;
  base::RepeatingCallback<void(bool)> speaking_changed_;
  base::WeakPtrFactory<VoiceSpeech> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_H_
