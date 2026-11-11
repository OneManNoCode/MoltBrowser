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
  // The samples are much louder than the quiet before them: speech may
  // have begun, before the detector can tell (it notices speech 0.25 to
  // 0.3 s in). Noise and music set it too; only ever a reason to wait.
  bool sound = false;
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
  // When the spotter fires after the detector ended the speech that held
  // the wake phrase (late, as over speakers), at most this much after that
  // end: the recognizer hears that speech from its start, and the helper
  // finishes it at once (OnWake()).
  static constexpr int64_t kLateWakeSamples = kSampleRate * 3 / 2;
  // Audio held for the pre-rolls: a short command and its wake phrase,
  // with the pre-roll, when the spotter fired kLateWakeSamples after them.
  static constexpr int64_t kRingSamples = 4 * kSampleRate;
  // Audio is processed in steps of at most this much.
  static constexpr size_t kStepSamples = 1600;
  // When the browser asks for the wake phrase again, the spotter starts on
  // this much of the audio before (none an utterance used), so a phrase
  // begun just before isn't cut in half.
  static constexpr int64_t kRespotSamples = kSampleRate * 3 / 2;
  // When the browser asks for the wake phrase again, speech the detector
  // notices within this much audio began while it wasn't needed (the
  // detector hears speech 0.1 s in, a step later at most): it counts
  // without the phrase, from its start.
  static constexpr int64_t kLateOpenSamples = kSampleRate * 3 / 10;
  // When an utterance's speech ends with at most one word after the wake
  // phrase (or one word, without it), the helper waits this much more
  // audio before finishing it, recognizing on: a short pause inside a
  // command ("Hey Molt, select [pause] by letter", or an unvoiced sound
  // the detector takes for silence) is the same command, which a new
  // utterance would hear from its middle. Only short commands wait.
  static constexpr int64_t kShortCommandHangoverSamples = kSampleRate * 4 / 10;
  // Sound in that wait (VadUpdate::sound) keeps it going this much longer,
  // for the detector to notice the speech, and the wait never goes on for
  // more than kMaxHangoverSamples in all.
  static constexpr int64_t kSoundHangoverSamples = kSampleRate * 4 / 10;
  static constexpr int64_t kMaxHangoverSamples = kSampleRate;
  // How long speech is heard for a wake phrase the spotter missed
  // (|State::kHearing|) before the helper gives up on it: the phrase comes
  // first.
  static constexpr int64_t kHearingSamples = 4 * kSampleRate;

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
    // Wake phrase required, someone speaking, and the spotter hasn't fired:
    // it goes on listening, and the recognizer hears the speech too, for
    // the wake phrase the spotter may miss (WakePhraseWordsAtStart()).
    // Nothing is written unless the phrase is there, or the speech began
    // before the phrase was needed (kLateOpenSamples).
    kHearing,
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
  // The spotter fired at sample |at|.
  void OnWake(int64_t at);
  // kHearing: what the recognizer heard so far, or all of it at the end
  // of the speech (|whole|).
  void OnHeard(const RecognizerResult& result, bool whole);
  // A wake phrase the recognizer heard and the spotter didn't: |words| of
  // |result|'s first words.
  void OnHeardWake(const RecognizerResult& result, size_t words, bool whole);
  // Back to waiting for the wake phrase for the rest of this speech.
  void StopHearing();
  // The spotter on the last kRespotSamples of audio no utterance used.
  void RespotRecent();
  void BeginFrom(int64_t from_sample);
  void FinishUtterance();
  void EndUtterance(const RecognizerResult& result);
  void EndWithoutCommand();
  void ToBaseState();
  // Whether |result| has words said after the wake phrase: one starting
  // after the cut, or (|whole|, at the end of the speech) more words than
  // the phrase has.
  bool HasWordsAfterWake(const RecognizerResult& result, bool whole) const;
  // Whether |result| has at most one word after the wake phrase (or at
  // most one word, without one).
  bool ShortCommand(const RecognizerResult& result) const;
  // The short-command hangover, from now.
  void StartHangover();
  // In the hangover: whether it is over, because speech resumed (and
  // |vad.in_speech|; the hangover is then cleared) or its time ran out.
  // Sound keeps it going a little (kSoundHangoverSamples).
  bool HangoverOver(const VadUpdate& vad);
  void EmitPartial(const RecognizerResult& result);
  void ApplyPendingVad();
  EventTime Now() const;
  EventTime At(int64_t sample) const;
  void Emit(const std::string& line);

  // Pre-roll: the last kRingSamples samples.
  void Remember(const std::vector<float>& samples);
  std::vector<float> Recall(int64_t from_sample) const;

  std::unique_ptr<SpeechEngines> engines_;
  Vocabularies vocabularies_;
  EmitFunction emit_;

  SpeechConfig config_;
  // The keyword lines (WakeKeywordLine(): the wake phrases and their
  // threshold) the spotter was last built with; empty before.
  std::vector<std::string> keyword_lines_;
  // The hotwords argument for the next utterance.
  std::string hotwords_;
  // An end-of-speech silence waiting for the end of an utterance; 0 if
  // none.
  int pending_vad_ms_ = 0;
  // The one the detector has; 0 before Start(). Every CONFIG the browser
  // sends carries it: the detector is built again only when it changes.
  int vad_ms_ = 0;
  // kWaitWake: the recognizer gave up on this stretch of speech (it isn't
  // heard again until the detector hears it end).
  bool heard_off_ = false;

  State state_ = State::kWaitWake;
  int64_t epoch_ = 0;
  int64_t received_ = 0;
  bool vad_in_speech_ = false;
  // Where the detector last started to hear speech (minus its lag), or -1.
  int64_t speech_onset_ = -1;
  // The last stretch of speech the detector heard, kept after it ended:
  // where it started (-1 if none), and where the detector ended it (0 if
  // it hasn't).
  int64_t last_onset_ = -1;
  int64_t last_end_ = 0;
  // The sample after the last one an utterance used.
  int64_t consumed_ = 0;
  // The sample after the last one a written final (or a reset) used: a
  // late wake hears nothing before it again (OnWake()).
  int64_t written_ = 0;
  int64_t wake_at_ = 0;
  // kWaitWake: speech the detector notices up to this sample began before
  // the browser asked for the wake phrase again (kLateOpenSamples); -1 if
  // none.
  int64_t open_until_ = -1;
  // kUtterance: its speech ended short (kShortCommandHangoverSamples); it
  // finishes at this sample unless speech resumes. 0 if not waiting.
  int64_t hangover_until_ = 0;
  // The latest it may go on to (kMaxHangoverSamples), and where the
  // speech it waits after ended.
  int64_t hangover_limit_ = 0;
  int64_t speech_ended_at_ = 0;
  // kHearing: where speech begun before the phrase was needed started (it
  // counts without the phrase); -1 if it didn't.
  int64_t hearing_open_from_ = -1;
  // Words in the wake phrase heard last.
  size_t wake_words_ = 0;
  int64_t utterance_from_ = 0;
  std::string last_partial_;

  std::vector<float> ring_;
  size_t ring_next_ = 0;
};

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_LISTENER_H_
