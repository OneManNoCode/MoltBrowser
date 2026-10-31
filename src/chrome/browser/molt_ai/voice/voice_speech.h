// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's spoken answers (docs/design/TOUCHLESS_BROWSING.md §5.4,
// §6.6), at the user's rate, through MoltBrowser's own voice when it is
// installed and the platform's voices otherwise.
//
//   - The bundled voice (voice_speech_bundled.h): the `molt-voice` helper,
//     a Piper voice through sherpa-onnx, the same on every OS, played by
//     the browser through the audio service. It runs while voice mode is
//     on in the window (SetActive()). Its audio is in the echo canceller's
//     reference, so the user can interrupt it by voice
//     (InEchoReference(), voice_session_logic.h). It has its own queue: a
//     page's speechSynthesis can neither pause it nor be cut short by it.
//   - The platform's voices (content::TtsController: Windows SAPI and
//     OneCore, macOS AVSpeechSynthesizer, speech-dispatcher on Linux):
//     when the bundled voice isn't installed, while its helper is still
//     starting (after kBundledReadyWait), and for good once it has failed
//     BundledVoice::kMaxFailures times in a row. What it hadn't finished
//     saying when it failed is said again by the platform's voice.
//   - Interruptible: Speak() with |interrupt| stops what voice mode is
//     saying before it starts (the user just asked for something); Stop()
//     silences voice mode. A new command, barge-in and "stop" use them.
//   - Each utterance has an id (Speak()'s result), and the utterance-done
//     callback says when each one was said to its end or dropped
//     unfinished: reading a page aloud (voice_reader.h) keeps its place and
//     hands over the next chunk with it.
//   - Only voice mode's own speech. TtsController has one queue for the
//     whole browser: pages' speechSynthesis, extensions and voice mode.
//     Voice mode never clears it (no SetShouldClearQueue(true), no
//     TtsController::Stop()), so a page's speech is never cut short by it,
//     which would tell the page that voice mode is speaking and when.
//     Voice mode's platform answers wait in the queue behind a page's
//     speech instead. VoiceSpeech keeps its own queue and gives
//     TtsController one utterance at a time; it stops that one with
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
// and without the bundled voice every utterance ends at once with an error
// and voice mode is silent (the caption still shows what it would say).
// Windows and macOS need nothing extra.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_speech_bundled.h"

namespace content {
class BrowserContext;
}  // namespace content

namespace molt_ai {
namespace voice {

class VoiceSpeech : public BundledVoice::Delegate {
 public:
  // The language voice mode speaks.
  static constexpr char kLanguage[] = "en-US";
  // How long an answer waits for the bundled voice's helper to get ready
  // before the platform's voice says it.
  static constexpr base::TimeDelta kBundledReadyWait = base::Seconds(3);
  // Answers handed to the bundled voice ahead of the one playing (so the
  // next is synthesized while the last plays).
  static constexpr size_t kBundledAhead = 2;

  explicit VoiceSpeech(content::BrowserContext* context);
  VoiceSpeech(const VoiceSpeech&) = delete;
  VoiceSpeech& operator=(const VoiceSpeech&) = delete;
  ~VoiceSpeech() override;

  // Says |text| at |rate| (1 is the voice's normal rate, clamped to
  // 0.1-10; the bundled voice takes 0.5-2.5), after what voice mode is
  // saying, or instead of it with |interrupt|. Returns the utterance's id
  // (never 0), or 0 when nothing is to be said.
  uint64_t Speak(const std::string& text, double rate, bool interrupt);

  // As Speak(), but |text| is said only by the bundled voice, whose audio
  // is in the echo canceller's reference (it holds the voice gesture's
  // confirmation word, voice_gesture_channel.h). Wherever the platform's
  // voice would say it instead (the bundled voice isn't ready, or failed
  // before it finished), that voice says |fallback| (the same without the
  // word); nothing, when |fallback| is empty.
  uint64_t SpeakPrivately(const std::string& text,
                          const std::string& fallback,
                          double rate,
                          bool interrupt);

  // Whether the bundled voice can speak now (its helper started and is
  // ready): what SpeakPrivately() says would be said by it.
  bool CanSpeakPrivately() const;

  // Stops voice mode's speech: what it is saying and what waits. Other
  // speech is left alone.
  void Stop();

  // Whether voice mode's speech is waiting or being spoken.
  bool speaking() const;

  // Whether the speech engine is speaking for someone else now.
  bool OthersSpeaking() const;

