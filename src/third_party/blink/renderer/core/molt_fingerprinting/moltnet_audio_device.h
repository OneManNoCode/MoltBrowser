// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the output device every realtime
// AudioContext in a MoltNet renderer renders to. It renders the context on
// the uniform device's clock, whatever the real device does, and plays the
// result on the real device; the rules are in moltnet_audio_clock.h.
//
//   Upstream hook: AudioDestination's constructor
//   (platform/audio/audio_destination.cc) gets its WebAudioDevice from
//   CreateMoltNetAudioDevice instead of Platform::CreateAudioDevice once
//   PinUniformAudioClock() installed it (AudioDestination::
//   SetMoltNetDeviceFactory), which Configure() does in kUniform mode.
//
// What the AudioDestination (and through it the page) sees:
//   * SampleRate(), FramesPerBuffer(): the uniform device's callback for the
//     context's rate and latency hint (moltnet_media.h), so AudioDestination
//     sizes its FIFO, its worklet earmark and the AudioWorklet thread's
//     realtime period from it.
//   * Render(): called on a realtime thread of its own, `FramesPerBuffer()`
//     frames every FramesPerBuffer() / SampleRate() seconds from Start() or
//     Resume() (media::FakeAudioWorker, Chromium's clock for silent sinks:
//     a late tick runs once and the next one is the next on time), with a
//     fixed delay and no glitches. Single-thread rendering, AudioWorklet
//     rendering and their render quanta follow it exactly as they would a
//     device with that buffer.
//   * MaxChannelCount(): 2.
//   * OnRenderError(): only for an output device the page chose by its ID
//     (which takes a media permission); the default device and silent sinks
//     never fail: when the real device goes away, the context keeps
//     rendering, its output goes nowhere, and the device is opened again
//     after 1, 2, 4 ... 30 s.
//   * MaybeCreateSinkAndGetStatus(): OK for the default device and silent
//     sinks, the real answer for a chosen device.
// The real device (the WebAudioDevice Platform::CreateAudioDevice makes, at
// the context's rate) reads what was rendered from an OutputRing, through a
// media::MultiChannelResampler that DriftControl keeps in step; silent sinks
// ({type: "none"}) have no real device at all.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_DEVICE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_DEVICE_H_

#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "media/base/audio_renderer_sink.h"
#include "third_party/blink/public/platform/web_audio_device.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_clock.h"

namespace base {
class SingleThreadTaskRunner;
}

namespace media {
class AudioBus;
class FakeAudioWorker;
class MultiChannelResampler;
}  // namespace media

namespace blink {

class NonMainThread;
class WebAudioLatencyHint;
class WebAudioSinkDescriptor;

namespace molt_fingerprinting {

class CORE_EXPORT MoltNetAudioDevice final
    : public WebAudioDevice,
      public media::AudioRendererSink::RenderCallback {
 public:
  // Renders for `callback` (the AudioDestination) with
  // `number_of_output_channels` channels at `context_sample_rate` (48 kHz if
  // none). Call SetOutputDevice() next for an audible sink.
  MoltNetAudioDevice(const WebAudioSinkDescriptor& sink_descriptor,
                     unsigned number_of_output_channels,
                     const WebAudioLatencyHint& latency_hint,
                     std::optional<float> context_sample_rate,
                     media::AudioRendererSink::RenderCallback* callback);
  ~MoltNetAudioDevice() override;
  MoltNetAudioDevice(const MoltNetAudioDevice&) = delete;
  MoltNetAudioDevice& operator=(const MoltNetAudioDevice&) = delete;

  // The real output device, created with this object as its callback.
  // Main thread, before Start().
  void SetOutputDevice(std::unique_ptr<WebAudioDevice> device);

  // blink::WebAudioDevice (main thread).
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Resume() override;
  double SampleRate() override;
  int FramesPerBuffer() override;
  int MaxChannelCount() override;
  void SetDetectSilence(bool detect_silence) override;
  media::OutputDeviceStatus MaybeCreateSinkAndGetStatus() override;

  // media::AudioRendererSink::RenderCallback, for the real device: Render()
  // on its audio thread, OnRenderError() on the main thread.
  int Render(base::TimeDelta delay,
             base::TimeTicks delay_timestamp,
             const media::AudioGlitchInfo& glitch_info,
             media::AudioBus* dest) override;
  void OnRenderError() override;

  // For tests: the delay the clock's ticks pass, how often the real device
  // was (re)opened, and how much audio waits for it.
  base::TimeDelta TickDelayForTesting() const { return tick_delay_; }
  int OutputDeviceStartsForTesting() const { return output_device_starts_; }
  int RingLevelForTesting();

 private:
  enum class State { kStopped, kRunning, kPaused };

  // The clock's tick: renders one callback and hands it to the ring
  // (the clock's thread).
  void OnTick(base::TimeTicks ideal_time, base::TimeTicks now);
  // The resampler's input: the next chunk from the ring (the real device's
  // audio thread).
  void ReadRing(int frame_delay, media::AudioBus* bus);

  void StartClock();
  void StopClock();
  void StartOutputDevice();
  void StopOutputDevice();
  void RetryOutputDevice();

  const raw_ptr<media::AudioRendererSink::RenderCallback> callback_;
  const bool chosen_device_;
  const unsigned channels_;
  const int clock_sample_rate_;
  const int tick_frames_;
  const base::TimeDelta tick_delay_;
  const scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;

  // Main thread.
  State state_ = State::kStopped;
  std::unique_ptr<WebAudioDevice> output_device_;
  bool output_device_running_ = false;
  int output_device_starts_ = 0;
  base::TimeDelta retry_delay_;
  std::unique_ptr<NonMainThread> clock_thread_;
  std::unique_ptr<media::FakeAudioWorker> clock_;

  // The clock's thread.
  std::unique_ptr<media::AudioBus> tick_bus_;

  // The real device's audio thread.
  std::unique_ptr<media::MultiChannelResampler> resampler_;
  double nominal_ratio_ = 1;
  double resampler_rate_ = 1;

  // Only for an audible sink (SetOutputDevice()).
  base::Lock lock_;
  std::unique_ptr<moltnet::OutputRing> ring_ GUARDED_BY(lock_);
  std::unique_ptr<moltnet::DriftControl> control_ GUARDED_BY(lock_);
  // Whether ticks go to the ring (only while the real device runs).
  bool feeding_ GUARDED_BY(lock_) = false;
  base::TimeTicks last_tick_due_ GUARDED_BY(lock_);
  bool flush_resampler_ GUARDED_BY(lock_) = false;

  base::WeakPtrFactory<MoltNetAudioDevice> weak_factory_{this};
};

// The factory AudioDestination uses in MoltNet: a MoltNetAudioDevice
// rendering for `callback`, with the device Platform::CreateAudioDevice
// makes for an audible sink (nullptr if there is none, as upstream).
CORE_EXPORT std::unique_ptr<WebAudioDevice> CreateMoltNetAudioDevice(
    const WebAudioSinkDescriptor& sink_descriptor,
    unsigned number_of_output_channels,
    const WebAudioLatencyHint& latency_hint,
    std::optional<float> context_sample_rate,
    media::AudioRendererSink::RenderCallback* callback);

// From now on every realtime AudioContext in this process renders on
// MoltNet's clock (`enabled`), or on its device again. Main thread, before
// any AudioContext exists: Configure() calls it in kUniform mode, tests
// through SetMoltNetMediaForTesting().
CORE_EXPORT void PinUniformAudioClock(bool enabled = true);

}  // namespace molt_fingerprinting
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_DEVICE_H_
