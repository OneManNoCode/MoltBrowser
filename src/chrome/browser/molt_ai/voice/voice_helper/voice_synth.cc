// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_synth.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_text.h"

namespace molt_voice {

VoiceEngine::~VoiceEngine() = default;
SynthHooks::~SynthHooks() = default;

int16_t ToPcm16(float sample) {
  if (!(sample == sample)) {
    return 0;  // NaN
  }
  const float clamped = std::clamp(sample, -1.0f, 1.0f);
  return static_cast<int16_t>(std::lround(clamped * 32767.0f));
}

std::vector<std::vector<int32_t>> FirstSentenceUnits(
    const std::vector<int32_t>& phonemes) {
  if (phonemes.size() <= kFirstUnitCodePoints) {
    return Phonemizer::Units(phonemes);
  }
  // A cut after a pause in the first unit's second half, else at its last
  // space.
  size_t cut = 0;
  for (size_t i = kFirstUnitCodePoints; i > kFirstUnitCodePoints / 2; --i) {
    if (phonemes[i - 1] == ',') {
      cut = i;
      break;
    }
  }
  if (!cut) {
    for (size_t i = kFirstUnitCodePoints; i > 1; --i) {
      if (phonemes[i - 1] == ' ') {
        cut = i - 1;
        break;
      }
    }
  }
  if (!cut) {
    return Phonemizer::Units(phonemes);
  }
  std::vector<int32_t> first(phonemes.begin(),
                             phonemes.begin() + static_cast<ptrdiff_t>(cut));
  if (!first.empty() && first.back() != ',') {
    first.push_back(',');
  }
  size_t rest_start = cut;
  while (rest_start < phonemes.size() && phonemes[rest_start] == ' ') {
    ++rest_start;
  }
  std::vector<std::vector<int32_t>> units;
  units.push_back(std::move(first));
  const std::vector<int32_t> rest(
      phonemes.begin() + static_cast<ptrdiff_t>(rest_start), phonemes.end());
  for (std::vector<int32_t>& unit : Phonemizer::Units(rest)) {
    units.push_back(std::move(unit));
  }
  return units;
}

DoneInfo SpeakRequestNow(const SpeakRequest& request,
                         const Phonemizer& phonemizer,
                         VoiceEngine& engine,
                         SynthHooks& hooks,
                         const SynthLimits& limits) {
  DoneInfo done;
  done.id = request.id;
  const int64_t start_ms = hooks.NowMs();
  if (hooks.Cancelled()) {
    done.status = DoneStatus::kCancelled;
    return done;
  }
  const int rate =
      std::clamp(request.rate_percent, kMinRatePercent, kMaxRatePercent);
  const float length_scale = 100.0f / static_cast<float>(rate);
  const int sample_rate = std::max(engine.sample_rate(), 1);
  const auto ms_of = [sample_rate](uint64_t samples) {
    return static_cast<int64_t>(samples * 1000 /
                                static_cast<uint64_t>(sample_rate));
  };
  const uint64_t max_request_samples =
      static_cast<uint64_t>(limits.max_request_audio_ms) *
      static_cast<uint64_t>(sample_rate) / 1000;
  const uint64_t max_unit_samples =
      static_cast<uint64_t>(limits.max_unit_audio_ms) *
      static_cast<uint64_t>(sample_rate) / 1000;

  uint64_t written = 0;
  int64_t first_audio_at = -1;
  int64_t synth_ms = 0;
  bool any_unit = false;
  bool cut = false;
  bool failed = false;
  bool cancelled = false;
  std::vector<int16_t> pcm;

  const std::vector<SpokenSentence> sentences =
      NormalizeForSpeech(request.text);
  for (size_t s = 0; s < sentences.size() && !cut && !failed && !cancelled;
       ++s) {
    const std::vector<int32_t> phonemes = phonemizer.Sentence(sentences[s]);
    if (phonemes.empty()) {
      continue;
    }
    const std::vector<std::vector<int32_t>> units =
        s == 0 || !any_unit ? FirstSentenceUnits(phonemes)
                            : Phonemizer::Units(phonemes);
    for (const std::vector<int32_t>& unit : units) {
      if (hooks.Cancelled()) {
        cancelled = true;
        break;
      }
      // Stay within the lookahead of real time.
      if (first_audio_at >= 0) {
        const int64_t played = hooks.NowMs() - first_audio_at;
        const int64_t ahead = ms_of(written) - played;
        if (ahead > limits.lookahead_ms) {
          hooks.WaitUntil(hooks.NowMs() + ahead - limits.lookahead_ms);
          if (hooks.Cancelled()) {
            cancelled = true;
            break;
          }
        }
      }
      any_unit = true;
      uint64_t unit_samples = 0;
      const int64_t unit_start = hooks.NowMs();
      const bool ok = engine.Synthesize(
          unit, length_scale, [&](const float* samples, size_t n) {
            if (hooks.Cancelled()) {
              cancelled = true;
              return false;
            }
            uint64_t take = n;
            if (unit_samples + take > max_unit_samples) {
              take = max_unit_samples - unit_samples;
              cut = true;
            }
            if (written + take > max_request_samples) {
              take = max_request_samples - written;
              cut = true;
            }
            size_t at = 0;
            while (at < take) {
              const size_t chunk = static_cast<size_t>(
                  std::min<uint64_t>(take - at, kAudioChunkSamples));
              pcm.resize(chunk);
              for (size_t i = 0; i < chunk; ++i) {
                pcm[i] = ToPcm16(samples[at + i]);
              }
              if (first_audio_at < 0) {
                first_audio_at = hooks.NowMs();
                done.first_audio_ms = static_cast<uint32_t>(
                    std::max<int64_t>(first_audio_at - start_ms, 0));
              }
              hooks.Emit(EncodeAudio(request.id, pcm.data(), chunk));
              at += chunk;
            }
            unit_samples += take;
            written += take;
            return !cut;
          });
      synth_ms += hooks.NowMs() - unit_start;
      if (!ok && !cancelled && !cut) {
        failed = true;
      }
      if (cut || failed || cancelled) {
        break;
      }
    }
  }
  done.audio_ms = static_cast<uint32_t>(ms_of(written));
  done.synth_ms = static_cast<uint32_t>(std::max<int64_t>(synth_ms, 0));
  if (cancelled) {
    done.status = DoneStatus::kCancelled;
  } else if (failed) {
    done.status = DoneStatus::kEngineFailed;
  } else if (cut) {
    done.status = DoneStatus::kCut;
  } else if (!any_unit || written == 0) {
    done.status = DoneStatus::kNothingToSay;
  } else {
    done.status = DoneStatus::kSpoken;
  }
  return done;
}

}  // namespace molt_voice
