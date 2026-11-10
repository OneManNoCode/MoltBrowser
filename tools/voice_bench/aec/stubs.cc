// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// apm_sim: AGC2's voice detector, which needs RNNoise's weights (not
// fetched), and which voice mode's stream never creates (gain control is
// off). Calling one aborts.

#include <cstdlib>

#include "modules/audio_processing/agc2/vad_wrapper.h"

namespace webrtc {

VoiceActivityDetectorWrapper::VoiceActivityDetectorWrapper(
    const AvailableCpuFeatures&,
    int)
    : vad_reset_period_frames_(0), frame_size_(0), time_to_vad_reset_(0) {
  abort();
}

VoiceActivityDetectorWrapper::VoiceActivityDetectorWrapper(
    int,
    const AvailableCpuFeatures&,
    int)
    : vad_reset_period_frames_(0), frame_size_(0), time_to_vad_reset_(0) {
  abort();
}

VoiceActivityDetectorWrapper::VoiceActivityDetectorWrapper(
    int,
    std::unique_ptr<MonoVad>,
    int)
    : vad_reset_period_frames_(0), frame_size_(0), time_to_vad_reset_(0) {
  abort();
}

VoiceActivityDetectorWrapper::~VoiceActivityDetectorWrapper() = default;

float VoiceActivityDetectorWrapper::Analyze(DeinterleavedView<const float>) {
  abort();
}

}  // namespace webrtc
