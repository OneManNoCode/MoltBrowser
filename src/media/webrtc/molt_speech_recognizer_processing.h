// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the echo canceller's tuning for a microphone stream that
// feeds a speech recognizer (media::AudioProcessingSettings::
// speech_recognizer: voice mode's), applied by
// CreateWebRtcAudioProcessingModule().
//
// WebRTC's echo canceller (AEC3) finds the echo's delay with matched
// filters, and by default then looks for an earlier lag at which the echo
// might already begin (EchoCanceller3Config::Delay::detect_pre_echo). On a
// song of steady tones the filters' errors repeat with the tones' periods,
// the check keeps choosing one of those earlier lags, and the linear filter
// never settles. Measured with Chromium 156.0.8078.80's AEC3, built
// standalone, on the spoken suite's song (tools/voice_bench/audio.py
// synth_music()) and its echo 6 dB down: the linear filter removed 6 dB of
// the song's echo, against 30 dB of speech's or broadband noise's, and with
// that much echo left the suppressor took speech over the song for echo:
// the first 300 ms of a talker came out 60 dB down, "Hey Molt" said over the
// video 27 dB down, and the speech helper woke for none of 24 commands said
// over it (TOUCHLESS_BROWSING.md §6.1). Without the check the filter removes
// 20 dB of the song's echo within 10 s and the talker's start comes through.
// The suppressor also turns sooner to its near-end tuning, which lets a
// voice heard over the echo through: with both changes the helper woke for
// 32 of 48 such commands said at four points of the song and wrote 21 of
// them right (23 and 10 with the first alone). A video's speech and voice
// mode's own voice are still removed (measured the same way: neither woke
// the helper nor gave it any of the 40 confirmation words).
//
// Only voice mode's stream gets it: pages' streams and calls keep
// Chromium's tuning (the browser clears the flag on the streams renderers
// create, content/browser/renderer_host/media/
// molt_audio_capture_processing.h).
//
// Voice mode's stream also gets, in its second channel, what the linear
// filter alone left of the microphone, before the suppressor
// (AudioProcessor; the stream is stereo at 16 kHz). Over a playing video the
// speech helper hears that while it waits for the wake phrase: on the
// spoken suite's song it woke for 48 of the 48 commands above. It still
// holds a video's own speech, which the suppressor removes, so voice mode
// takes a wake heard there only if the processed channel had the phrase too
// (chrome/browser/molt_ai/voice/voice_session_logic.h, kLinearWakeRatio).

#ifndef MEDIA_WEBRTC_MOLT_SPEECH_RECOGNIZER_PROCESSING_H_
#define MEDIA_WEBRTC_MOLT_SPEECH_RECOGNIZER_PROCESSING_H_

#include <array>

#include "base/component_export.h"
#include "third_party/webrtc/api/audio/audio_processing.h"
#include "third_party/webrtc/api/audio/echo_canceller3_config.h"

namespace media {

// The linear filter's output: 10 ms buffers at 16 kHz, whatever the
// processing's rate.
inline constexpr int kLinearEchoCancellerOutputRate = 16000;
inline constexpr int kLinearEchoCancellerOutputFrames = 160;

// `config` (WebRTC's single- or multichannel default, or one the stream
// already chose) tuned for a speech recognizer, and exporting the linear
// filter's output (EchoCanceller3Config::Filter::export_linear_aec_output;
// harmless unless the audio processing's config asks for it too).
COMPONENT_EXPORT(MEDIA_WEBRTC)
webrtc::EchoCanceller3Config SpeechRecognizerEchoCancellerConfig(
    webrtc::EchoCanceller3Config config);

// Whether `apm`'s config asks its echo canceller for the linear filter's
// output: a speech recognizer's stream with echo cancellation
// (CreateWebRtcAudioProcessingModule()).
COMPONENT_EXPORT(MEDIA_WEBRTC)
bool ExportsLinearEchoCancellerOutput(const webrtc::AudioProcessing& apm);

// After `apm` processed a buffer into one channel: the linear filter's
// output for it, in [-1, 1]. False, leaving `out` as it was, when there is
// none.
COMPONENT_EXPORT(MEDIA_WEBRTC)
bool CopyLinearEchoCancellerOutput(
    const webrtc::AudioProcessing& apm,
    std::array<float, kLinearEchoCancellerOutputFrames>* out);

}  // namespace media

#endif  // MEDIA_WEBRTC_MOLT_SPEECH_RECOGNIZER_PROCESSING_H_
