// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's work for one SPEAK (docs/design/VOICE_HELPER_PROTOCOL.md
// §4): the text through the front end (voice_text.h, voice_phonemes.h),
// each unit of phonemes through the engine, the samples out as AUDIO
// frames, then one DONE.
//
//   - Fast first audio: the first unit of a request is short (its first
//     sentence cut at its first pause after kMinFirstUnitCodePoints, else
//     between words before kFirstUnitCodePoints), so the first sound
//     doesn't wait for a long sentence to be synthesized.
//   - Paced: the helper stays at most |lookahead_ms| of audio ahead of real
//     time (counted from the request's first AUDIO frame), so the browser
//     never holds much more than that, and a CANCEL has little to undo.
//   - Bounded: at most |max_request_audio_ms| of audio per request and
//     |max_unit_audio_ms| per unit (the engine's output for hostile input
//     is cut there; DONE then says kCut).
//   - Cancellable between units, while pacing and inside the engine's
//     callback.
//
// The engine and the clock sit behind interfaces, so this is std-only and
// tested with a fake engine; standalone/sherpa_voice.h is the real one.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_SYNTH_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_SYNTH_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_phonemes.h"

namespace molt_voice {

// The first unit of a request, in phoneme code points, when its first
// sentence is longer: about a second of speech. It ends at the first pause
// after kMinFirstUnitCodePoints ("Two," alone would be too short a unit).
inline constexpr size_t kFirstUnitCodePoints = 48;
inline constexpr size_t kMinFirstUnitCodePoints = 8;
// AUDIO frames carry at most this many samples (0.19 s at 22,050 Hz).
inline constexpr size_t kAudioChunkSamples = 4096;

struct SynthLimits {
  int64_t lookahead_ms = 3000;
  int64_t max_request_audio_ms = 360000;
  int64_t max_unit_audio_ms = 30000;
};

class VoiceEngine {
 public:
  // Gets samples in [-1, 1] as they come (once or several times per unit);
  // returns false to stop the engine early.
  using AudioCallback = std::function<bool(const float* samples, size_t n)>;

  virtual ~VoiceEngine();

  virtual int sample_rate() const = 0;
  // One unit of phonemes (each a code point of the voice's tokens) at
  // |length_scale| (1 the voice's pace; 0.5 twice as fast). False if the
  // engine failed (a stop asked for by |on_audio| is not a failure).
  virtual bool Synthesize(const std::vector<int32_t>& phonemes,
                          float length_scale,
                          const AudioCallback& on_audio) = 0;
};

class SynthHooks {
 public:
  virtual ~SynthHooks();

  // A monotonic clock, in ms.
  virtual int64_t NowMs() = 0;
  // Whether this request was cancelled.
  virtual bool Cancelled() = 0;
  // Returns at |until_ms|, or earlier when the request is cancelled.
  virtual void WaitUntil(int64_t until_ms) = 0;
  // An AUDIO frame (EncodeAudio()) to write.
  virtual void Emit(std::string frame) = 0;
};

// The units of a request's first sentence: the first one at most
// kFirstUnitCodePoints, the rest as Phonemizer::Units().
std::vector<std::vector<int32_t>> FirstSentenceUnits(
    const std::vector<int32_t>& phonemes);

// Says |request| (the AUDIO frames go to |hooks|) and returns its DONE.
// |length_scale| comes from the request's rate.
DoneInfo SpeakRequestNow(const SpeakRequest& request,
                         const Phonemizer& phonemizer,
                         VoiceEngine& engine,
                         SynthHooks& hooks,
                         const SynthLimits& limits);

// Samples in [-1, 1] as 16-bit PCM, clamped.
int16_t ToPcm16(float sample);

}  // namespace molt_voice

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_SYNTH_H_
