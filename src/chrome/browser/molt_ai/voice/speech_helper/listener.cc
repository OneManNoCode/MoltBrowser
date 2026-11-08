// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/listener.h"

#include <stddef.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace molt_speech {

namespace {

// Words with a letter or digit ("Hey, Molt." has two).
size_t CountWords(const std::string& text) {
  size_t words = 0;
  bool in_word = false;
  bool counted = false;
  for (const char c : text) {
    if (c == ' ') {
      in_word = false;
      continue;
    }
    if (!in_word) {
      in_word = true;
      counted = false;
    }
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') ||
                       (static_cast<unsigned char>(c) >= 0x80);
    if (alnum && !counted) {
      ++words;
      counted = true;
    }
  }
  return words;
}

}  // namespace

RecognizerResult::RecognizerResult() = default;
RecognizerResult::RecognizerResult(const RecognizerResult&) = default;
RecognizerResult& RecognizerResult::operator=(const RecognizerResult&) =
    default;
RecognizerResult::~RecognizerResult() = default;

SpeechEngines::~SpeechEngines() = default;

Vocabularies::Vocabularies() = default;
Vocabularies::Vocabularies(const Vocabularies&) = default;
Vocabularies& Vocabularies::operator=(const Vocabularies&) = default;
Vocabularies::~Vocabularies() = default;

Listener::Listener(std::unique_ptr<SpeechEngines> engines,
                   Vocabularies vocabularies,
                   EmitFunction emit)
    : engines_(std::move(engines)),
      vocabularies_(std::move(vocabularies)),
      emit_(std::move(emit)),
      config_(DefaultSpeechConfig()),
      ring_(static_cast<size_t>(kRingSamples), 0.0f) {}

Listener::~Listener() = default;

bool Listener::Start(std::string* error) {
  ConfigUpdate defaults;
  defaults.has_wake_phrases = true;
  defaults.values.wake_phrases = config_.wake_phrases;
  defaults.has_vad_silence_ms = true;
  defaults.values.vad_silence_ms = config_.vad_silence_ms;
  state_ = config_.wake_required ? State::kWaitWake : State::kOpen;
  return ApplyConfig(defaults, error) == ConfigResult::kOk;
}

Listener::ConfigResult Listener::ApplyConfig(const ConfigUpdate& update,
                                             std::string* error) {
  if (update.has_wake_phrases) {
    std::vector<std::string> lines;
    for (const std::string& phrase : update.values.wake_phrases) {
      if (!WakeKeywordLines(phrase, vocabularies_.kws_pieces,
                            vocabularies_.kws_tokens, &lines)) {
        if (error) {
          *error =
              "CONFIG: a wake phrase has a word the spotter can't spell";
        }
        return ConfigResult::kConfigError;
      }
    }
    // Every CONFIG the browser sends carries the wake phrases: the spotter
    // (and its model) is rebuilt only when they change.
    if (lines != keyword_lines_) {
      keyword_lines_.clear();
      if (!engines_->SetKeywords(lines)) {
        if (error) {
          *error = "the spotter could not take the wake phrases";
        }
        return ConfigResult::kEngineError;
      }
      keyword_lines_ = std::move(lines);
    }
    config_.wake_phrases = update.values.wake_phrases;
  }
  if (update.has_hotwords) {
    config_.hotwords = update.values.hotwords;
    hotwords_ = HotwordsArgument(config_.hotwords, vocabularies_.asr_pieces,
                                 vocabularies_.asr_tokens);
  }
  if (update.has_vad_silence_ms) {
    config_.vad_silence_ms = update.values.vad_silence_ms;
    // Every CONFIG carries it: a new detector, which forgets the speech it
    // was following, only when it changes.
    pending_vad_ms_ = update.values.vad_silence_ms == vad_ms_
                          ? 0
                          : update.values.vad_silence_ms;
    if (state_ == State::kWaitWake || state_ == State::kOpen) {
      ApplyPendingVad();
    }
  }
  if (update.has_wake_required) {
    config_.wake_required = update.values.wake_required;
    if (state_ == State::kHearing && !config_.wake_required) {
      // Speech heard while the wake phrase was needed, and now it isn't:
      // the utterance counts from its start.
      Emit(SpeechStartEvent(Now()));
      state_ = State::kUtterance;
    } else if (state_ == State::kWaitWake && !config_.wake_required) {
      state_ = State::kOpen;
    } else if (state_ == State::kOpen && config_.wake_required) {
      state_ = State::kWaitWake;
      // Speech already begun that the detector hasn't noticed yet.
      open_until_ = vad_in_speech_ ? -1 : received_ + kLateOpenSamples;
      engines_->ResetSpotter();
      RespotRecent();
    }
  }
  return ConfigResult::kOk;
}

