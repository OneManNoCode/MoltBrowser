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

#ifndef MEDIA_WEBRTC_MOLT_SPEECH_RECOGNIZER_PROCESSING_H_
#define MEDIA_WEBRTC_MOLT_SPEECH_RECOGNIZER_PROCESSING_H_

#include "base/component_export.h"
#include "third_party/webrtc/api/audio/echo_canceller3_config.h"

namespace media {

// `config` (WebRTC's single- or multichannel default, or one the stream
// already chose) tuned for a speech recognizer.
COMPONENT_EXPORT(MEDIA_WEBRTC)
webrtc::EchoCanceller3Config SpeechRecognizerEchoCancellerConfig(
    webrtc::EchoCanceller3Config config);

}  // namespace media

#endif  // MEDIA_WEBRTC_MOLT_SPEECH_RECOGNIZER_PROCESSING_H_
