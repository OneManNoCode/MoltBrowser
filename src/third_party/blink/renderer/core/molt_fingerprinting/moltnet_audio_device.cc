// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_device.h"

#include <algorithm>
#include <utility>

#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_timestamp_helper.h"
#include "media/base/channel_layout.h"
#include "media/base/fake_audio_worker.h"
#include "media/base/output_device_info.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_audio_latency_hint.h"
#include "third_party/blink/public/platform/web_audio_sink_descriptor.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_clock.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_output.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.h"
#include "third_party/blink/renderer/platform/audio/audio_destination.h"
#include "third_party/blink/renderer/platform/scheduler/public/non_main_thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink::molt_fingerprinting {

namespace {

// How long the real device stays closed after it failed, the first time and
// at most.
constexpr base::TimeDelta kFirstRetry = base::Seconds(1);
constexpr base::TimeDelta kMaxRetry = base::Seconds(30);

int ClockRateFor(std::optional<float> context_sample_rate) {
  const int rate = moltnet::ClockSampleRate(
      context_sample_rate.value_or(moltnet::kAudioSampleRate));
  return rate > 0 ? rate : moltnet::kAudioDeviceSampleRate;
}

int TickFramesFor(const WebAudioLatencyHint& latency_hint,
                  std::optional<float> context_sample_rate) {
  const uint32_t frames = UniformDeviceCallbackFrames(
      latency_hint, context_sample_rate.value_or(moltnet::kAudioSampleRate));
  return frames > 0 ? static_cast<int>(frames)
                    : moltnet::kAudioDeviceFramesPerBuffer;
}

// What the clock's ticks pass as the device's delay: the uniform output
// latency less the buffer AudioDestination adds itself. Only traces and
// histograms see it; MoltNet's page-visible latencies are fixed
// (moltnet_media.h).
base::TimeDelta TickDelay(int tick_frames, int sample_rate) {
  return std::max(base::TimeDelta(),
                  base::Seconds(moltnet::kAudioOutputLatencySeconds) -
                      media::AudioTimestampHelper::FramesToTime(tick_frames,
                                                                sample_rate));
}

}  // namespace

MoltNetAudioDevice::MoltNetAudioDevice(
    const WebAudioSinkDescriptor& sink_descriptor,
    unsigned number_of_output_channels,
    const WebAudioLatencyHint& latency_hint,
    std::optional<float> context_sample_rate,
    media::AudioRendererSink::RenderCallback* callback)
    : callback_(callback),
      chosen_device_(sink_descriptor.Type() ==
                         WebAudioSinkDescriptor::kAudible &&
                     !sink_descriptor.SinkId().IsEmpty()),
      channels_(number_of_output_channels),
      clock_sample_rate_(ClockRateFor(context_sample_rate)),
      tick_frames_(TickFramesFor(latency_hint, context_sample_rate)),
      tick_delay_(TickDelay(tick_frames_, clock_sample_rate_)),
      main_task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()),
      retry_delay_(kFirstRetry),
      tick_bus_(media::AudioBus::Create(static_cast<int>(channels_),
                                        tick_frames_)) {}

MoltNetAudioDevice::~MoltNetAudioDevice() {
  Stop();
  clock_.reset();
  // Runs what the clock left on its thread, then joins it.
  clock_thread_.reset();
  output_device_.reset();
}

void MoltNetAudioDevice::SetOutputDevice(
    std::unique_ptr<WebAudioDevice> device) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(state_, State::kStopped);
  if (!device) {
    return;
  }
  output_ = std::make_unique<MoltNetAudioOutput>(
      static_cast<int>(channels_), clock_sample_rate_, tick_frames_,
      device->SampleRate(), device->FramesPerBuffer());
  output_device_ = std::move(device);
}

void MoltNetAudioDevice::Start() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (state_ != State::kStopped) {
    return;
  }
  state_ = State::kRunning;
  retry_delay_ = kFirstRetry;
  StartOutputDevice();
  StartClock();
}

void MoltNetAudioDevice::Stop() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (state_ == State::kStopped) {
    return;
  }
  // No tick renders after this returns (AudioDestination relies on it).
  StopClock();
  StopOutputDevice();
  weak_factory_.InvalidateWeakPtrs();
  state_ = State::kStopped;
}

void MoltNetAudioDevice::Pause() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (state_ != State::kRunning) {
    return;
  }
  StopClock();
  if (output_device_running_) {
    output_device_->Pause();
  }
  state_ = State::kPaused;
}

void MoltNetAudioDevice::Resume() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (state_ != State::kPaused) {
    return;
  }
  state_ = State::kRunning;
  if (output_device_running_) {
    output_->Restart();
    output_device_->Resume();
  } else {
    StartOutputDevice();
  }
  StartClock();
}

double MoltNetAudioDevice::SampleRate() {
  return clock_sample_rate_;
}

int MoltNetAudioDevice::FramesPerBuffer() {
  return tick_frames_;
}

int MoltNetAudioDevice::MaxChannelCount() {
  return static_cast<int>(moltnet::kAudioMaxChannelCount);
}

