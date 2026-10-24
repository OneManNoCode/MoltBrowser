// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_sink.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "media/audio/audio_device_description.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_latency.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "media/base/fake_audio_worker.h"
#include "media/base/output_device_info.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_output.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.h"
#include "third_party/blink/renderer/platform/scheduler/public/non_main_thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink::molt_fingerprinting {

namespace {

// How long a default device that failed stays closed, the first time and at
// most (as Web Audio's, moltnet_audio_device.cc).
constexpr base::TimeDelta kFirstRetry = base::Seconds(1);
constexpr base::TimeDelta kMaxRetry = base::Seconds(30);

// Added to the delay the client is told (two of its buffers, as Chromium's
// NullAudioSink): about what the ring (MoltNetAudioOutput fills it to one
// of the real device's callbacks and a cushion before it plays) and a
// typical wired output add on top, measured with the simulated devices of
// the media-clock harness. So on such an output the picture of a video and
// its sound line up within a few milliseconds; with a slower output (a
// Bluetooth headset) the sound comes that much later than the picture. It
// is the same on every machine: the real latency stays out of the timing.
constexpr base::TimeDelta kOutputPathDelay = base::Milliseconds(30);

// The realtime period the shared clock thread asks for (macOS): the uniform
// device's buffer, the shortest tick a media sink runs.
constexpr base::TimeDelta kClockPeriod = base::Milliseconds(10);

// The real device's parameters: its own rate, the client's channels, and the
// buffer Chromium asks a device for at the client's latency.
media::AudioParameters RealDeviceParams(const media::AudioParameters& client,
                                        const media::AudioParameters& hardware) {
  const bool usable = hardware.IsValid() &&
                      hardware.format() != media::AudioParameters::AUDIO_FAKE;
  const int rate = usable ? hardware.sample_rate() : client.sample_rate();
  const int hardware_frames = usable ? hardware.frames_per_buffer() : 0;
  int frames = 0;
  switch (client.latency_tag()) {
    case media::AudioLatency::Type::kPlayback:
      frames = media::AudioLatency::GetHighLatencyBufferSize(rate,
                                                             hardware_frames);
      break;
    case media::AudioLatency::Type::kRtc:
      frames = media::AudioLatency::GetRtcBufferSize(rate, hardware_frames);
      break;
    default:
      frames = hardware_frames > 0 ? hardware_frames : rate / 100;
      break;
  }
  media::AudioParameters params(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                client.channel_layout_config(), rate,
                                std::max(frames, 1));
  params.set_effects(client.effects());
  params.set_latency_tag(client.latency_tag());
  return params;
}

scoped_refptr<base::SingleThreadTaskRunner> MediaClockTaskRunner() {
  // One realtime thread renders every media sink of the process, as one
  // device thread renders all of a page's media elements through their
  // mixer. Lives as long as the process.
  static NonMainThread* const thread = [] {
    ThreadCreationParams params(ThreadType::kRealtimeAudioWorkletThread);
    params.base_thread_type = base::ThreadType::kRealtimeAudio;
    params.realtime_period = kClockPeriod;
    return NonMainThread::CreateThread(params).release();
  }();
  return thread->GetTaskRunner();
}

}  // namespace

