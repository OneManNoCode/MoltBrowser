// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_clock.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace blink::molt_fingerprinting::moltnet {

namespace {

// audio_utilities::MinAudioBufferSampleRate() / MaxAudioBufferSampleRate().
constexpr double kMinContextSampleRate = 3000;
constexpr double kMaxContextSampleRate = 768000;

// DriftControl's constants, in seconds of audio unless noted.
// The headroom kept above what a read takes: covers the jitter of the
// clock's and the device's threads that a window didn't happen to see.
constexpr double kMargin = 0.003;
// An underrun adds this much to the margin, up to kMaxMargin.
constexpr double kMarginStep = 0.002;
constexpr double kMaxMargin = 0.020;
// After a (re)start or an underrun the ring fills to a read plus the margin
// plus this before the device hears it again.
constexpr double kPrimeExtra = 0.005;
// How much audio the device reads between two adjustments; each looks at
// the lowest headroom of the last kWindows windows.
constexpr double kWindow = 0.5;
// The read rate corrects an excess of headroom in this long, at most
// kMaxAdjust faster or slower than the clock.
constexpr double kTimeConstant = 4;
constexpr double kMaxAdjust = 0.002;
// More excess than this is dropped at once; silent excess above
// kSilentExcess is dropped freely.
constexpr double kHardExcess = 0.050;
constexpr double kSilentExcess = 0.001;

bool IsValidRate(double rate) {
  return std::isfinite(rate) && rate >= kMinContextSampleRate &&
         rate <= kMaxContextSampleRate;
}

int ClampFrames(double frames) {
  if (!(frames > 0)) {
    return 0;
  }
  constexpr double kMax = std::numeric_limits<int>::max() / 4;
  return static_cast<int>(std::min(frames, kMax));
}

}  // namespace

int ClockSampleRate(double context_sample_rate) {
  if (!IsValidRate(context_sample_rate)) {
    return 0;
  }
  return static_cast<int>(context_sample_rate);
}

// --- OutputRing --------------------------------------------------------------

OutputRing::OutputRing(int channels, int capacity_frames) {
  if (channels < 1 || channels > kMaxChannels || capacity_frames < 1 ||
      capacity_frames > std::numeric_limits<int>::max() / kMaxChannels) {
    return;
  }
  channels_ = channels;
  capacity_ = capacity_frames;
  samples_.assign(static_cast<size_t>(channels) *
                      static_cast<size_t>(capacity_frames),
                  0.0f);
}

OutputRing::~OutputRing() = default;

int OutputRing::Push(std::span<const std::span<const float>> channels,
                     int frames) {
  if (frames <= 0 || capacity_ == 0) {
    return 0;
  }
  // Only the newest `capacity_` frames can be kept.
  int skip = 0;
  if (frames > capacity_) {
    skip = frames - capacity_;
    frames = capacity_;
  }
  int dropped = 0;
  if (level_ + frames > capacity_) {
    dropped = level_ + frames - capacity_;
    Discard(dropped);
  }
  const int write = (read_ + level_) % capacity_;
  for (int c = 0; c < channels_; ++c) {
    const size_t base = static_cast<size_t>(c) * static_cast<size_t>(capacity_);
    std::span<const float> source;
    if (static_cast<size_t>(c) < channels.size()) {
      source = channels[static_cast<size_t>(c)];
    }
    for (int i = 0; i < frames; ++i) {
      const size_t from = static_cast<size_t>(skip) + static_cast<size_t>(i);
      const float sample = from < source.size() ? source[from] : 0.0f;
      samples_[base + static_cast<size_t>((write + i) % capacity_)] = sample;
    }
  }
  level_ += frames;
  return dropped + skip;
}

int OutputRing::Pop(std::span<const std::span<float>> channels, int frames) {
  const size_t used =
      std::min(channels.size(), static_cast<size_t>(channels_));
  int count = std::clamp(frames, 0, level_);
  for (size_t c = 0; c < used; ++c) {
    count = static_cast<int>(
        std::min(channels[c].size(), static_cast<size_t>(count)));
  }
  for (size_t c = 0; c < used; ++c) {
    const std::span<float> destination = channels[c];
    const size_t base = c * static_cast<size_t>(capacity_);
    for (int i = 0; i < count; ++i) {
      destination[static_cast<size_t>(i)] =
          samples_[base + static_cast<size_t>((read_ + i) % capacity_)];
    }
  }
  Discard(count);
  return count;
}

int OutputRing::Discard(int frames) {
  const int count = std::clamp(frames, 0, level_);
  if (count > 0) {
    read_ = (read_ + count) % capacity_;
    level_ -= count;
  }
  if (level_ == 0) {
    read_ = 0;
  }
  return count;
}