void MoltNetAudioDevice::SetDetectSilence(bool detect_silence) {
  // Lets the real device rest while the context is silent; the clock keeps
  // ticking either way.
  if (output_device_) {
    output_device_->SetDetectSilence(detect_silence);
  }
}

media::OutputDeviceStatus MoltNetAudioDevice::MaybeCreateSinkAndGetStatus() {
  if (!output_device_) {
    return media::OutputDeviceStatus::OUTPUT_DEVICE_STATUS_OK;
  }
  const media::OutputDeviceStatus status =
      output_device_->MaybeCreateSinkAndGetStatus();
  // Every MoltNet machine has a default output device; one the page chose
  // by its ID answers for itself.
  return chosen_device_ ? status
                        : media::OutputDeviceStatus::OUTPUT_DEVICE_STATUS_OK;
}

int MoltNetAudioDevice::Render(base::TimeDelta delay,
                               base::TimeTicks delay_timestamp,
                               const media::AudioGlitchInfo& glitch_info,
                               media::AudioBus* dest) {
  // The real device only takes what the clock rendered: nothing it passes
  // here (its buffer, delay, timing or glitches) reaches the page.
  if (!output_) {
    dest->Zero();
    return dest->frames();
  }
  output_->Pull(dest);
  return dest->frames();
}

void MoltNetAudioDevice::OnRenderError() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (chosen_device_) {
    callback_->OnRenderError();
    return;
  }
  if (!output_device_running_) {
    return;
  }
  // The context keeps rendering on its clock; its output goes nowhere until
  // the device opens again.
  StopOutputDevice();
  main_task_runner_->PostDelayedTask(
      FROM_HERE,
      blink::BindOnce(&MoltNetAudioDevice::RetryOutputDevice,
                      weak_factory_.GetWeakPtr()),
      retry_delay_);
  retry_delay_ = std::min(retry_delay_ * 2, kMaxRetry);
}

int MoltNetAudioDevice::RingLevelForTesting() {
  return output_ ? output_->RingLevelForTesting() : 0;
}

void MoltNetAudioDevice::OnTick(base::TimeTicks ideal_time,
                                base::TimeTicks now) {
  callback_->Render(tick_delay_, ideal_time, media::AudioGlitchInfo(),
                    tick_bus_.get());
  if (output_) {
    output_->Push(*tick_bus_, ideal_time);
  }
}

void MoltNetAudioDevice::StartClock() {
  if (!clock_) {
    ThreadCreationParams params(ThreadType::kRealtimeAudioWorkletThread);
    params.base_thread_type = base::ThreadType::kRealtimeAudio;
    params.realtime_period = media::AudioTimestampHelper::FramesToTime(
        tick_frames_, clock_sample_rate_);
    clock_thread_ = NonMainThread::CreateThread(params);
    clock_ = std::make_unique<media::FakeAudioWorker>(
        clock_thread_->GetTaskRunner(),
        media::AudioParameters(media::AudioParameters::AUDIO_FAKE,
                               media::ChannelLayoutConfig::Stereo(),
                               clock_sample_rate_, tick_frames_));
  }
  clock_->Start(ConvertToBaseRepeatingCallback(CrossThreadBindRepeating(
      &MoltNetAudioDevice::OnTick, CrossThreadUnretained(this))));
}

void MoltNetAudioDevice::StopClock() {
  if (clock_) {
    clock_->Stop();
  }
}

void MoltNetAudioDevice::StartOutputDevice() {
  if (!output_device_ || output_device_running_) {
    return;
  }
  output_->StartFeeding();
  ++output_device_starts_;
  output_device_running_ = true;
  output_device_->Start();
}

void MoltNetAudioDevice::StopOutputDevice() {
  if (!output_device_running_) {
    return;
  }
  output_device_->Stop();
  output_device_running_ = false;
  output_->StopFeeding();
}

void MoltNetAudioDevice::RetryOutputDevice() {
  if (state_ == State::kRunning) {
    StartOutputDevice();
  }
}

std::unique_ptr<WebAudioDevice> CreateMoltNetAudioDevice(
    const WebAudioSinkDescriptor& sink_descriptor,
    unsigned number_of_output_channels,
    const WebAudioLatencyHint& latency_hint,
    std::optional<float> context_sample_rate,
    media::AudioRendererSink::RenderCallback* callback) {
  auto device = std::make_unique<MoltNetAudioDevice>(
      sink_descriptor, number_of_output_channels, latency_hint,
      context_sample_rate, callback);
  if (sink_descriptor.Type() == WebAudioSinkDescriptor::kAudible) {
    std::unique_ptr<WebAudioDevice> output_device =
        Platform::Current()->CreateAudioDevice(
            sink_descriptor, number_of_output_channels, latency_hint,
            context_sample_rate, device.get());
    if (!output_device) {
      return nullptr;
    }
    device->SetOutputDevice(std::move(output_device));
  }
  return device;
}

void PinUniformAudioClock(bool enabled) {
  AudioDestination::SetMoltNetDeviceFactory(
      enabled ? &CreateMoltNetAudioDevice : nullptr);
}

}  // namespace blink::molt_fingerprinting