// Everything a sink does, kept alive by the tasks it posts to the clock's
// thread (the sink itself has one owner, as the mixer expects).
class MoltNetMediaAudioSink::Engine final
    : public base::RefCountedThreadSafe<MoltNetMediaAudioSink::Engine> {
 public:
  Engine(CreateDeviceCB create_device,
         std::string device_id,
         scoped_refptr<base::SingleThreadTaskRunner> clock_runner)
      : create_device_(std::move(create_device)),
        device_id_(std::move(device_id)),
        chosen_device_(
            !media::AudioDeviceDescription::IsDefaultDevice(device_id_)),
        clock_runner_(std::move(clock_runner)) {}
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // The client's side.

  void Initialize(const media::AudioParameters& params,
                  RenderCallback* client) {
    DCHECK(params.IsValid());
    DCHECK(!params.IsBitstreamFormat());
    base::AutoLock locker(lock_);
    DCHECK(!started_);
    params_ = params;
    client_ = client;
  }

  void Start() {
    uint64_t generation = 0;
    {
      base::AutoLock locker(lock_);
      DCHECK(client_);
      if (started_ || !client_) {
        return;
      }
      // Sinks play on start, like an AudioOutputDevice's stream.
      started_ = true;
      playing_ = true;
      generation = ++generation_;
    }
    PostToClock(CrossThreadBindOnce(&Engine::OnStart, WrapRefCounted(this),
                                    generation));
  }

  void Stop() {
    {
      base::AutoLock locker(lock_);
      if (!started_ && !client_) {
        return;
      }
      started_ = false;
      playing_ = false;
      client_ = nullptr;
      ++generation_;
    }
    // No Render() and no OnRenderError() after this returns.
    WaitForTick();
    PostToClock(CrossThreadBindOnce(&Engine::OnStop, WrapRefCounted(this)));
  }

  void Play() {
    uint64_t generation = 0;
    {
      base::AutoLock locker(lock_);
      if (!started_ || playing_) {
        return;
      }
      playing_ = true;
      generation = ++generation_;
    }
    PostToClock(CrossThreadBindOnce(&Engine::OnPlay, WrapRefCounted(this),
                                    generation));
  }

  void Pause() {
    {
      base::AutoLock locker(lock_);
      if (!playing_) {
        return;
      }
      playing_ = false;
      ++generation_;
    }
    WaitForTick();
    PostToClock(CrossThreadBindOnce(&Engine::OnPause, WrapRefCounted(this)));
  }

  void Flush() {
    PostToClock(CrossThreadBindOnce(&Engine::OnFlush, WrapRefCounted(this)));
  }

  bool SetVolume(double volume) {
    if (!(volume >= 0 && volume <= 1)) {
      return false;
    }
    {
      base::AutoLock locker(lock_);
      volume_ = volume;
    }
    PostToClock(
        CrossThreadBindOnce(&Engine::OnVolume, WrapRefCounted(this), volume));
    return true;
  }

  // The real device for this sink's ID (also for a probe of its status).
  scoped_refptr<media::AudioRendererSink> CreateDevice() const {
    return create_device_.Run();
  }
  const std::string& device_id() const { return device_id_; }
  bool chosen_device() const { return chosen_device_; }
  bool IsClockThread() const { return clock_runner_->BelongsToCurrentThread(); }

  int device_opens() const {
    return device_opens_.load(std::memory_order_relaxed);
  }
  int RingLevelForTesting() const {
    return output_ ? output_->RingLevelForTesting() : 0;
  }

 private:
  friend class base::RefCountedThreadSafe<Engine>;

  // The real device's callbacks, tagged with the attempt that opened it.
  class DeviceClient final : public media::AudioRendererSink::RenderCallback {
   public:
    DeviceClient(Engine* engine, uint64_t attempt)
        : engine_(engine), attempt_(attempt) {}

    // The device's audio thread. Nothing it passes (its buffer, delay,
    // timing, glitches) goes further.
    int Render(base::TimeDelta delay,
               base::TimeTicks delay_timestamp,
               const media::AudioGlitchInfo& glitch_info,
               media::AudioBus* dest) override {
      engine_->RenderDevice(dest);
      return dest->frames();
    }

    // Any thread (an AudioOutputDevice calls it on the IO thread, holding a
    // lock its Stop() takes): handled on the clock's thread.
    void OnRenderError() override {
      engine_->PostToClock(CrossThreadBindOnce(
          &Engine::OnDeviceError, WrapRefCounted(engine_.get()), attempt_));
    }

   private:
    const raw_ptr<Engine> engine_;
    const uint64_t attempt_;
  };

  ~Engine() {
    DCHECK(!device_);
    DCHECK(!worker_running_);
  }

  void PostToClock(CrossThreadOnceClosure task) {
    PostCrossThreadTask(*clock_runner_, FROM_HERE, std::move(task));
  }

  // Waits for a tick that is rendering to finish. Called after the
  // generation changed, so no later tick renders. Not on the clock's thread:
  // there the caller is the tick's client itself.
  void WaitForTick() {
    if (IsClockThread()) {
      return;
    }
    base::AutoLock wait(render_lock_);
  }

  // The clock's thread.

  void OnStart(uint64_t generation) {
    {
      base::AutoLock locker(lock_);
      if (!started_) {
        return;
      }
    }
    retry_delay_ = kFirstRetry;
    OpenDevice();
    OnPlay(generation);
  }

  void OnPlay(uint64_t generation) {
    media::AudioParameters params;
    {
      base::AutoLock locker(lock_);
      if (generation != generation_ || !playing_) {
        return;
      }
      params = params_;
    }
    StartClock(generation, params);
    if (device_) {
      // As Web Audio's resume: what the device still holds is gone, the
      // ring fills again from the clock's new schedule.
      output_->Restart();
      device_->Play();
    }
  }

  void OnPause() {
    StopClock();
    if (device_) {
      device_->Pause();
    }
  }

  void OnStop() {
    StopClock();
    CloseDevice();
    // Cancels a device being opened and any retry.
    ++device_attempt_;
    opening_ = false;
  }

  void OnFlush() {
    if (device_) {
      output_->Restart();
      device_->Flush();
    }
  }

  void OnVolume(double volume) {
    if (device_) {
      device_->SetVolume(volume);
    }
  }

  void StartClock(uint64_t generation, const media::AudioParameters& params) {
    StopClock();
    if (!worker_ || !worker_params_.Equals(params)) {
      worker_ = std::make_unique<media::FakeAudioWorker>(clock_runner_, params);
      worker_params_ = params;
      tick_bus_ = media::AudioBus::Create(params);
      tick_delay_ = media::FakeAudioWorker::ComputeFakeOutputDelay(params) +
                    kOutputPathDelay;
    }
    worker_->Start(ConvertToBaseRepeatingCallback(CrossThreadBindRepeating(
        &Engine::OnTick, WrapRefCounted(this), generation)));
    worker_running_ = true;
  }

  // Only from a task that holds a reference: the worker's callback may hold
  // the last other one.
  void StopClock() {
    if (worker_running_) {
      worker_->Stop();
      worker_running_ = false;
    }
  }

  // One tick: the client renders a buffer, which goes to the real device.
  void OnTick(uint64_t generation,
              base::TimeTicks ideal_time,
              base::TimeTicks now) {
    base::AutoLock render(render_lock_);
    RenderCallback* client = nullptr;
    {
      base::AutoLock locker(lock_);
      if (generation != generation_ || !playing_ || !client_) {
        return;
      }
      client = client_;
    }
    // The client may pause or stop the sink from here (the mixer does after
    // ten seconds without inputs); that only takes effect for later ticks.
    client->Render(tick_delay_, ideal_time, media::AudioGlitchInfo(),
                   tick_bus_.get());
    if (output_) {
      output_->Push(*tick_bus_, ideal_time);
    }
  }

  void OpenDevice() {
    if (device_ || opening_) {
      return;
    }
    const uint64_t attempt = ++device_attempt_;
    scoped_refptr<media::AudioRendererSink> device = CreateDevice();
    if (!device) {
      DeviceFailed(attempt);
      return;
    }
    opening_ = true;
    // Answers on this thread once the device is authorized (or not).
    device->GetOutputDeviceInfoAsync(
        ConvertToBaseOnceCallback(CrossThreadBindOnce(
            &Engine::OnDeviceInfo, WrapRefCounted(this), device, attempt)));
  }

  void OnDeviceInfo(scoped_refptr<media::AudioRendererSink> device,
                    uint64_t attempt,
                    media::OutputDeviceInfo info) {
    if (attempt != device_attempt_) {
      device->Stop();
      return;
    }
    opening_ = false;
    media::AudioParameters client_params;
    bool playing = false;
    double volume = 1;
    {
      base::AutoLock locker(lock_);
      if (!started_) {
        device->Stop();
        return;
      }
      client_params = params_;
      playing = playing_;
      volume = volume_;
    }
    if (info.device_status() != media::OUTPUT_DEVICE_STATUS_OK) {
      device->Stop();
      DeviceFailed(attempt);
      return;
    }
    const media::AudioParameters device_params =
        RealDeviceParams(client_params, info.output_params());
    output_ = std::make_unique<MoltNetAudioOutput>(
        client_params.channels(), client_params.sample_rate(),
        client_params.frames_per_buffer(), device_params.sample_rate(),
        device_params.frames_per_buffer());
    device_client_ = std::make_unique<DeviceClient>(this, attempt);
    device->Initialize(device_params, device_client_.get());
    output_->StartFeeding();
    device_ = std::move(device);
    device_->Start();
    device_->SetVolume(volume);
    if (!playing) {
      device_->Pause();
    }
    device_opens_.fetch_add(1, std::memory_order_relaxed);
  }

  void CloseDevice() {
    if (!device_) {
      return;
    }
    // Joins the device's audio thread: no Render() or OnRenderError() from
    // it after this.
    device_->Stop();
    device_ = nullptr;
    output_->StopFeeding();
    output_.reset();
    device_client_.reset();
  }

  void OnDeviceError(uint64_t attempt) {
    if (attempt != device_attempt_ || !device_) {
      return;
    }
    CloseDevice();
    DeviceFailed(attempt);
  }

  // The real device couldn't be opened, or went away.
  void DeviceFailed(uint64_t attempt) {
    if (chosen_device_) {
      // A device the page chose answers for itself, as in Chromium.
      base::AutoLock render(render_lock_);
      RenderCallback* client = nullptr;
      {
        base::AutoLock locker(lock_);
        client = client_;
      }
      if (client) {
        client->OnRenderError();
      }
      return;
    }
    // The default device never fails: the clock goes on, its output goes
    // nowhere until a device opens again.
    PostToClockDelayed(CrossThreadBindOnce(&Engine::RetryDevice,
                                           WrapRefCounted(this), attempt),
                       retry_delay_);
    retry_delay_ = std::min(retry_delay_ * 2, kMaxRetry);
  }

  void PostToClockDelayed(CrossThreadOnceClosure task, base::TimeDelta delay) {
    PostDelayedCrossThreadTask(*clock_runner_, FROM_HERE, std::move(task),
                               delay);
  }

  void RetryDevice(uint64_t attempt) {
    if (attempt != device_attempt_ || device_ || opening_) {
      return;
    }
    {
      base::AutoLock locker(lock_);
      if (!started_) {
        return;
      }
    }
    OpenDevice();
  }

  // The real device's audio thread: `output_` is set before the device
  // starts and reset only after it stopped.
  void RenderDevice(media::AudioBus* dest) {
    if (!output_) {
      dest->Zero();
      return;
    }
    output_->Pull(dest);
  }

  const CreateDeviceCB create_device_;
  const std::string device_id_;
  const bool chosen_device_;
  const scoped_refptr<base::SingleThreadTaskRunner> clock_runner_;

  // Held by a tick while its client renders.
  base::Lock render_lock_;

  base::Lock lock_;
  media::AudioParameters params_ GUARDED_BY(lock_);
  raw_ptr<RenderCallback> client_ GUARDED_BY(lock_) = nullptr;
  bool started_ GUARDED_BY(lock_) = false;
  bool playing_ GUARDED_BY(lock_) = false;
  double volume_ GUARDED_BY(lock_) = 1;
  // Changes with every start, stop, play and pause: a tick of an older one
  // renders nothing.
  uint64_t generation_ GUARDED_BY(lock_) = 0;

  // The clock's thread.
  std::unique_ptr<media::FakeAudioWorker> worker_;
  media::AudioParameters worker_params_;
  bool worker_running_ = false;
  std::unique_ptr<media::AudioBus> tick_bus_;
  base::TimeDelta tick_delay_;
  scoped_refptr<media::AudioRendererSink> device_;
  std::unique_ptr<DeviceClient> device_client_;
  std::unique_ptr<MoltNetAudioOutput> output_;
  uint64_t device_attempt_ = 0;
  bool opening_ = false;
  base::TimeDelta retry_delay_ = kFirstRetry;

  std::atomic<int> device_opens_{0};
};

