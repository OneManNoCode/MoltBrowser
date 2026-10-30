// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-speech's listening states (docs/design/SPEECH_HELPER_PROTOCOL.md
// §4): when the wake phrase counts, when an utterance starts and ends, and
// which events that writes. The engines (spotter, voice activity detector,
// recognizer) sit behind SpeechEngines, so the logic is std-only and is
// tested with fake engines; sherpa_engines.h implements them with
// sherpa-onnx.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_LISTENER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_LISTENER_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/speech_helper/speech_config.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_events.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_text.h"

namespace molt_speech {

inline constexpr int kSampleRate = 16000;

// What the voice activity detector saw in some samples.
struct VadUpdate {
  // Speech at the end of the samples.
  bool in_speech = false;
  // A stretch of speech ended within them (|vad_silence_ms| of silence
  // after it, or it reached the detector's 20 s limit).
  bool segment_ended = false;
};

struct RecognizerResult {
  RecognizerResult();
  RecognizerResult(const RecognizerResult&);
  RecognizerResult& operator=(const RecognizerResult&);
  ~RecognizerResult();

  std::string text;
  // When each word starts (its first token), in seconds from the first
  // sample given to the utterance.
  std::vector<float> word_seconds;
  // 0 to 1.
  double confidence = 0;
};

class SpeechEngines {
 public:
  virtual ~SpeechEngines();

  // Replaces the spotter's keywords (WakeKeywordLine() lines). False if the
  // spotter couldn't be built with them.
  virtual bool SetKeywords(const std::vector<std::string>& keyword_lines) = 0;
  // True if a keyword ended within |samples|, with how many words it has
  // in |words|; the spotter then starts afresh.
  virtual bool Spot(const std::vector<float>& samples, size_t* words) = 0;
  virtual void ResetSpotter() = 0;

  virtual void SetVadSilenceMs(int ms) = 0;
  virtual VadUpdate Detect(const std::vector<float>& samples) = 0;
  virtual void ResetVad() = 0;

  // A new utterance for the recognizer, biased toward |hotwords| (a
  // HotwordsArgument(), may be empty).
  virtual void BeginUtterance(const std::string& hotwords) = 0;
  virtual void Recognize(const std::vector<float>& samples) = 0;
  // The hypothesis so far.
  virtual RecognizerResult Partial() = 0;
  // Completes the utterance (the model's last chunk padded with silence)
  // and returns what it heard.
  virtual RecognizerResult Finish() = 0;
  virtual void AbandonUtterance() = 0;
};

// The models' vocabularies the listener needs to turn CONFIG text into
// engine input.
struct Vocabularies {
  Vocabularies();
  Vocabularies(const Vocabularies&);
  Vocabularies& operator=(const Vocabularies&);
  ~Vocabularies();

  PieceVocab kws_pieces;
  TokenSet kws_tokens;
  PieceVocab asr_pieces;
  TokenSet asr_tokens;
};

class Listener {
 public:
  // Called with each event line, in order.
  using EmitFunction = std::function<void(const std::string& line)>;

  // Timing constants, in samples (docs/design/SPEECH_HELPER_PROTOCOL.md
  // §4).
  // The wait for a command after a wake phrase.
  static constexpr int64_t kWakeWindowSamples = 6 * kSampleRate;
  // The longest utterance.
  static constexpr int64_t kMaxUtteranceSamples = 20 * kSampleRate;
  // Audio before the detector noticed speech that goes to the recognizer.
  static constexpr int64_t kPrerollSamples = kSampleRate * 6 / 10;
  // How far back from a wake phrase's detection the recognizer starts
  // (the phrase itself is recognized too; the grammar drops it).
  static constexpr int64_t kWakePrerollSamples = 2 * kSampleRate;
  // A recognized word counts as said after the wake phrase if it starts
  // later than this before the detection: the spotter fires 0.2 to 0.3 s
  // after the phrase ends (docs/VOICE_BENCH.md §3.8), and the phrase's
  // last word started earlier than that.
  static constexpr int64_t kCommandCutSamples = kSampleRate * 3 / 10;
  // Audio held for the pre-rolls.
  static constexpr int64_t kRingSamples = kSampleRate * 5 / 2;
  // Audio is processed in steps of at most this much.
  static constexpr size_t kStepSamples = 1600;

  enum class ConfigResult { kOk, kConfigError, kEngineError };

  Listener(std::unique_ptr<SpeechEngines> engines,
           Vocabularies vocabularies,
           EmitFunction emit);
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  ~Listener();

  // Sets the engines up with DefaultSpeechConfig(). False with |error| if
  // the default wake phrase can't be spelled or the spotter can't be built.
  bool Start(std::string* error);

  ConfigResult ApplyConfig(const ConfigUpdate& update, std::string* error);
  void Reset();
  // 16 kHz mono signed 16-bit samples.
  void Feed(const int16_t* samples, size_t count);

  const SpeechConfig& config() const { return config_; }
  int64_t epoch() const { return epoch_; }
  int64_t received_samples() const { return received_; }

 private:
  enum class State {
    // Wake phrase required; waiting for it.
    kWaitWake,
    // Every utterance counts; waiting for speech.
    kOpen,
    // A wake phrase was heard; the recognizer hears it and what follows,
    // and no word after it has been recognized yet.
    kAfterWake,
    // A wake phrase was heard and its speech ended with nothing after it;
    // waiting for the command to start.
    kAwaitCommand,
    // speech_start was written; recognizing.
    kUtterance,
  };

  void Step(const std::vector<float>& samples);
  void OnWake();
  void BeginFrom(int64_t from_sample);
  void FinishUtterance();
  void EndUtterance(const RecognizerResult& result);
  void EndWithoutCommand();
  void ToBaseState();
  // Whether |result| has words said after the wake phrase: one starting
  // after the cut, or (|whole|, at the end of the speech) more words than
  // the phrase has.
  bool HasWordsAfterWake(const RecognizerResult& result, bool whole) const;
  void EmitPartial(const RecognizerResult& result);
  void ApplyPendingVad();
  EventTime Now() const;
  void Emit(const std::string& line);

  // Pre-roll: the last kRingSamples samples.
  void Remember(const std::vector<float>& samples);
  std::vector<float> Recall(int64_t from_sample) const;

  std::unique_ptr<SpeechEngines> engines_;
  Vocabularies vocabularies_;
  EmitFunction emit_;

  SpeechConfig config_;
  // The hotwords argument for the next utterance.
  std::string hotwords_;
  // An end-of-speech silence waiting for the end of an utterance; 0 if
  // none.
  int pending_vad_ms_ = 0;

  State state_ = State::kWaitWake;
  int64_t epoch_ = 0;
  int64_t received_ = 0;
  bool vad_in_speech_ = false;
  // Where the detector last started to hear speech (minus its lag), or -1.
  int64_t speech_onset_ = -1;
  // The sample after the last one an utterance used.
  int64_t consumed_ = 0;
  int64_t wake_at_ = 0;
  // Words in the wake phrase heard last.
  size_t wake_words_ = 0;
  int64_t utterance_from_ = 0;
  std::string last_partial_;

  std::vector<float> ring_;
  size_t ring_next_ = 0;
};

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_LISTENER_H_
