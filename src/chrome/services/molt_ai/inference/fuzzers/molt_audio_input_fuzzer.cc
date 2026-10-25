// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Audio clips for the on-device model (engine/audio_input.h): the bounds
// the sandboxed service checks every clip against, how the browser turns a
// page's decoded audio (any channel count, rate, frame count and sample
// values a compromised renderer could send) into 16 kHz mono 16-bit PCM,
// and the encoder's conversion back to floats.
//
// Checks: CheckModelAudio() accepts exactly the even byte lengths of 0.1 to
// 30 seconds; PrepareModelAudio() accepts only one channel at 16 kHz with
// a frame count that is the data's, reads only the samples it is given
// (ASan), turns each into the nearest 16-bit value of the clamped sample
// (NaN and infinities into silence), pads short clips with
// silence to the minimum, and what it makes always passes
// CheckModelAudio(); AudioSamplesToFloat() stays within [-1, 1).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/audio_input.h"

namespace {

// What PrepareModelAudio() should make of one sample.
int16_t Expected(float s) {
  if (!std::isfinite(s)) {
    return 0;
  }
  const float clamped = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
  return static_cast<int16_t>(std::lrint(clamped * 32767.0f));
}

float SomeSample(FuzzedDataProvider& provider) {
  switch (provider.ConsumeIntegralInRange<int>(0, 5)) {
    case 0:
      return std::numeric_limits<float>::quiet_NaN();
    case 1:
      return provider.ConsumeBool() ? std::numeric_limits<float>::infinity()
                                    : -std::numeric_limits<float>::infinity();
    case 2:
      return provider.ConsumeFloatingPoint<float>();
    case 3:
      return provider.ConsumeFloatingPointInRange<float>(-1.5f, 1.5f);
    default:
      return provider.ConsumeProbability<float>() * 2.0f - 1.0f;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider provider(data, size);

  // The service's check on any byte length.
  const size_t len = provider.ConsumeBool()
                         ? provider.ConsumeIntegral<size_t>()
                         : provider.ConsumeIntegralInRange<size_t>(
                               0, kMaxAudioBytes + 8);
  const AudioCheck check = CheckModelAudio(len);
  const bool in_bounds = len % kAudioBytesPerSample == 0 &&
                         len >= kMinAudioSamples * kAudioBytesPerSample &&
                         len <= kMaxAudioBytes;
  CHECK((check == AudioCheck::kOk) == in_bounds);

  // The browser's conversion: mostly what Blink sends (one channel at
  // 16 kHz), sometimes anything.
  const bool honest = provider.ConsumeBool();
  const int64_t channels =
      honest ? 1 : provider.ConsumeIntegralInRange<int64_t>(-2, 3);
  const int64_t rate =
      honest ? int64_t{kAudioSampleRate}
             : (provider.ConsumeBool()
                    ? provider.ConsumeIntegral<int64_t>()
                    : provider.ConsumeIntegralInRange<int64_t>(15999, 16001));
  // Short clips are cheap to fill; long ones are mostly silence.
  const size_t count =
      provider.ConsumeIntegralInRange<int>(0, 31) != 0
          ? provider.ConsumeIntegralInRange<size_t>(0, 4000)
          : provider.ConsumeIntegralInRange<size_t>(kMaxAudioSamples - 2,
                                                    kMaxAudioSamples + 2);
  int64_t frames = static_cast<int64_t>(count);
  if (!honest && provider.ConsumeBool()) {
    frames = provider.ConsumeIntegral<int64_t>();
  }
  std::vector<float> samples(count, 0.0f);
  for (size_t i = 0; i < samples.size() && provider.remaining_bytes() > 0;
       i += provider.ConsumeIntegralInRange<size_t>(1, 64)) {
    samples[i] = SomeSample(provider);
  }
  const float* ptr = samples.empty() ? nullptr : samples.data();
  const std::optional<std::vector<int16_t>> pcm =
      PrepareModelAudio(channels, rate, frames, ptr, samples.size());
  const bool shape_ok = channels == 1 && rate == kAudioSampleRate &&
                        frames == static_cast<int64_t>(samples.size()) &&
                        !samples.empty() && samples.size() <= kMaxAudioSamples;
  CHECK(pcm.has_value() == shape_ok);
  if (pcm) {
    CHECK(CheckModelAudio(pcm->size() * kAudioBytesPerSample) ==
          AudioCheck::kOk);
    CHECK(pcm->size() == (samples.size() < kMinAudioSamples
                              ? kMinAudioSamples
                              : samples.size()));
    // Every sample of a short clip; a sparse sample of a long one (cheap
    // runs), always including the written ones' neighbourhood ends.
    const size_t step = pcm->size() > 8000 ? 97 : 1;
    for (size_t i = 0; i < pcm->size(); i += step) {
      CHECK((*pcm)[i] ==
            (i < samples.size() ? Expected(samples[i]) : int16_t{0}));
    }
    CHECK(pcm->back() == (pcm->size() == samples.size()
                              ? Expected(samples.back())
                              : int16_t{0}));
    const std::vector<float> back =
        AudioSamplesToFloat(pcm->data(), pcm->size());
    CHECK(back.size() == pcm->size());
    for (size_t i = 0; i < back.size(); i += step) {
      CHECK(back[i] >= -1.0f && back[i] < 1.0f);
    }
  }

  // The encoder's conversion of any samples.
  std::vector<int16_t> raw;
  while (provider.remaining_bytes() >= 2 && raw.size() < 2048) {
    raw.push_back(provider.ConsumeIntegral<int16_t>());
  }
  const std::vector<float> floats = AudioSamplesToFloat(
      raw.empty() ? nullptr : raw.data(), raw.size());
  CHECK(floats.size() == raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    CHECK(floats[i] == static_cast<float>(raw[i]) / 32768.0f);
  }
  return 0;
}