MoltNetMediaAudioSink::MoltNetMediaAudioSink(
    CreateDeviceCB create_device,
    const std::string& device_id,
    scoped_refptr<base::SingleThreadTaskRunner> clock_task_runner)
    : engine_(base::MakeRefCounted<Engine>(std::move(create_device),
                                           device_id,
                                           std::move(clock_task_runner))) {}

MoltNetMediaAudioSink::~MoltNetMediaAudioSink() {
  engine_->Stop();
}

void MoltNetMediaAudioSink::Initialize(const media::AudioParameters& params,
                                       RenderCallback* callback) {
  engine_->Initialize(params, callback);
}

void MoltNetMediaAudioSink::Start() {
  engine_->Start();
}

void MoltNetMediaAudioSink::Stop() {
  engine_->Stop();
}

void MoltNetMediaAudioSink::Pause() {
  engine_->Pause();
}

void MoltNetMediaAudioSink::Play() {
  engine_->Play();
}

void MoltNetMediaAudioSink::Flush() {
  engine_->Flush();
}

bool MoltNetMediaAudioSink::SetVolume(double volume) {
  return engine_->SetVolume(volume);
}

media::OutputDeviceInfo MoltNetMediaAudioSink::GetOutputDeviceInfo() {
  if (!engine_->chosen_device()) {
    return UniformMediaOutputDeviceInfo(engine_->device_id(),
                                        media::OUTPUT_DEVICE_STATUS_OK);
  }
  // A chosen device's status is its own (blocking, as an AudioOutputDevice
  // answers); the parameters stay the clock's.
  scoped_refptr<media::AudioRendererSink> probe = engine_->CreateDevice();
  const media::OutputDeviceStatus status =
      probe ? probe->GetOutputDeviceInfo().device_status()
            : media::OUTPUT_DEVICE_STATUS_ERROR_INTERNAL;
  if (probe) {
    probe->Stop();
  }
  return UniformMediaOutputDeviceInfo(engine_->device_id(), status);
}