void Listener::Reset() {
  if (state_ == State::kAfterWake || state_ == State::kUtterance ||
      state_ == State::kHearing) {
    engines_->AbandonUtterance();
  }
  heard_off_ = false;
  engines_->ResetVad();
  vad_in_speech_ = false;
  speech_onset_ = -1;
  consumed_ = received_;
  ++epoch_;
  ToBaseState();
}

void Listener::Feed(const int16_t* samples, size_t count) {
  std::vector<float> step;
  for (size_t offset = 0; offset < count; offset += kStepSamples) {
    const size_t n = std::min(kStepSamples, count - offset);
    step.resize(n);
    for (size_t i = 0; i < n; ++i) {
      step[i] = static_cast<float>(samples[offset + i]) / 32768.0f;
    }
    Step(step);
  }
}

void Listener::Step(const std::vector<float>& samples) {
  Remember(samples);
  const int64_t step_start = received_;
  received_ += static_cast<int64_t>(samples.size());

  const VadUpdate vad = engines_->Detect(samples);
  if (vad.in_speech && !vad_in_speech_) {
    speech_onset_ = step_start;
  } else if (!vad.in_speech) {
    speech_onset_ = -1;
  }
  vad_in_speech_ = vad.in_speech;

  switch (state_) {
    case State::kWaitWake:
      if (engines_->Spot(samples, &wake_words_)) {
        OnWake(received_);
        return;
      }
      if (!vad.in_speech) {
        heard_off_ = false;
      } else if (!heard_off_) {
        const int64_t onset = speech_onset_ >= 0 ? speech_onset_ : step_start;
        // Begun as the browser asked for the wake phrase again, before the
        // detector noticed it: while every utterance counted
        // (kLateOpenSamples). Heard for a wake phrase as usual; without
        // one it still counts.
        hearing_open_from_ = onset <= open_until_ ? onset : -1;
        open_until_ = -1;
        // Heard as well, in case the spotter misses the phrase.
        BeginFrom(onset - kPrerollSamples);
        state_ = State::kHearing;
      }
      return;
    case State::kHearing:
      if (engines_->Spot(samples, &wake_words_)) {
        // The spotter heard it after all: as usual (the recognizer starts
        // again from before the phrase).
        engines_->AbandonUtterance();
        OnWake(received_);
        return;
      }
      engines_->Recognize(samples);
      if (hangover_until_ > 0) {
        if (!HangoverOver(vad)) {
          return;
        }
        if (!vad.in_speech) {
          OnHeard(engines_->Finish(), /*whole=*/true);
          return;
        }
      }
      if (vad.segment_ended &&
          received_ - utterance_from_ < kMaxUtteranceSamples) {
        // The phrase and one word, and a pause: the command may go on
        // (kShortCommandHangoverSamples).
        const RecognizerResult partial = engines_->Partial();
        const size_t words =
            WakePhraseWordsAtStart(partial.text, config_.wake_phrases);
        const size_t heard = CountWords(partial.text);
        if (words > 0 && heard == words + 1) {
          OnHeardWake(partial, words, /*whole=*/false);
          StartHangover();
          return;
        }
        // Only the phrase, or the phrase begun, written so far: the
        // recognizer writes a word a moment after it is said, so a word
        // and a pause may follow it ("Hey, Malt, click" ... "sport").
        if ((words > 0 && heard == words) ||
            OnlyAWakePhraseBegun(partial.text, config_.wake_phrases)) {
          StartHangover();
          return;
        }
      }
      if (vad.segment_ended ||
          received_ - utterance_from_ >= kMaxUtteranceSamples) {
        OnHeard(engines_->Finish(), /*whole=*/true);
      } else {
        OnHeard(engines_->Partial(), /*whole=*/false);
      }
      return;
    case State::kOpen:
      if (vad.in_speech) {
        Emit(SpeechStartEvent(Now()));
        const int64_t onset = speech_onset_ >= 0 ? speech_onset_ : step_start;
        BeginFrom(onset - kPrerollSamples);
        state_ = State::kUtterance;
      }
      return;
    case State::kAfterWake: {
      engines_->Recognize(samples);
      const RecognizerResult result = engines_->Partial();
      if (HasWordsAfterWake(result, /*whole=*/false)) {
        Emit(SpeechStartEvent(Now()));
        state_ = State::kUtterance;
        EmitPartial(result);
      }
      if (state_ == State::kUtterance) {
        // A wait begun below goes on as the utterance's own.
        if (hangover_until_ == 0 && vad.segment_ended) {
          if (ShortCommand(result)) {
            StartHangover();
          } else {
            FinishUtterance();
          }
        }
        return;
      }
      if (hangover_until_ == 0) {
        if (vad.segment_ended) {
          // The speech that held the wake phrase ended with no word after
          // it written yet: the recognizer writes a word a moment after it
          // is said, so a word and a pause may follow ("Hey Molt, click"
          // ... "sport"). Waited out as a short command's.
          StartHangover();
        } else if (received_ - wake_at_ >= kWakeWindowSamples) {
          EndWithoutCommand();
        }
        return;
      }
      if (!HangoverOver(vad) || vad.in_speech) {
        return;
      }
      {
        // The speech that held the wake phrase ended. Was there more?
        const RecognizerResult whole = engines_->Finish();
        consumed_ = received_;
        last_partial_.clear();
        if (HasWordsAfterWake(whole, /*whole=*/true)) {
          Emit(SpeechStartEvent(Now()));
          EndUtterance(whole);
        } else {
          // Only the wake phrase: wait for the command.
          state_ = State::kAwaitCommand;
        }
      }
      return;
    }
    case State::kAwaitCommand:
      if (vad.in_speech) {
        Emit(SpeechStartEvent(Now()));
        const int64_t onset = speech_onset_ >= 0 ? speech_onset_ : step_start;
        BeginFrom(onset - kPrerollSamples);
        state_ = State::kUtterance;
      } else if (received_ - wake_at_ >= kWakeWindowSamples) {
        EndWithoutCommand();
      }
      return;
    case State::kUtterance: {
      engines_->Recognize(samples);
      const RecognizerResult partial = engines_->Partial();
      EmitPartial(partial);
      if (received_ - utterance_from_ >= kMaxUtteranceSamples) {
        FinishUtterance();
        return;
      }
      if (hangover_until_ > 0) {
        if (HangoverOver(vad) && !vad.in_speech) {
          FinishUtterance();
        }
        return;
      }
      if (vad.segment_ended) {
        if (ShortCommand(partial)) {
          StartHangover();
          return;
        }
        FinishUtterance();
      }
      return;
    }
  }
}

