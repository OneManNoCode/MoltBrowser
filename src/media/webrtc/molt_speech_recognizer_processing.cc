// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/webrtc/molt_speech_recognizer_processing.h"

#include <initializer_list>

namespace media {

webrtc::EchoCanceller3Config SpeechRecognizerEchoCancellerConfig(
    webrtc::EchoCanceller3Config config) {
  // The echo's delay from the matched filters' best lag, without the search
  // for an earlier one that a song of steady tones keeps misleading.
  config.delay.detect_pre_echo = false;
  // The suppressor turns to its near-end tuning as soon as a voice is heard
  // over the echo: after one 4 ms block in which what is left of the echo
  // has less than four times the voice's power and the voice more than five
  // times the noise's (by default 12 blocks, a quarter, thirty times), and
  // stays there for a second (by default 0.2 s).
  auto& nearend = config.suppressor.dominant_nearend_detection;
  nearend.enr_threshold = 4.f;
  nearend.snr_threshold = 5.f;
  nearend.trigger_threshold = 1;
  nearend.hold_duration = 250;
  // In that tuning it lets speech through until the echo estimate's power
  // is three times the speech's (by default about the same in the lower
  // bands and a tenth in the upper ones).
  for (auto* mask : {&config.suppressor.nearend_tuning.mask_lf,
                     &config.suppressor.nearend_tuning.mask_hf}) {
    mask->enr_transparent = 3.f;
    mask->enr_suppress = 6.f;
  }
  // What the linear filter alone left: the stream's second channel.
  config.filter.export_linear_aec_output = true;
  return config;
}

bool ExportsLinearEchoCancellerOutput(const webrtc::AudioProcessing& apm) {
  const webrtc::AudioProcessing::Config config = apm.GetConfig();
  return config.echo_canceller.enabled &&
         config.echo_canceller.export_linear_aec_output;
}

bool CopyLinearEchoCancellerOutput(
    const webrtc::AudioProcessing& apm,
    std::array<float, kLinearEchoCancellerOutputFrames>* out) {
  // One channel: the stream's processing is mono (GetLinearAecOutput()
  // fills one array per processed channel).
  if (apm.num_proc_channels() != 1) {
    return false;
  }
  std::array<std::array<float, kLinearEchoCancellerOutputFrames>, 1> linear;
  if (!apm.GetLinearAecOutput(linear)) {
    return false;
  }
  *out = linear[0];
  return true;
}

}  // namespace media