void MoltNetMediaAudioSink::GetOutputDeviceInfoAsync(
    OutputDeviceInfoCB info_cb) {
  if (!engine_->chosen_device()) {
    // Posted, as the contract requires; never waits for a real device.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        blink::BindOnce(std::move(info_cb),
                 UniformMediaOutputDeviceInfo(engine_->device_id(),
                                              media::OUTPUT_DEVICE_STATUS_OK)));
    return;
  }
  scoped_refptr<media::AudioRendererSink> probe = engine_->CreateDevice();
  if (!probe) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, blink::BindOnce(std::move(info_cb),
                            UniformMediaOutputDeviceInfo(
                                engine_->device_id(),
                                media::OUTPUT_DEVICE_STATUS_ERROR_INTERNAL)));
    return;
  }
  probe->GetOutputDeviceInfoAsync(blink::BindOnce(
      [](scoped_refptr<media::AudioRendererSink> probe, std::string device_id,
         OutputDeviceInfoCB info_cb, media::OutputDeviceInfo info) {
        probe->Stop();
        std::move(info_cb).Run(
            UniformMediaOutputDeviceInfo(device_id, info.device_status()));
      },
      probe, engine_->device_id(), std::move(info_cb)));
}

bool MoltNetMediaAudioSink::IsOptimizedForHardwareParameters() {
  return true;
}

