// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace blink::molt_fingerprinting::moltnet {

namespace {

// audio_utilities::MinAudioBufferSampleRate() / MaxAudioBufferSampleRate():
// the rates an AudioContext accepts.
constexpr double kMinContextSampleRate = 3000;
constexpr double kMaxContextSampleRate = 768000;

// A latencyHint longer than this asks for more than kAudioMaxCallbackFrames
// at every valid rate; clamping first keeps the arithmetic in range.
constexpr double kMaxExactSeconds = 10;

bool IsValidContextSampleRate(double rate) {
  return std::isfinite(rate) && rate >= kMinContextSampleRate &&
         rate <= kMaxContextSampleRate;
}

int RoundUpToMultiple(int value, int multiple) {
  return (value + multiple - 1) / multiple * multiple;
}

// The device's 10 ms at the context's rate, as Chromium scales a device's
// buffer to the context rate and clamps it to Web Audio's limits.
int DefaultCallbackFrames(double rate) {
  const double frames = std::ceil(kAudioDeviceFramesPerBuffer * rate /
                                  kAudioDeviceSampleRate);
  return std::clamp(static_cast<int>(frames), kAudioMinCallbackFrames,
                    kAudioMaxCallbackFrames);
}

}  // namespace

int AudioCallbackFrames(AudioLatency latency,
                        double exact_seconds,
                        double context_sample_rate) {
  if (!IsValidContextSampleRate(context_sample_rate)) {
    return 0;
  }
  const int default_frames = DefaultCallbackFrames(context_sample_rate);
  switch (latency) {
    case AudioLatency::kInteractive:
    case AudioLatency::kBalanced:
      return default_frames;
    case AudioLatency::kPlayback: {
      // media::AudioLatency::GetHighLatencyBufferSize outside Windows: 20 ms
      // rounded up to a power of two (at most 16384 frames at 768 kHz).
      const auto twenty_ms = static_cast<uint32_t>(
          std::lround(2.0 * context_sample_rate / 100));
      return std::max(default_frames,
                      static_cast<int>(std::bit_ceil(twenty_ms)));
    }
    case AudioLatency::kExact: {
      // media::AudioLatency::GetExactBufferSize outside Windows, with Web
      // Audio's own limits as the device's (128 to 8192 frames).
      const double seconds =
          exact_seconds > 0 ? std::min(exact_seconds, kMaxExactSeconds) : 0;
      const auto requested =
          static_cast<int>(std::lround(seconds * context_sample_rate));
      if (requested <= kAudioMinCallbackFrames) {
        return kAudioMinCallbackFrames;
      }
      if (requested <= default_frames) {
        return default_frames;
      }
      int frames = RoundUpToMultiple(requested, kAudioMinCallbackFrames);
      if (frames >= kAudioMaxCallbackFrames) {
        frames = RoundUpToMultiple(requested, kAudioMaxCallbackFrames);
      }
      return std::min(frames, kAudioMaxCallbackFrames);
    }
  }
  return default_frames;
}

double AudioBaseLatency(AudioLatency latency,
                        double exact_seconds,
                        uint32_t render_quantum_frames,
                        double context_sample_rate) {
  const int callback_frames =
      AudioCallbackFrames(latency, exact_seconds, context_sample_rate);
  if (callback_frames == 0) {
    return 0;
  }
  const double frames = std::max(static_cast<double>(callback_frames),
                                 static_cast<double>(render_quantum_frames));
  return frames / context_sample_rate;
}

uint32_t ScriptProcessorBufferSize(uint32_t callback_frames) {
  constexpr uint32_t kMinSize = 256;
  constexpr uint32_t kMaxSize = 16384;
  if (callback_frames == 0) {
    return kMinSize;
  }
  const double exponent =
      std::log2(4.0 * static_cast<double>(callback_frames)) + 0.5;
  if (exponent >= 15) {
    return kMaxSize;
  }
  const uint32_t size = 1u << static_cast<uint32_t>(exponent);
  return std::clamp(size, kMinSize, kMaxSize);
}

AudioOutputPosition AudioOutputPositionAt(double current_time) {
  if (!(current_time > 0)) {
    return {0, false};
  }
  return {std::max(0.0, current_time - kAudioOutputLatencySeconds), true};
}

bool ListsUniformDevice(size_t listed, bool ids_exposed) {
  return listed == 0 && !ids_exposed;
}

UserMediaVerdict UserMediaFailureVerdict(
    std::span<const PermissionAnswer> answers) {
  if (answers.empty()) {
    return UserMediaVerdict::kPermissionDenied;
  }
  bool granted = true;
  for (const PermissionAnswer answer : answers) {
    if (answer == PermissionAnswer::kDenied) {
      return UserMediaVerdict::kPermissionDenied;
    }
    granted &= answer == PermissionAnswer::kGranted;
  }
  return granted ? UserMediaVerdict::kRealFailure
                 : UserMediaVerdict::kPermissionDismissed;
}

std::string DeviceLabel(DeviceKind kind, std::string_view device_id) {
  if (device_id.empty()) {
    return std::string();
  }
  if (device_id == kDefaultDeviceId) {
    return "Default";
  }
  if (device_id == kCommunicationsDeviceId) {
    return "Communications";
  }
  std::string label;
  switch (kind) {
    case DeviceKind::kMicrophone:
      label = "Microphone ";
      break;
    case DeviceKind::kCamera:
      label = "Camera ";
      break;
    case DeviceKind::kSpeaker:
      label = "Speaker ";
      break;
  }
  label.append(device_id.substr(0, 4));
  return label;
}

std::string_view DeviceGroup(std::string_view device_id) {
  return device_id;
}

bool ListsAudioDevice(std::string_view device_id) {
  return device_id != kCommunicationsDeviceId;
}

std::string_view OutputDeviceIdToLookUp(std::string_view sink_id) {
  return ListsAudioDevice(sink_id) ? sink_id
                                   : std::string_view(kUnlistedDeviceId);
}

MediaCapabilitiesAnswer VideoDecodingInfo(bool has_built_in_decoder) {
  if (!has_built_in_decoder) {
    return {false, false, false};
  }
  return {true, true, false};
}

}  // namespace blink::molt_fingerprinting::moltnet