int OutputRing::LeadingSilence(int max_frames) const {
  const int limit = std::clamp(max_frames, 0, level_);
  for (int i = 0; i < limit; ++i) {
    const size_t offset = static_cast<size_t>((read_ + i) % capacity_);
    for (int c = 0; c < channels_; ++c) {
      if (samples_[static_cast<size_t>(c) * static_cast<size_t>(capacity_) +
                   offset] != 0.0f) {
        return i;
      }
    }
  }
  return limit;
}

void OutputRing::Clear() {
  read_ = 0;
  level_ = 0;
}

int OutputRingFrames(int tick_frames,
                     int device_frames,
                     int read_frames,
                     double sample_rate) {
  if (!IsValidRate(sample_rate)) {
    return 0;
  }
  const double frames =
      std::max(0, device_frames) + 2.0 * std::max(0, tick_frames) +
      2.0 * std::max(0, read_frames) +
      (kHardExcess + kMaxMargin + kPrimeExtra) * sample_rate;
  return ClampFrames(std::ceil(frames));
}

// --- DriftControl ------------------------------------------------------------

DriftControl::DriftControl(int tick_frames,
                           int device_frames,
                           double sample_rate)
    : sample_rate_(IsValidRate(sample_rate) ? sample_rate : 48000),
      tick_frames_(std::max(0, tick_frames)),
      device_frames_(std::max(0, device_frames)),
      margin_(kMargin * sample_rate_) {}

void DriftControl::Reset() {
  primed_ = false;
  window_read_ = 0;
  window_min_headroom_ = 0;
  windows_seen_ = 0;
  pending_drop_ = 0;
  pending_silent_drop_ = 0;
}

double DriftControl::VirtualLevel(int level,
                                  double since_tick_seconds) const {
  const double since =
      std::isfinite(since_tick_seconds) ? std::max(0.0, since_tick_seconds)
                                        : 0.0;
  // The clock hands over a whole tick at once; continuously, only the part
  // that is due would be there. Once the next tick is overdue, the level is
  // what it is.
  const double not_yet_due =
      std::max(0.0, tick_frames_ - sample_rate_ * since);
  return std::max(0, level) - not_yet_due;
}

ReadPlan DriftControl::BeforeRead(int level,
                                  double since_tick_seconds,
                                  int frames) {
  const double read = std::max(0, frames);
  const double virtual_level = VirtualLevel(level, since_tick_seconds);
  ReadPlan plan;
  if (!primed_) {
    // Enough for the device's whole callback: its reads come back to back.
    if (virtual_level <
        std::max(read, device_frames_) + margin_ + kPrimeExtra * sample_rate_) {
      return plan;
    }
    primed_ = true;
    window_read_ = 0;
    window_min_headroom_ = std::numeric_limits<double>::infinity();
  }
  plan.play = true;
  plan.drop = std::min(pending_drop_, std::max(0, level));
  plan.drop_if_silent = pending_silent_drop_;
  pending_drop_ = 0;
  pending_silent_drop_ = 0;

  const double headroom = virtual_level - plan.drop - read;
  window_min_headroom_ = std::min(window_min_headroom_, headroom);
  window_read_ += read;
  if (window_read_ >= kWindow * sample_rate_) {
    EndWindow();
  }
  return plan;
}

void DriftControl::EndWindow() {
  // The lowest headroom of the last few windows: a window may just have
  // missed the device's or the clock's worst moment.
  recent_min_headroom_[windows_seen_ % kWindows] = window_min_headroom_;
  ++windows_seen_;
  double lowest = window_min_headroom_;
  for (size_t i = 0; i < std::min(windows_seen_, kWindows); ++i) {
    lowest = std::min(lowest, recent_min_headroom_[i]);
  }
  const double excess = lowest - margin_;
  window_read_ = 0;
  window_min_headroom_ = std::numeric_limits<double>::infinity();
  if (excess > kHardExcess * sample_rate_) {
    // Down to the margin in one step; the next window starts from there.
    pending_drop_ = ClampFrames(std::floor(excess));
    adjust_ = 0;
    return;
  }
  adjust_ = std::clamp(excess / (kTimeConstant * sample_rate_), -kMaxAdjust,
                       kMaxAdjust);
  if (excess > kSilentExcess * sample_rate_) {
    pending_silent_drop_ = ClampFrames(std::floor(excess));
  }
}

void DriftControl::OnShortRead() {
  margin_ = std::min(margin_ + kMarginStep * sample_rate_,
                     kMaxMargin * sample_rate_);
  Reset();
}

}  // namespace blink::molt_fingerprinting::moltnet