bool MoltNetMediaAudioSink::CurrentThreadIsRenderingThread() {
  return engine_->IsClockThread();
}

int MoltNetMediaAudioSink::DeviceOpensForTesting() const {
  return engine_->device_opens();
}

int MoltNetMediaAudioSink::RingLevelForTesting() const {
  return engine_->RingLevelForTesting();
}

bool UsesUniformMediaClock(WebAudioDeviceSourceType source_type) {
  if (!HasUniformDefaultAudioOutput()) {
    return false;
  }
  return source_type == WebAudioDeviceSourceType::kMediaElement ||
         source_type == WebAudioDeviceSourceType::kNonRtcAudioTrack;
}

scoped_refptr<media::AudioRendererSink> CreateMoltNetMediaAudioSink(
    MoltNetMediaAudioSink::CreateDeviceCB create_device,
    const std::string& device_id) {
  return base::MakeRefCounted<MoltNetMediaAudioSink>(
      std::move(create_device), device_id, MediaClockTaskRunner());
}

std::optional<media::OutputDeviceStatus> UniformDefaultOutputStatus(
    const std::string& sink_id) {
  if (!HasUniformDefaultAudioOutput() ||
      !media::AudioDeviceDescription::IsDefaultDevice(sink_id)) {
    return std::nullopt;
  }
  return media::OUTPUT_DEVICE_STATUS_OK;
}

media::OutputDeviceInfo UniformMediaOutputDeviceInfo(
    const std::string& device_id,
    media::OutputDeviceStatus status) {
  return media::OutputDeviceInfo(
      device_id, status,
      media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                             media::ChannelLayoutConfig::Stereo(),
                             moltnet::kAudioDeviceSampleRate,
                             moltnet::kAudioDeviceFramesPerBuffer));
}

}  // namespace blink::molt_fingerprinting
