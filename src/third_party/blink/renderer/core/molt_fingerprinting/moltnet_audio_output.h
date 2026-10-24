// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: audio a MoltNet clock rendered, on
// its way to the real output device. MoltNet renders Web Audio
// (moltnet_audio_device.h) and media playback (moltnet_audio_sink.h) on a
// fixed clock of its own instead of when the device asks; both hand what the
// clock rendered to the device through one of these: an OutputRing the
// clock's ticks fill, read by the device through a media::
// MultiChannelResampler whose ratio DriftControl nudges so the ring neither
// runs dry nor grows (the rules are in moltnet_audio_clock.h). Nothing the
// device does (its buffer, rate, timing, glitches) reaches the clock.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_OUTPUT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_OUTPUT_H_

#include <memory>

#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_clock.h"

namespace media {
class AudioBus;
class MultiChannelResampler;
}  // namespace media

namespace blink::molt_fingerprinting {

class CORE_EXPORT MoltNetAudioOutput final {
 public:
  // `channels` channels the clock renders at `clock_sample_rate`,
  // `tick_frames` per tick, for a device running at `device_sample_rate`
  // (anything not finite and positive is taken as the clock's rate) that
  // asks for `device_frames` frames per callback.
  MoltNetAudioOutput(int channels,
                     int clock_sample_rate,
                     int tick_frames,
                     double device_sample_rate,
                     int device_frames);
  ~MoltNetAudioOutput();
  MoltNetAudioOutput(const MoltNetAudioOutput&) = delete;
  MoltNetAudioOutput& operator=(const MoltNetAudioOutput&) = delete;

  // The clock's thread: a tick's audio, due at `ideal_time`. Kept only while
  // feeding.
  void Push(const media::AudioBus& bus, base::TimeTicks ideal_time);

  // The device's audio thread: fills `dest` (at the device's rate) with what
  // the clock rendered, or silence while the ring fills.
  void Pull(media::AudioBus* dest);

  // The device (re)starts: from an empty ring, keep what the clock renders.
  void StartFeeding();
  // The device stopped: drop what the clock renders, and what is left.
  void StopFeeding();
  // The clock restarts (a resume): empty the ring and let it fill again.
  void Restart();

  int channels() const { return channels_; }
  int RingLevelForTesting();

 private:
  // The resampler's input: the next chunk from the ring (the device's audio
  // thread).
  void ReadRing(int frame_delay, media::AudioBus* bus);
  void ResetLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  const int channels_;
  const double nominal_ratio_;

  // The device's audio thread.
  std::unique_ptr<media::MultiChannelResampler> resampler_;
  double resampler_rate_ = 1;

  base::Lock lock_;
  std::unique_ptr<moltnet::OutputRing> ring_ GUARDED_BY(lock_);
  std::unique_ptr<moltnet::DriftControl> control_ GUARDED_BY(lock_);
  // Whether ticks go to the ring (only while the device runs).
  bool feeding_ GUARDED_BY(lock_) = false;
  base::TimeTicks last_tick_due_ GUARDED_BY(lock_);
  bool flush_resampler_ GUARDED_BY(lock_) = false;
};

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_OUTPUT_H_
