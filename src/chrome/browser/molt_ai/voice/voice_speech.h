// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's spoken answers (docs/design/TOUCHLESS_BROWSING.md §5.4,
// §6.6): through content::TtsController with the platform's voices
// (Windows SAPI and OneCore, macOS AVSpeechSynthesizer, speech-dispatcher
// on Linux), at the user's rate.
//
//   - Interruptible: Speak() with |interrupt| stops what voice mode is
//     saying before it starts (the user just asked for something); Stop()
//     silences voice mode. A new command, barge-in and "stop" use them.
//   - Only voice mode's own speech. TtsController has one queue for the
//     whole browser: pages' speechSynthesis, extensions and voice mode.
//     Voice mode never clears it (no SetShouldClearQueue(true), no
//     TtsController::Stop()), so a page's speech is never cut short by it,
//     which would tell the page that voice mode is speaking and when.
//     Voice mode's answers wait in the queue behind a page's speech
//     instead. VoiceSpeech keeps its own queue and gives TtsController one
//     utterance at a time; it stops that one with
//     RemoveUtteranceEventDelegate(), which stops the utterance being
//     spoken and leaves the queue alone, once the utterance has started
//     (removing one still waiting in TtsController's queue would delete it
//     unfinished); one stopped before it starts is stopped as it starts.
//     (156's TtsController::Stop(source_url) wouldn't do: it clears the
//     whole queue once it has stopped the matching utterance, and does
//     nothing while another source's utterance is being spoken.)
//   - OthersSpeaking(): the speech engine is speaking for someone else (a
//     page's speechSynthesis, an extension). That speech plays outside
//     Chromium's audio service, so the echo canceller can't remove it from
//     what the microphone hears; voice_session.h treats it as possibly
//     hostile (a page saying "Hey Molt, ...").
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

#include <deque>
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
  // 0.1-10), after what voice mode is saying, or instead of it with
  // |interrupt|.
  void Speak(const std::string& text, double rate, bool interrupt);

  // Stops voice mode's speech: what it is saying and what waits. Other
  // speech is left alone.
  void Stop();

  // Whether voice mode's speech is waiting or being spoken.
  bool speaking() const;

  // Whether the speech engine is speaking for someone else now.
  bool OthersSpeaking() const;

  // Runs (posted) when voice mode starts or stops speaking.
  void SetSpeakingChangedCallback(base::RepeatingCallback<void(bool)> callback);

 private:
  class Delegate;

  struct Pending {
    std::string text;
    double rate = 1.0;
  };

  // Gives TtsController the next utterance, if none of ours is there.
  void SpeakNext();
  // |delegate|'s utterance ended, was stopped or was dropped.
  void OnGone(Delegate* delegate);
  void UpdateSpeaking();

  raw_ptr<content::BrowserContext> context_;
  // Waiting for the utterance in TtsController to end.
  std::deque<Pending> waiting_;
  // The delegate of the one utterance of ours TtsController holds (owned
  // by the utterance), if any, and whether something else was being spoken
  // when it was handed over (it then waits in TtsController's queue).
  raw_ptr<Delegate> in_flight_ = nullptr;
  bool in_flight_behind_others_ = false;
  bool was_speaking_ = false;
  base::RepeatingCallback<void(bool)> speaking_changed_;
  base::WeakPtrFactory<VoiceSpeech> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_H_