void Listener::OnWake(int64_t at) {
  hearing_open_from_ = -1;
  hangover_until_ = 0;
  wake_at_ = at;
  Emit(WakeEvent(At(at)));
  int64_t from = wake_at_ - kWakePrerollSamples;
  if (vad_in_speech_ && speech_onset_ >= 0) {
    // Not the speech before this stretch of it.
    from = std::max(from, speech_onset_ - kPrerollSamples);
  }
  BeginFrom(from);
  state_ = State::kAfterWake;
}

void Listener::OnHeard(const RecognizerResult& result, bool whole) {
  const size_t words =
      WakePhraseWordsAtStart(result.text, config_.wake_phrases);
  const size_t heard = CountWords(result.text);
  // The phrase is complete once a word follows it, or the speech ended.
  if (words > 0 && (whole || heard > words)) {
    OnHeardWake(result, words, whole);
    return;
  }
  if (hearing_open_from_ >= 0) {
    // Begun before the phrase was needed: an utterance from its start,
    // once it ends (the spotter may still hear the phrase in it).
    if (whole) {
      Emit(SpeechStartEvent(At(hearing_open_from_)));
      EndUtterance(result);
    }
    return;
  }
  if (whole) {
    // No wake phrase: nothing is written, and the speech is done with.
    consumed_ = received_;
    ToBaseState();
    return;
  }
  size_t longest = 0;
  for (const std::string& phrase : config_.wake_phrases) {
    longest = std::max(longest, CountWords(phrase));
  }
  // Its first words aren't a wake phrase (run together, the phrase may
  // take one word more than it has), or the speech went on without one.
  if ((words == 0 && heard > longest + 1) ||
      received_ - utterance_from_ >= kHearingSamples) {
    StopHearing();
  }
}

