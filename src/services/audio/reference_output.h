// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_AUDIO_REFERENCE_OUTPUT_H_
#define SERVICES_AUDIO_REFERENCE_OUTPUT_H_

#include "base/time/time.h"

namespace media {
class AudioBus;
}  // namespace media

namespace audio {
class ReferenceOutput {
 public:
  class Listener {
   public:
    // Provides read-only access to the auio played by ReferenceOutput.
    // Must execute quickly, as it will typically be called on a realtime
    // thread; otherwise, audio glitches may occur.
    virtual void OnPlayoutData(const media::AudioBus& audio_bus,
                               int sample_rate,
                               base::TimeDelta audio_delay) = 0;

    // Called if an error occurred in the reference stream. Called on the same
    // sequence as StartListening.
    virtual void OnReferenceStreamError() = 0;

    // MoltBrowser: true if this listener's processing computes the same
    // samples on every CPU (a MoltNet microphone stream's echo canceller,
    // media::AudioProcessingSettings::cpu_independent). While such a listener
    // listens, the output it hears is mixed with the portable code
    // (media/base/molt_portable_media.h, ScopedPortableAudioThread), so that
    // its reference is the same on every CPU and OS. Asked once, when it
    // starts listening.
    virtual bool IsCpuIndependent() const { return false; }

   protected:
    virtual ~Listener() = default;
  };

  // Starts/Stops listening to the reference output.
  virtual void StartListening(Listener* listener) = 0;
  virtual void StopListening(Listener* listener) = 0;

 protected:
  virtual ~ReferenceOutput() = default;
};
}  // namespace audio

#endif  // SERVICES_AUDIO_REFERENCE_OUTPUT_H_
