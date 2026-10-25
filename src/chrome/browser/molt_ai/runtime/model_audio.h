// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Audio clips for the on-device model (docs/design/SANDBOXED_INFERENCE.md,
// section 13). Neither the browser nor the sandboxed inference service
// decodes a compressed audio file: a page's audio is decoded by its own
// renderer (Blink, which also resamples it to the rate the session asks
// for), and the browser only checks and converts the samples it sent
// (engine/audio_input.h PrepareModelAudio()). The service checks them again.
//
// Std-only, like model_image.h: the web AI session context, which keeps a
// conversation's clips, is unit-tested standalone.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_AUDIO_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_AUDIO_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace molt_ai {

// An audio clip ready for the model: 16-bit mono PCM at kAudioSampleRate,
// within the inference service's bounds. Untrusted content, like page
// text.
struct ModelAudio {
  ModelAudio();
  explicit ModelAudio(std::vector<int16_t> samples);
  ~ModelAudio();
  ModelAudio(ModelAudio&&);
  ModelAudio& operator=(ModelAudio&&);
  ModelAudio(const ModelAudio&);
  ModelAudio& operator=(const ModelAudio&);

  // Within the bounds (engine/audio_input.h CheckModelAudio()).
  bool IsValid() const;
  // Its length in seconds, rounded up.
  uint32_t Seconds() const;

  std::vector<int16_t> samples;
};

// A page's decoded audio (Blink's on_device_model.mojom.AudioData) as a
// model clip: engine/audio_input.h PrepareModelAudio() (one channel at
// kAudioSampleRate only; clamped, 16-bit, padded to the minimum; nullopt
// for anything else or a clip over kMaxAudioSamples).
std::optional<ModelAudio> ModelAudioFromPage(int64_t channel_count,
                                             int64_t sample_rate,
                                             int64_t frame_count,
                                             const float* samples,
                                             size_t sample_count);

// Which audio clips a chat request sends, at most kMaxAudioPerRequest
// (engine/audio_input.h): all |user_audio| of the new message (nullopt if
// they alone are more), then the newest history turns' while there is
// room. |turn_audio| holds each history turn's clip count, oldest turn
// first; the result holds, per turn, how many of its newest clips are sent.
std::optional<std::vector<size_t>> PlanRequestAudio(
    size_t user_audio,
    const std::vector<size_t>& turn_audio);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_MODEL_AUDIO_H_
