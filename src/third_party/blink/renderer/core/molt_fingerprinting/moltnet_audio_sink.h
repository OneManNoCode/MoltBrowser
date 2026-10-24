// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the output device <audio>, <video>
// and MediaStream playback render to in a MoltNet renderer. Like Web Audio's
// (moltnet_audio_device.h), it renders on the uniform device's clock,
// whatever the real device does, and plays the result on the real device.
//
//   Upstream hook: AudioDeviceFactory (modules/media/audio/
//   audio_device_factory.cc) wraps the output device it creates for media
//   elements (the sink of the AudioRendererMixer every media element of a
//   page plays through) and for MediaStream tracks (TrackAudioRenderer) in
//   one of these when UsesUniformMediaClock() says so.
//
// What the client (the mixer or the track renderer, and through it
// AudioRendererImpl, currentTime, the frames video shows and
// requestVideoFrameCallback) sees:
//   * GetOutputDeviceInfo(): the uniform device, 48 kHz stereo with a
//     480-frame buffer (moltnet_media_values.h), status OK for the default
//     device whether or not the machine has one; for a device the page chose
//     by its ID (which takes a media permission) the device's own status.
//     So AudioRendererImpl and the mixer pick the same rates and buffers on
//     every machine.
//   * Render(): called on a realtime thread MoltNet shares between its media
//     sinks, the client's buffer (Initialize()) every buffer's duration from
//     Start() or Play() (media::FakeAudioWorker: a late tick runs once and
//     the next one is the next on time), with a fixed delay and no
//     glitches. The delay is what a FakeAudioWorker sink reports (two
//     buffers, media::FakeAudioWorker::ComputeFakeOutputDelay, as Chromium's
//     NullAudioSink) and 30 ms, about what the ring and a typical wired
//     output add, so a video's picture keeps in step with its sound there.
//   * OnRenderError(): only for a chosen device. The default device never
//     fails: when the real device can't be opened or goes away, rendering
//     goes on, its output goes nowhere, and a new device is opened after
//     1, 2, 4 ... 30 s.
// The real device (opened at its own rate, with the buffer Chromium would
// use for the client's latency) plays what was rendered through a
// MoltNetAudioOutput (a ring and a resampler, moltnet_audio_output.h).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_SINK_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_SINK_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/output_device_info.h"
#include "third_party/blink/public/platform/audio/web_audio_device_source_type.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace media {
class AudioParameters;
}

namespace blink::molt_fingerprinting {

class CORE_EXPORT MoltNetMediaAudioSink final : public media::AudioRendererSink {
 public:
  // Makes a new real output device for the sink's device ID (authorization
  // requested); the sink makes one for every attempt to open it. Called on
  // the clock's thread and on the client's.
  using CreateDeviceCB =
      base::RepeatingCallback<scoped_refptr<media::AudioRendererSink>()>;

  // `device_id`: the device the client asked for ("" or "default" for the
  // default device). `clock_task_runner`: the thread the clock renders on.
  MoltNetMediaAudioSink(
      CreateDeviceCB create_device,
      const std::string& device_id,
      scoped_refptr<base::SingleThreadTaskRunner> clock_task_runner);
  MoltNetMediaAudioSink(const MoltNetMediaAudioSink&) = delete;
  MoltNetMediaAudioSink& operator=(const MoltNetMediaAudioSink&) = delete;

  // media::AudioRendererSink. Any thread but the real device's; Pause() and
  // Stop() may also be called from inside the client's Render() (the mixer
  // pauses its sink after ten seconds without inputs).
  void Initialize(const media::AudioParameters& params,
                  RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  bool SetVolume(double volume) override;
  media::OutputDeviceInfo GetOutputDeviceInfo() override;
  void GetOutputDeviceInfoAsync(OutputDeviceInfoCB info_cb) override;
  bool IsOptimizedForHardwareParameters() override;
  bool CurrentThreadIsRenderingThread() override;

  // For tests: how often a real device was opened, and how much audio waits
  // for it (the clock's thread).
  int DeviceOpensForTesting() const;
  int RingLevelForTesting() const;

 private:
  class Engine;
  ~MoltNetMediaAudioSink() override;

  const scoped_refptr<Engine> engine_;
};

// Whether the output device of a `source_type` stream is a
// MoltNetMediaAudioSink: in MoltNet, for media elements and MediaStream
// tracks played in one (Web Audio has its own, moltnet_audio_device.h; WebRTC
// is off in MoltNet).
CORE_EXPORT bool UsesUniformMediaClock(WebAudioDeviceSourceType source_type);

// The sink AudioDeviceFactory hands out in MoltNet, rendering on the clock
// thread MoltNet's media sinks share (created on first use, for the life of
// the process).
CORE_EXPORT scoped_refptr<media::AudioRendererSink> CreateMoltNetMediaAudioSink(
    MoltNetMediaAudioSink::CreateDeviceCB create_device,
    const std::string& device_id);

// HTMLMediaElement.setSinkId() for an element without a player: in MoltNet,
// OK for the default device ("" or "default") on every machine, without
// looking for it; nullopt otherwise (the device answers for itself).
CORE_EXPORT std::optional<media::OutputDeviceStatus> UniformDefaultOutputStatus(
    const std::string& sink_id);

// The device info a MoltNet media sink reports for the default device (and,
// with the device's own status, for a chosen one).
CORE_EXPORT media::OutputDeviceInfo UniformMediaOutputDeviceInfo(
    const std::string& device_id,
    media::OutputDeviceStatus status);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_AUDIO_SINK_H_
