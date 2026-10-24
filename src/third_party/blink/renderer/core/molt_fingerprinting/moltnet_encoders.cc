// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_encoders.h"

#include <atomic>

#include "media/base/supported_types.h"
#include "media/base/video_encoder.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"

namespace blink::molt_fingerprinting {

namespace {

std::atomic<bool> g_encoders_for_testing{false};

bool UniformMode() {
  return IsUniform() || g_encoders_for_testing.load(std::memory_order_relaxed);
}

}  // namespace

bool MediaRecorderUsesPlatformEncoders() {
  return !UniformMode();
}

bool IsMediaRecorderVideoTypeSupported(const media::VideoType& type) {
  if (!UniformMode()) {
    return media::IsEncoderSupportedVideoType(type);
  }
  // The build's own encoders only. This doesn't ask the media client either,
  // which may wait for the GPU process to list its encoders.
  return media::IsEncoderBuiltInVideoType(type);
}

void PinUniformSoftwareEncoding() {
  media::SetProcessorCountForSoftwareEncoding(
      static_cast<int>(kUniformHardwareConcurrency));
}

void SetMoltNetEncodersForTesting(bool uniform) {
  g_encoders_for_testing.store(uniform, std::memory_order_relaxed);
}

}  // namespace blink::molt_fingerprinting
