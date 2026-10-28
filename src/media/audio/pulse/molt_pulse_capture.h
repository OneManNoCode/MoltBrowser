// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the PulseAudio side of molt_capture_source.h. Upstream hook
// (marked "MoltBrowser:"): PulseAudioInputStream::Open() (pulse_input.cc).

#ifndef MEDIA_AUDIO_PULSE_MOLT_PULSE_CAPTURE_H_
#define MEDIA_AUDIO_PULSE_MOLT_PULSE_CAPTURE_H_

#include <pulse/pulseaudio.h>

#include <string>

namespace media {

class AudioParameters;

namespace pulse {

// The source a capture stream opened with `params` records from when asked
// for `device_id` (a source name, or AudioDeviceDescription::kDefaultDeviceId
// for the server's default source). For a portable media process's stream
// (media::kPortableCaptureEffect in `params`), the microphone under a filter
// source (molt::CaptureSourceFor(), from the server's sources and recording
// streams); otherwise, when `device_id` isn't a filter, and when the server
// can't be asked, `device_id` itself. Takes the mainloop's lock and waits
// for the server, so it must be called without the lock held.
std::string MoltCaptureSource(pa_threaded_mainloop* mainloop,
                              pa_context* context,
                              const std::string& device_id,
                              const AudioParameters& params);

}  // namespace pulse
}  // namespace media

#endif  // MEDIA_AUDIO_PULSE_MOLT_PULSE_CAPTURE_H_
