// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

#include "media/base/audio_bus.h"
#include "media/base/multi_channel_resampler.h"
#include "media/base/sinc_resampler.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

namespace blink::molt_fingerprinting {

namespace {

// The resampler asks for the ring's audio in chunks of this many frames
// (the smallest request that keeps its 64-tap kernel).
constexpr int kReadFrames = media::SincResampler::kSmallRequestSize;

double NominalRatio(int clock_sample_rate, double device_sample_rate) {
  return device_sample_rate > 0 && std::isfinite(device_sample_rate)
             ? clock_sample_rate / device_sample_rate
             : 1.0;
}

}  // namespace

MoltNetAudioOutput::MoltNetAudioOutput(int channels,
                                       int clock_sample_rate,
                                       int tick_frames,
                                       double device_sample_rate,
                                       int device_frames)
    : channels_(channels),
      nominal_ratio_(NominalRatio(clock_sample_rate, device_sample_rate)) {
  // The ring holds audio at the clock's rate; the device's buffer in those
  // frames.
  const int device_frames_at_clock_rate = static_cast<int>(
      std::ceil(std::max(device_frames, 0) * std::min(nominal_ratio_, 64.0)));
  resampler_ = std::make_unique<media::MultiChannelResampler>(
      channels_, nominal_ratio_, kReadFrames,
      ConvertToBaseRepeatingCallback(CrossThreadBindRepeating(
          &MoltNetAudioOutput::ReadRing, CrossThreadUnretained(this))));
  base::AutoLock locker(lock_);
  ring_ = std::make_unique<moltnet::OutputRing>(
      channels_,
      moltnet::OutputRingFrames(tick_frames, device_frames_at_clock_rate,
                                kReadFrames, clock_sample_rate));
  control_ = std::make_unique<moltnet::DriftControl>(
      tick_frames, device_frames_at_clock_rate, clock_sample_rate);
}

MoltNetAudioOutput::~MoltNetAudioOutput() = default;

void MoltNetAudioOutput::Push(const media::AudioBus& bus,
                              base::TimeTicks ideal_time) {
  base::AutoLock locker(lock_);
  if (!feeding_) {
    return;
  }
  std::array<std::span<const float>, moltnet::OutputRing::kMaxChannels>
      channels = {};
  const size_t count =
      std::min(static_cast<size_t>(bus.channels()), channels.size());
  for (size_t c = 0; c < count; ++c) {
    channels[c] = std::span<const float>(bus.channel(static_cast<int>(c)));
  }
  ring_->Push(std::span(channels).first(count), bus.frames());
  last_tick_due_ = ideal_time;
}

void MoltNetAudioOutput::Pull(media::AudioBus* dest) {
  if (dest->channels() != channels_) {
    dest->Zero();
    return;
  }
  bool flush = false;
  double rate = 1;
  {
    base::AutoLock locker(lock_);
    flush = std::exchange(flush_resampler_, false);
    rate = control_->read_rate();
  }
  if (flush) {
    resampler_->Flush();
  }
  if (rate != resampler_rate_) {
    resampler_->SetRatio(nominal_ratio_ * rate);
    resampler_rate_ = rate;
  }
  resampler_->Resample(dest->frames(), dest);
}

void MoltNetAudioOutput::StartFeeding() {
  base::AutoLock locker(lock_);
  ResetLocked();
  feeding_ = true;
}

void MoltNetAudioOutput::StopFeeding() {
  base::AutoLock locker(lock_);
  feeding_ = false;
  ring_->Clear();
}

void MoltNetAudioOutput::Restart() {
  base::AutoLock locker(lock_);
  ResetLocked();
}

int MoltNetAudioOutput::RingLevelForTesting() {
  base::AutoLock locker(lock_);
  return ring_->level();
}

void MoltNetAudioOutput::ResetLocked() {
  ring_->Clear();
  control_->Reset();
  last_tick_due_ = base::TimeTicks();
  flush_resampler_ = true;
}

void MoltNetAudioOutput::ReadRing(int frame_delay, media::AudioBus* bus) {
  base::AutoLock locker(lock_);
  const double since_tick =
      last_tick_due_.is_null()
          ? 0.0
          : (base::TimeTicks::Now() - last_tick_due_).InSecondsF();
  const moltnet::ReadPlan plan =
      control_->BeforeRead(ring_->level(), since_tick, bus->frames());
  ring_->Discard(plan.drop);
  ring_->Discard(ring_->LeadingSilence(plan.drop_if_silent));
  if (!plan.play) {
    bus->Zero();
    return;
  }
  std::array<std::span<float>, moltnet::OutputRing::kMaxChannels> channels =
      {};
  const size_t count =
      std::min(static_cast<size_t>(bus->channels()), channels.size());
  for (size_t c = 0; c < count; ++c) {
    channels[c] = std::span<float>(bus->channel(static_cast<int>(c)));
  }
  const int got = ring_->Pop(std::span(channels).first(count), bus->frames());
  if (got < bus->frames()) {
    bus->ZeroFramesPartial(got, bus->frames() - got);
    control_->OnShortRead();
  }
}

}  // namespace blink::molt_fingerprinting
