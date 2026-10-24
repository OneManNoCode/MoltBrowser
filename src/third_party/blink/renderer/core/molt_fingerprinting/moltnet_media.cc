// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"

#include <atomic>

#include "base/no_destructor.h"
#include "third_party/blink/public/platform/web_audio_latency_hint.h"
#include "third_party/blink/public/platform/web_encrypted_media_client.h"
#include "third_party/blink/public/platform/web_encrypted_media_request.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_device.h"

namespace blink::molt_fingerprinting {

namespace {

// The latency categories name WebAudioLatencyHint's.
static_assert(static_cast<int>(WebAudioLatencyHint::kCategoryInteractive) ==
              static_cast<int>(moltnet::AudioLatency::kInteractive));
static_assert(static_cast<int>(WebAudioLatencyHint::kCategoryBalanced) ==
              static_cast<int>(moltnet::AudioLatency::kBalanced));
static_assert(static_cast<int>(WebAudioLatencyHint::kCategoryPlayback) ==
              static_cast<int>(moltnet::AudioLatency::kPlayback));
static_assert(static_cast<int>(WebAudioLatencyHint::kCategoryExact) ==
              static_cast<int>(moltnet::AudioLatency::kExact));

std::atomic<bool> g_media_for_testing{false};

bool UniformMode() {
  return IsUniform() || g_media_for_testing.load(std::memory_order_relaxed);
}

// Refuses every key system the way Chromium refuses an unsupported one.
// Chromium may answer synchronously too (an unknown key system), and both
// callers acquire their promise first.
class NoKeySystemsClient final : public WebEncryptedMediaClient {
 public:
  void RequestMediaKeySystemAccess(WebEncryptedMediaRequest request) override {
    request.RequestNotSupported(
        WebString::FromUtf8(moltnet::kNoKeySystemMessage));
  }
};

}  // namespace

std::optional<float> UniformAudioContextSampleRate(
    std::optional<float> requested) {
  if (requested.has_value() || !UniformMode()) {
    return requested;
  }
  return moltnet::kAudioSampleRate;
}

std::optional<uint32_t> UniformRenderQuantumFrames(
    std::optional<uint32_t> requested) {
  if (requested.has_value() || !UniformMode()) {
    return requested;
  }
  return moltnet::kAudioHardwareRenderQuantum;
}

std::optional<uint32_t> UniformAudioCallbackFrames(
    const WebAudioLatencyHint& latency_hint,
    double context_sample_rate) {
  if (!UniformMode()) {
    return std::nullopt;
  }
  const uint32_t frames =
      UniformDeviceCallbackFrames(latency_hint, context_sample_rate);
  if (frames == 0) {
    return std::nullopt;
  }
  return frames;
}

uint32_t UniformDeviceCallbackFrames(const WebAudioLatencyHint& latency_hint,
                                     double context_sample_rate) {
  moltnet::AudioLatency latency = moltnet::AudioLatency::kInteractive;
  double exact_seconds = 0;
  switch (latency_hint.Category()) {
    case WebAudioLatencyHint::kCategoryInteractive:
      latency = moltnet::AudioLatency::kInteractive;
      break;
    case WebAudioLatencyHint::kCategoryBalanced:
      latency = moltnet::AudioLatency::kBalanced;
      break;
    case WebAudioLatencyHint::kCategoryPlayback:
      latency = moltnet::AudioLatency::kPlayback;
      break;
    case WebAudioLatencyHint::kCategoryExact:
      latency = moltnet::AudioLatency::kExact;
      exact_seconds = latency_hint.Seconds();
      break;
    case WebAudioLatencyHint::kLastValue:
      break;
  }
  const int frames =
      moltnet::AudioCallbackFrames(latency, exact_seconds, context_sample_rate);
  return frames > 0 ? static_cast<uint32_t>(frames) : 0;
}

bool HasUniformDefaultAudioOutput() {
  return UniformMode();
}

uint32_t UniformAudioMaxChannelCount(uint32_t real) {
  return UniformMode() ? moltnet::kAudioMaxChannelCount : real;
}

std::optional<double> UniformAudioOutputLatency() {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return moltnet::kAudioOutputLatencySeconds;
}

std::optional<base::TimeDelta> UniformAudioPlayoutDelay() {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return base::Seconds(moltnet::kAudioOutputLatencySeconds);
}

std::optional<moltnet::AudioOutputPosition> UniformAudioOutputPosition(
    double current_time) {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return moltnet::AudioOutputPositionAt(current_time);
}

WebEncryptedMediaClient* MoltNetEncryptedMediaClient() {
  if (!UniformMode()) {
    return nullptr;
  }
  static base::NoDestructor<NoKeySystemsClient> client;
  return client.get();
}

std::optional<moltnet::MediaCapabilitiesAnswer> UniformVideoDecodingInfo(
    bool has_built_in_decoder) {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return moltnet::VideoDecodingInfo(has_built_in_decoder);
}

std::optional<moltnet::MediaCapabilitiesAnswer> UniformWebrtcCodingInfo() {
  if (!UniformMode()) {
    return std::nullopt;
  }
  return moltnet::kWebrtcCodingInfo;
}

bool UsesPlatformMediaCodecs() {
  return !UniformMode();
}

void SetMoltNetMediaForTesting(bool uniform) {
  g_media_for_testing.store(uniform, std::memory_order_relaxed);
  PinUniformAudioClock(uniform || IsUniform());
}

}  // namespace blink::molt_fingerprinting