  // Voice mode is on in the window (true) or off: the bundled voice's
  // helper starts, or ends once nothing more is to be said. Never called
  // for a MoltNet window (VoiceController::Start() refuses those).
  void SetActive(bool active);

  // Whether voice mode speaks through the bundled voice now (its helper is
  // ready and no platform utterance of voice mode's is in flight): its
  // audio is browser output, in the echo canceller's reference.
  bool InEchoReference() const;

  // The language voice mode speaks (voice_language.h). English speaks as
  // always: the bundled voice when it is installed, else the platform's,
  // tagged kLanguage. Another language speaks through a platform voice of
  // that language on this computer (PickPlatformVoice(); |ui_locale|'s
  // region first), as the bundled voice reads English only; without one,
  // the platform's default voice reads it, and the log says so once. A
  // change stops what is being said.
  void SetLanguage(Language language, const std::string& ui_locale);

  // Whether this computer has a platform voice for |language| (English:
  // always true, the bundled voice or the platform's default).
  static bool HasPlatformVoice(content::BrowserContext* context,
                               Language language);

  // Runs (posted) when voice mode starts or stops speaking.
  void SetSpeakingChangedCallback(base::RepeatingCallback<void(bool)> callback);

  // Runs (posted) for each utterance Speak() took, once: |spoken| when it
  // was said to its end (or the voice reported it over), false when it was
  // stopped or dropped before that.
  using UtteranceDoneCallback =
      base::RepeatingCallback<void(uint64_t id, bool spoken)>;
  void SetUtteranceDoneCallback(UtteranceDoneCallback callback);

  // BundledVoice::Delegate:
  void OnBundledVoiceReady() override;
  void OnBundledVoiceFinished(uint32_t id) override;
  void OnBundledVoiceLost(std::vector<uint32_t> unfinished,
                          bool gave_up,
                          const std::string& why) override;

  // Tests: the bundled voice, once found (null before, or without one).
  BundledVoice* bundled_for_testing() { return bundled_.get(); }

 private:
  class Delegate;

  struct Pending {
    std::string text;
    double rate = 1.0;
    // The bundled voice's id, once handed to it.
    uint32_t bundled_id = 0;
    // Speak()'s id.
    uint64_t id = 0;
    // SpeakPrivately(): only the bundled voice says |text|; the platform's
    // says |fallback|.
    bool privately = false;
    std::string fallback;
  };

  void OnBundledFound(std::optional<BundledVoice::Paths> paths);
  // Hands the next answers to the bundled voice or TtsController.
  void SpeakNext();
  void SpeakWithPlatform();
  void OnReadyWaitOver();
  // |delegate|'s utterance ended, was stopped or was dropped.
  void OnGone(Delegate* delegate);
  void UpdateSpeaking();
  // Tells the utterance-done callback (posted).
  void Done(uint64_t id, bool spoken);
  // Whether the bundled voice speaks the language (English only).
  bool BundledSpeaks() const { return language_ == Language::kEnglish; }

  raw_ptr<content::BrowserContext> context_;
  // Not yet handed to either voice.
  std::deque<Pending> waiting_;
  // Handed to the bundled voice and not finished, oldest first.
  std::deque<Pending> bundled_in_flight_;
  // The delegate of the one utterance of ours TtsController holds (owned
  // by the utterance), if any, and whether something else was being spoken
  // when it was handed over (it then waits in TtsController's queue).
  raw_ptr<Delegate> in_flight_ = nullptr;
  bool in_flight_behind_others_ = false;
  // Its utterance's id.
  uint64_t in_flight_id_ = 0;
  uint64_t next_id_ = 1;
  bool was_speaking_ = false;

  std::unique_ptr<BundledVoice> bundled_;
  // FindBundled() is running, or has run.
  bool looked_for_bundled_ = false;
  bool active_ = false;
  // The bundled voice's helper is starting and an answer waited
  // kBundledReadyWait: the platform's voice speaks until it is ready.
  bool platform_until_ready_ = false;
  base::OneShotTimer ready_wait_timer_;

  base::RepeatingCallback<void(bool)> speaking_changed_;
  UtteranceDoneCallback utterance_done_;
  // SetLanguage(): the language, its platform tag and the browser's own
  // language; whether the log said there is no voice for it.
  Language language_ = Language::kEnglish;
  std::string language_tag_ = kLanguage;
  std::string ui_locale_;
  bool said_no_voice_ = false;
  base::WeakPtrFactory<VoiceSpeech> weak_factory_{this};
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SPEECH_H_