void Listener::OnHeardWake(const RecognizerResult& result,
                           size_t words,
                           bool whole) {
  // As if the spotter had fired where the phrase ended (where the next
  // word starts): the browser's checks for its own voice and other
  // sources go by when it was said.
  int64_t at = received_;
  if (words < result.word_seconds.size()) {
    at = utterance_from_ +
         static_cast<int64_t>(result.word_seconds[words] * kSampleRate);
  } else if (speech_ended_at_ > 0) {
    // The phrase alone, heard once the short-command wait was over: where
    // its speech ended.
    at = speech_ended_at_;
  }
  at = std::clamp(at, std::max(consumed_, utterance_from_), received_);
  hearing_open_from_ = -1;
  wake_at_ = at;
  wake_words_ = words;
  Emit(WakeEvent(At(at)));
  if (CountWords(result.text) <= words) {
    // The phrase alone, and its speech is over: wait for the command.
    consumed_ = received_;
    last_partial_.clear();
    state_ = State::kAwaitCommand;
    return;
  }
  Emit(SpeechStartEvent(At(at)));
  if (whole) {
    EndUtterance(result);
    return;
  }
  state_ = State::kUtterance;
  EmitPartial(result);
}

void Listener::StopHearing() {
  engines_->AbandonUtterance();
  last_partial_.clear();
  heard_off_ = true;
  state_ = State::kWaitWake;
}

void Listener::RespotRecent() {
  const int64_t from =
      std::max({consumed_, received_ - kRespotSamples, int64_t{0}});
  const std::vector<float> recent = Recall(from);
  for (size_t offset = 0; offset < recent.size(); offset += kStepSamples) {
    const size_t n = std::min(kStepSamples, recent.size() - offset);
    const std::vector<float> step(
        recent.begin() + static_cast<ptrdiff_t>(offset),
        recent.begin() + static_cast<ptrdiff_t>(offset + n));
    if (engines_->Spot(step, &wake_words_)) {
      OnWake(from + static_cast<int64_t>(offset + n));
      return;
    }
  }
}

void Listener::BeginFrom(int64_t from_sample) {
  from_sample = std::max({from_sample, consumed_, received_ - kRingSamples,
                          int64_t{0}});
  utterance_from_ = from_sample;
  last_partial_.clear();
  engines_->BeginUtterance(hotwords_);
  engines_->Recognize(Recall(from_sample));
}

bool Listener::HasWordsAfterWake(const RecognizerResult& result,
                                 bool whole) const {
  for (const float seconds : result.word_seconds) {
    const int64_t at =
        utterance_from_ + static_cast<int64_t>(seconds * kSampleRate);
    if (at >= wake_at_ - kCommandCutSamples) {
      return true;
    }
  }
  // The spotter may fire late (in noise): then a short command said in
  // the same breath starts before the cut, but the words still outnumber
  // the phrase's, as the recognizer wrote it ("Himalt" is one word, and
  // "Emold" is all that is left of a phrase whose start was lost).
  if (!whole) {
    return false;
  }
  size_t phrase = WakePhraseWordsAtStart(result.text, config_.wake_phrases);
  if (phrase == 0) {
    phrase = WakeNameWordsAtStart(result.text, config_.wake_phrases);
  }
  return CountWords(result.text) > (phrase > 0 ? phrase : wake_words_);
}

