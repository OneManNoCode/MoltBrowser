// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Audio input for the on-device model (docs/design/SANDBOXED_INFERENCE.md,
// section 13). An audio clip reaches the sandboxed inference service only
// as 16 kHz mono 16-bit PCM within fixed bounds; nothing in the browser or
// the service decodes a compressed audio file. This file is the one
// definition of those bounds and of how a page's decoded audio is brought
// within them:
//
//  - Blink decodes the page's audio (an AudioBuffer, or a Blob or
//    ArrayBuffer holding an audio file, with the renderer's own media
//    decoders) and resamples it to kAudioSampleRate mono, which the
//    session's instance info asks for;
//  - the browser turns what Blink sent into model audio with
//    PrepareModelAudio(): only kAudioSampleRate mono is taken, samples are
//    clamped to [-1, 1] (NaN and infinities become silence) and stored as
//    16-bit PCM, a clip shorter than kMinAudioSamples is padded with
//    silence, and one longer than kMaxAudioSamples is refused;
//  - the service checks every clip again with CheckModelAudio() (a
//    violation is a bad message), and so does LlamaEngine before handing
//    the samples to llama.cpp's multimodal library as floats
//    (AudioSamplesToFloat()).
//
// Std-only, unit-tested in audio_input_unittest.cc.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_AUDIO_INPUT_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_AUDIO_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace molt_ai {

// Must match molt_inference.mojom (static_asserts in molt_model_impl.cc).
// What every supported audio encoder takes (llama.cpp's audio
// preprocessors resample nothing): 16 kHz, one channel.
inline constexpr uint32_t kAudioSampleRate = 16000;
// 0.1 s: shorter clips are padded with silence (the encoders' windows need
// a few hundred samples).
inline constexpr size_t kMinAudioSamples = kAudioSampleRate / 10;
// 30 s, the window of the Whisper-style encoders: one clip is at most one
// or two encoder windows.
inline constexpr size_t kMaxAudioSeconds = 30;
inline constexpr size_t kMaxAudioSamples = kAudioSampleRate * kMaxAudioSeconds;
// 16-bit signed PCM in the host's byte order (little-endian on every
// platform Chromium builds for).
inline constexpr size_t kAudioBytesPerSample = 2;
inline constexpr size_t kMaxAudioBytes = kMaxAudioSamples * kAudioBytesPerSample;
// Audio clips in one request (the new message and the history together).
inline constexpr size_t kMaxAudioPerRequest = 4;

enum class AudioCheck {
  kOk,
  // Fewer than kMinAudioSamples samples.
  kTooShort,
  // More than kMaxAudioSamples samples.
  kTooLong,
  // Not a whole number of 16-bit samples.
  kBadByteLength,
};

// Whether |byte_length| bytes of 16-bit mono PCM at kAudioSampleRate may be
// given to the model.
AudioCheck CheckModelAudio(size_t byte_length);

// What the browser does with a page's decoded audio (Blink's
// on_device_model.mojom.AudioData: |channel_count| channels at
// |sample_rate| Hz, |frame_count| frames, |sample_count| floats at
// |samples|): the model's 16-bit samples, at least kMinAudioSamples (padded
// with silence). nullopt unless it is one channel at kAudioSampleRate,
// |frame_count| is |sample_count| and between 1 and kMaxAudioSamples, and
// |samples| is non-null. Each sample is clamped to [-1, 1] (NaN and
// infinities become 0) and rounded to the nearest of 32767 steps.
std::optional<std::vector<int16_t>> PrepareModelAudio(int64_t channel_count,
                                                      int64_t sample_rate,
                                                      int64_t frame_count,
                                                      const float* samples,
                                                      size_t sample_count);

// The samples as llama.cpp's audio encoders take them: s / 32768.
std::vector<float> AudioSamplesToFloat(const int16_t* samples, size_t count);

// Which audio encoders the service runs. llama.cpp's audio projectors
// encode a clip in windows; how many tokens a window takes depends on the
// projector's type and its frame-stacking factor. The service gives audio
// only to projectors on this list, each verified to give one 30-second
// window exactly the tokens llama.cpp reserves for it (section 13.1 of the
// design); a projector with any other audio encoder loads without audio
// input. |projector_type| and |stack_factor| come from the projector's own
// header, read in the service (molt_llama::ReadProjectorFacts()). The
// tokens of one window, or nullopt if the projector isn't on the list.
std::optional<uint32_t> AllowedAudioWindowTokens(
    std::string_view projector_type,
    uint32_t stack_factor);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_AUDIO_INPUT_H_
