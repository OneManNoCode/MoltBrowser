// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the clock MoltNet's Web Audio
// renders on, and how what it renders reaches the real output device. Pure
// and std-only (tested standalone, like moltnet_media_values.h); the Blink
// side is moltnet_audio_device.h.
//
// Chromium renders an AudioContext when the output device asks for audio:
// the device's buffer size, sample rate and callback pattern decide when
// every render quantum runs, so a page that times rendering (currentTime
// steps, AudioWorklet process() bursts, ScriptProcessorNode callbacks)
// learns the hardware. MoltNet renders on a clock of its own instead, the
// uniform device's (moltnet_media_values.h): one callback of
// AudioCallbackFrames() context frames every AudioCallbackFrames() /
// ClockSampleRate() seconds of the system's monotonic clock, from the moment
// the context starts. What it renders goes into an OutputRing; the real
// device reads the ring through a resampler whose speed DriftControl nudges
// by at most 0.2% to keep the ring from running dry or growing, so the
// device's clock never reaches the page. Underruns, device restarts and
// device changes only change what is heard.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_CLOCK_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_CLOCK_H_

#include <array>
#include <cstddef>
#include <span>
#include <vector>

// Blink's style check accepts the ring's std::vector with the annotation
// ALLOW_DISCOURAGED_TYPE() makes (third_party/blink/renderer/platform/
// allow_discouraged_type.h). That header is Blink platform's, and this one
// is std-only (the :farbling target, built and tested outside Blink too),
// so it spells the annotation out; undefined again at the end.
#if defined(__clang__)
#define MOLTNET_AUDIO_STD_MEMBER \
  __attribute__((annotate("allow_discouraged_type")))
#else
#define MOLTNET_AUDIO_STD_MEMBER
#endif

namespace blink::molt_fingerprinting::moltnet {

// --- The clock ---------------------------------------------------------------

// The rate the clock (and the stream to the real device) runs at for a
// context at `context_sample_rate`: the rate in whole frames per second, as
// Chromium opens a context's output stream (RendererWebAudioDeviceImpl).
// 0 for a rate an AudioContext can't have (3000 to 768000 Hz).
int ClockSampleRate(double context_sample_rate);

// --- From the clock to the device --------------------------------------------

// Planar float audio the clock has rendered and the device hasn't read yet,
// oldest first, at most capacity() frames. Not thread-safe.
class OutputRing {
 public:
  // At most kMaxChannels channels (media::limits::kMaxChannels); a bad
  // channel count or capacity makes an empty ring that holds nothing.
  static constexpr int kMaxChannels = 32;
  OutputRing(int channels, int capacity_frames);
  ~OutputRing();
  OutputRing(const OutputRing&) = delete;
  OutputRing& operator=(const OutputRing&) = delete;

  int channels() const { return channels_; }
  int capacity() const { return capacity_; }
  // Frames held.
  int level() const { return level_; }

  // Appends `frames` frames, channel c from channels[c] (missing or short
  // channels read as silence, extra ones are ignored). When the ring would
  // overflow, the oldest frames make room. Returns how many old frames were
  // dropped.
  int Push(std::span<const std::span<const float>> channels, int frames);

  // Moves up to `frames` of the oldest frames (no more than the shortest of
  // the spans holds) into channels[c] and returns how many it moved; the
  // ring's channels beyond channels.size() lose those frames. The rest of
  // each span is left untouched.
  int Pop(std::span<const std::span<float>> channels, int frames);

  // Drops up to `frames` of the oldest frames; returns how many.
  int Discard(int frames);

  // How many of the oldest frames, up to `max_frames`, are digital silence
  // (exactly 0 in every channel).
  int LeadingSilence(int max_frames) const;

  void Clear();

 private:
  int channels_ = 0;
  int capacity_ = 0;
  int read_ = 0;
  int level_ = 0;
  // channels_ * capacity_ samples, channel after channel.
  std::vector<float> samples_ MOLTNET_AUDIO_STD_MEMBER;
};

// The ring size for a clock tick of `tick_frames` and a device that reads
// `device_frames` (both in frames at `sample_rate`) in chunks of
// `read_frames`: room for what DriftControl fills it to (two of the
// device's callbacks and its largest margin), two ticks, and the largest
// excess it tolerates before dropping.
int OutputRingFrames(int tick_frames,
                     int device_frames,
                     int read_frames,
                     double sample_rate);

// What the device side does before reading a chunk from the ring.
struct ReadPlan {
  // Read the chunk; false: hand the device silence and read nothing (the
  // ring is filling up after a start, a resume or an underrun).
  bool play = false;
  // First drop this many of the oldest frames (a large excess, left by a
  // device that stalled; one audible skip).
  int drop = 0;
  // Then drop up to this many of the oldest frames as long as they are
  // silent (OutputRing::LeadingSilence): excess latency nobody hears go.
  int drop_if_silent = 0;
};

// Keeps the ring as short as the device allows without running dry, from
// the device side only. Each read of a chunk sees the ring's level and how
// long ago the clock's last tick was due; the clock's own sawtooth is taken
// out (the level it would have if the clock rendered continuously), so what
// remains is the device's pattern and both sides' jitter. Every half second
// of audio read, the lowest headroom of the last 1.5 s above the margin
// sets the read rate: 1 + excess / 4 s, within 1 +- 0.002 (a pitch change
// of at most 3.5 cents; following a typical device's clock takes about
// 0.01%). Excess that grows by more than 50 ms (a device that stalled) is
// dropped at once, silence above 1 ms is dropped freely. After a start or
// an underrun the ring fills to one of the device's callbacks and a cushion
// (another callback, at most 20 ms) before it plays; an underrun also adds
// a callback (at least 2 ms) to the margin, up to 20 ms or a callback. Not
// thread-safe; no time source of its own.
class DriftControl {
 public:
  // The clock renders `tick_frames` frames per tick at `sample_rate`; the
  // device asks for `device_frames` (at that rate) per callback.
  DriftControl(int tick_frames, int device_frames, double sample_rate);

  // After a start, a resume or a device restart: wait for the ring to fill.
  void Reset();

  // Before reading `frames` from a ring holding `level` frames, when the
  // clock's last tick was due `since_tick_seconds` ago.
  ReadPlan BeforeRead(int level, double since_tick_seconds, int frames);

  // The read got fewer frames than it asked for.
  void OnShortRead();

  // The rate to read at, relative to the clock's (1 + adjustment).
  double read_rate() const { return 1 + adjust_; }

  bool primed() const { return primed_; }
  double margin_frames() const { return margin_; }

  // The ring's level as if the clock rendered continuously: `level` less
  // the part of the last tick that isn't due yet.
  double VirtualLevel(int level, double since_tick_seconds) const;

 private:
  void EndWindow();

  const double sample_rate_;
  const double tick_frames_;
  const double device_frames_;
  double margin_;
  bool primed_ = false;
  double adjust_ = 0;
  double window_read_ = 0;
  double window_min_headroom_ = 0;
  static constexpr size_t kWindows = 3;
  std::array<double, kWindows> recent_min_headroom_ = {};
  size_t windows_seen_ = 0;
  int pending_drop_ = 0;
  int pending_silent_drop_ = 0;
  // An excess above this is dropped at once.
  double hard_limit_ = 0;
};

}  // namespace blink::molt_fingerprinting::moltnet

#undef MOLTNET_AUDIO_STD_MEMBER

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_CLOCK_H_