bool Listener::ShortCommand(const RecognizerResult& result) const {
  // The recognizer writes a word a moment after it is said: at the pause
  // after "Hey Molt" it may have written only "Hey, Mal".
  if (OnlyAWakePhraseBegun(result.text, config_.wake_phrases)) {
    return true;
  }
  const size_t words = CountWords(result.text);
  size_t phrase = WakePhraseWordsAtStart(result.text, config_.wake_phrases);
  if (phrase == 0) {
    phrase = WakeNameWordsAtStart(result.text, config_.wake_phrases);
  }
  return words <= phrase + 1;
}

void Listener::StartHangover() {
  speech_ended_at_ = received_;
  hangover_until_ = received_ + kShortCommandHangoverSamples;
  hangover_limit_ = received_ + kMaxHangoverSamples;
}

bool Listener::HangoverOver(const VadUpdate& vad) {
  if (vad.in_speech) {
    // It went on: the same command.
    hangover_until_ = 0;
    speech_ended_at_ = 0;
    return true;
  }
  if (vad.sound) {
    // Perhaps it goes on: long enough for the detector to tell.
    hangover_until_ = std::min(
        std::max(hangover_until_, received_ + kSoundHangoverSamples),
        hangover_limit_);
  }
  if (received_ < hangover_until_) {
    return false;
  }
  hangover_until_ = 0;
  return true;
}

void Listener::EmitPartial(const RecognizerResult& result) {
  if (result.text == last_partial_) {
    return;
  }
  last_partial_ = result.text;
  if (!CleanEventText(result.text, kMaxEventTextBytes).empty()) {
    Emit(PartialEvent(Now(), result.text));
  }
}

void Listener::FinishUtterance() {
  EndUtterance(engines_->Finish());
}

void Listener::EndUtterance(const RecognizerResult& result) {
  Emit(SpeechEndEvent(Now()));
  const bool empty = CleanEventText(result.text, kMaxEventTextBytes).empty();
  Emit(FinalEvent(Now(), result.text, empty ? 0.0 : result.confidence));
  consumed_ = received_;
  ToBaseState();
}

void Listener::EndWithoutCommand() {
  if (state_ == State::kAfterWake) {
    engines_->AbandonUtterance();
  }
  Emit(FinalEvent(Now(), std::string(), 0.0));
  consumed_ = received_;
  ToBaseState();
}

void Listener::ToBaseState() {
  last_partial_.clear();
  heard_off_ = false;
  hangover_until_ = 0;
  hangover_limit_ = 0;
  speech_ended_at_ = 0;
  open_until_ = -1;
  hearing_open_from_ = -1;
  state_ = config_.wake_required ? State::kWaitWake : State::kOpen;
  engines_->ResetSpotter();
  ApplyPendingVad();
}

void Listener::ApplyPendingVad() {
  if (pending_vad_ms_ == 0) {
    return;
  }
  engines_->SetVadSilenceMs(pending_vad_ms_);
  vad_ms_ = pending_vad_ms_;
  pending_vad_ms_ = 0;
  vad_in_speech_ = false;
  speech_onset_ = -1;
}

EventTime Listener::Now() const {
  return At(received_);
}

EventTime Listener::At(int64_t sample) const {
  EventTime time;
  time.epoch = epoch_;
  time.audio_ms = sample * 1000 / kSampleRate;
  return time;
}

void Listener::Emit(const std::string& line) {
  emit_(line);
}

void Listener::Remember(const std::vector<float>& samples) {
  const size_t size = ring_.size();
  for (const float sample : samples) {
    ring_[ring_next_] = sample;
    ring_next_ = (ring_next_ + 1) % size;
  }
}

std::vector<float> Listener::Recall(int64_t from_sample) const {
  // The ring holds samples [received_ - kRingSamples, received_), sample s
  // at index s % kRingSamples.
  std::vector<float> out;
  if (from_sample >= received_) {
    return out;
  }
  out.reserve(static_cast<size_t>(received_ - from_sample));
  for (int64_t s = from_sample; s < received_; ++s) {
    out.push_back(ring_[static_cast<size_t>(s % kRingSamples)]);
  }
  return out;
}

}  // namespace molt_speech
