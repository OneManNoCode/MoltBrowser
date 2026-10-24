// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_sink.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "media/base/fake_audio_worker.h"
#include "media/base/output_device_info.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/audio/web_audio_device_source_type.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"

namespace blink::molt_fingerprinting {
namespace {

// What the real devices a sink opens share with the test (they are made on
// the clock's thread).
struct Devices {
  base::Lock lock;
  int sample_rate = 44100;
  int frames = 441;
  media::OutputDeviceStatus status = media::OUTPUT_DEVICE_STATUS_OK;
  int created = 0;
  int starts = 0;
  int stops = 0;
  media::AudioParameters params;
  raw_ptr<media::AudioRendererSink::RenderCallback> callback = nullptr;
};

// The real output device: whatever rate, buffer and status the test gives
// it; it never asks for audio unless the test does.
class FakeDevice final : public media::AudioRendererSink {
 public:
  explicit FakeDevice(Devices* devices) : devices_(devices) {}

  void Initialize(const media::AudioParameters& params,
                  RenderCallback* callback) override {
    base::AutoLock locker(devices_->lock);
    devices_->params = params;
    devices_->callback = callback;
  }
  void Start() override {
    base::AutoLock locker(devices_->lock);
    ++devices_->starts;
  }
  void Stop() override {
    base::AutoLock locker(devices_->lock);
    ++devices_->stops;
    devices_->callback = nullptr;
  }
  void Pause() override {}
  void Play() override {}
  void Flush() override {}
  bool SetVolume(double volume) override { return true; }
  media::OutputDeviceInfo GetOutputDeviceInfo() override { return Info(); }
  void GetOutputDeviceInfoAsync(OutputDeviceInfoCB info_cb) override {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(info_cb), Info()));
  }
  bool IsOptimizedForHardwareParameters() override { return true; }
  bool CurrentThreadIsRenderingThread() override { return false; }

 private:
  ~FakeDevice() override = default;

  media::OutputDeviceInfo Info() {
    base::AutoLock locker(devices_->lock);
    return media::OutputDeviceInfo(
        "", devices_->status,
        media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                               media::ChannelLayoutConfig::Stereo(),
                               devices_->sample_rate, devices_->frames));
  }

  const raw_ptr<Devices> devices_;
};

scoped_refptr<media::AudioRendererSink> NewDevice(Devices* devices) {
  {
    base::AutoLock locker(devices->lock);
    ++devices->created;
  }
  return base::MakeRefCounted<FakeDevice>(devices);
}

// The sink's client (the mixer, in a renderer): writes a ramp, keeps what
// it was told.
class Client final : public media::AudioRendererSink::RenderCallback {
 public:
  int Render(base::TimeDelta delay,
             base::TimeTicks delay_timestamp,
             const media::AudioGlitchInfo& glitch_info,
             media::AudioBus* dest) override {
    base::AutoLock locker(lock_);
    for (int c = 0; c < dest->channels(); ++c) {
      float value = static_cast<float>(frames_ + 1);
      for (float& sample : dest->channel(c)) {
        sample = value;
        value += 1;
      }
    }
    frames_ += dest->frames();
    delays_.push_back(delay);
    glitches_ += glitch_info.count;
    return dest->frames();
  }
  void OnRenderError() override {
    base::AutoLock locker(lock_);
    ++errors_;
  }

  int renders() {
    base::AutoLock locker(lock_);
    return static_cast<int>(delays_.size());
  }
  int errors() {
    base::AutoLock locker(lock_);
    return errors_;
  }
  int glitches() {
    base::AutoLock locker(lock_);
    return glitches_;
  }
  std::vector<base::TimeDelta> delays() {
    base::AutoLock locker(lock_);
    return delays_;
  }

 private:
  base::Lock lock_;
  int frames_ = 0;
  int errors_ = 0;
  int glitches_ = 0;
  std::vector<base::TimeDelta> delays_;
};

// What a media element's mixer asks for: 48 kHz stereo, 1024 frames.
media::AudioParameters MixerParams() {
  return media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                media::ChannelLayoutConfig::Stereo(), 48000,
                                1024);
}

class MoltNetAudioSinkTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetMediaForTesting(true);
  }
  void TearDown() override {
    SetMoltNetMediaForTesting(false);
    ResetForTesting();
  }

  scoped_refptr<media::AudioRendererSink> CreateSink(
      const std::string& device_id = std::string()) {
    return CreateMoltNetMediaAudioSink(
        base::BindRepeating(&NewDevice, base::Unretained(&devices_)),
        device_id);
  }

  // Runs the main thread for `time` of real time.
  void RunFor(base::TimeDelta time) {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), time);
    run_loop.Run();
  }

  media::AudioRendererSink::RenderCallback* DeviceCallback() {
    base::AutoLock locker(devices_.lock);
    return devices_.callback;
  }

  test::TaskEnvironment task_environment_;
  Devices devices_;
  Client client_;
};

TEST_F(MoltNetAudioSinkTest, WhichStreams) {
  EXPECT_TRUE(UsesUniformMediaClock(WebAudioDeviceSourceType::kMediaElement));
  EXPECT_TRUE(
      UsesUniformMediaClock(WebAudioDeviceSourceType::kNonRtcAudioTrack));
  EXPECT_FALSE(UsesUniformMediaClock(WebAudioDeviceSourceType::kWebRtc));
  EXPECT_FALSE(
      UsesUniformMediaClock(WebAudioDeviceSourceType::kWebAudioInteractive));
  SetMoltNetMediaForTesting(false);
  EXPECT_FALSE(UsesUniformMediaClock(WebAudioDeviceSourceType::kMediaElement));
  EXPECT_FALSE(
      UsesUniformMediaClock(WebAudioDeviceSourceType::kNonRtcAudioTrack));
}

TEST_F(MoltNetAudioSinkTest, DefaultSinkIdStatus) {
  EXPECT_EQ(UniformDefaultOutputStatus(""), media::OUTPUT_DEVICE_STATUS_OK);
  EXPECT_EQ(UniformDefaultOutputStatus("default"),
            media::OUTPUT_DEVICE_STATUS_OK);
  EXPECT_EQ(UniformDefaultOutputStatus("some-device"), std::nullopt);
  SetMoltNetMediaForTesting(false);
  EXPECT_EQ(UniformDefaultOutputStatus(""), std::nullopt);
}

// The default device is the uniform one, found or not, without asking it.
TEST_F(MoltNetAudioSinkTest, ReportsTheUniformDevice) {
  for (const media::OutputDeviceStatus status :
       {media::OUTPUT_DEVICE_STATUS_OK,
        media::OUTPUT_DEVICE_STATUS_ERROR_NOT_FOUND}) {
    {
      base::AutoLock locker(devices_.lock);
      devices_.status = status;
    }
    scoped_refptr<media::AudioRendererSink> sink = CreateSink();
    const media::OutputDeviceInfo info = sink->GetOutputDeviceInfo();
    EXPECT_EQ(info.device_status(), media::OUTPUT_DEVICE_STATUS_OK);
    EXPECT_EQ(info.output_params().sample_rate(), 48000);
    EXPECT_EQ(info.output_params().frames_per_buffer(), 480);
    EXPECT_EQ(info.output_params().channels(), 2);

    std::optional<media::OutputDeviceInfo> async;
    base::RunLoop run_loop;
    sink->GetOutputDeviceInfoAsync(
        base::BindOnce(
            [](std::optional<media::OutputDeviceInfo>* out,
               base::OnceClosure quit, media::OutputDeviceInfo info) {
              *out = info;
              std::move(quit).Run();
            },
            &async, run_loop.QuitClosure()));
    // Posted, never answered from inside the call.
    EXPECT_FALSE(async.has_value());
    run_loop.Run();
    ASSERT_TRUE(async.has_value());
    EXPECT_EQ(async->device_status(), media::OUTPUT_DEVICE_STATUS_OK);
    EXPECT_EQ(async->output_params().sample_rate(), 48000);
    EXPECT_EQ(async->output_params().frames_per_buffer(), 480);
    base::AutoLock locker(devices_.lock);
    EXPECT_EQ(devices_.created, 0);
  }
}

// The client renders its buffer every buffer's duration, with a fixed delay,
// while the real device asks for nothing.
TEST_F(MoltNetAudioSinkTest, RendersOnItsOwnClock) {
  scoped_refptr<media::AudioRendererSink> sink = CreateSink();
  sink->Initialize(MixerParams(), &client_);
  sink->Start();
  sink->Play();
  RunFor(base::Milliseconds(320));
  sink->Stop();
  // 1024 frames at 48 kHz: every 21.3 ms, 15 in 320 ms.
  const int renders = client_.renders();
  EXPECT_GE(renders, 7);
  EXPECT_LE(renders, 30);
  for (const base::TimeDelta delay : client_.delays()) {
    EXPECT_EQ(delay,
              media::FakeAudioWorker::ComputeFakeOutputDelay(MixerParams()) +
                  base::Milliseconds(30));
  }
  EXPECT_EQ(client_.glitches(), 0);
  EXPECT_EQ(client_.errors(), 0);
  {
    base::AutoLock locker(devices_.lock);
    EXPECT_EQ(devices_.created, 1);
    EXPECT_EQ(devices_.starts, 1);
    // At the device's own rate, in Chromium's buffer for playback.
    EXPECT_EQ(devices_.params.sample_rate(), 44100);
  }
  // Nothing renders after Stop().
  RunFor(base::Milliseconds(60));
  EXPECT_EQ(client_.renders(), renders);
}

TEST_F(MoltNetAudioSinkTest, TheRealDevicePlaysWhatWasRendered) {
  {
    base::AutoLock locker(devices_.lock);
    devices_.sample_rate = 48000;
    devices_.frames = 256;
  }
  scoped_refptr<media::AudioRendererSink> sink = CreateSink();
  sink->Initialize(MixerParams(), &client_);
  sink->Start();
  RunFor(base::Milliseconds(100));
  media::AudioRendererSink::RenderCallback* device = DeviceCallback();
  ASSERT_TRUE(device);
  int frames = 0;
  {
    base::AutoLock locker(devices_.lock);
    frames = devices_.params.frames_per_buffer();
  }
  ASSERT_GT(frames, 0);
  std::unique_ptr<media::AudioBus> bus = media::AudioBus::Create(2, frames);
  int heard = 0;
  float first = 0;
  float last = 0;
  const base::TimeDelta period = base::Seconds(frames / 48000.0);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(device->Render(base::Milliseconds(20), base::TimeTicks::Now(),
                             {}, bus.get()),
              frames);
    for (const float sample : bus->channel(0)) {
      if (sample != 0) {
        first = heard ? first : sample;
        last = sample;
        ++heard;
      }
    }
    base::PlatformThread::Sleep(period);
  }
  sink->Stop();
  EXPECT_GT(heard, frames * 8);
  // The ramp went on (through the resampler's filter, so only roughly).
  EXPECT_GT(last - first, frames * 5);
}

// A machine without an output device times playback as one with it.
TEST_F(MoltNetAudioSinkTest, NoDeviceIsNoError) {
  {
    base::AutoLock locker(devices_.lock);
    devices_.status = media::OUTPUT_DEVICE_STATUS_ERROR_NOT_FOUND;
  }
  scoped_refptr<media::AudioRendererSink> sink = CreateSink();
  EXPECT_EQ(sink->GetOutputDeviceInfo().device_status(),
            media::OUTPUT_DEVICE_STATUS_OK);
  sink->Initialize(MixerParams(), &client_);
  sink->Start();
  RunFor(base::Milliseconds(200));
  sink->Stop();
  EXPECT_GE(client_.renders(), 4);
  EXPECT_EQ(client_.errors(), 0);
  base::AutoLock locker(devices_.lock);
  EXPECT_EQ(devices_.starts, 0);
}

// The default device goes away: the page hears of nothing, rendering goes
// on, and a new device is opened after a second.
TEST_F(MoltNetAudioSinkTest, ALostDefaultDeviceIsNoError) {
  scoped_refptr<media::AudioRendererSink> sink = CreateSink();
  auto* moltnet_sink = static_cast<MoltNetMediaAudioSink*>(sink.get());
  sink->Initialize(MixerParams(), &client_);
  sink->Start();
  RunFor(base::Milliseconds(60));
  media::AudioRendererSink::RenderCallback* device = DeviceCallback();
  ASSERT_TRUE(device);
  EXPECT_EQ(moltnet_sink->DeviceOpensForTesting(), 1);
  device->OnRenderError();
  RunFor(base::Milliseconds(100));
  EXPECT_EQ(client_.errors(), 0);
  const int before = client_.renders();
  RunFor(base::Milliseconds(1300));
  EXPECT_EQ(moltnet_sink->DeviceOpensForTesting(), 2);
  EXPECT_GT(client_.renders(), before + 40);
  sink->Stop();
  EXPECT_EQ(client_.errors(), 0);
}

// A device the page chose answers for itself: its status and its errors,
// with the clock's parameters.
TEST_F(MoltNetAudioSinkTest, AChosenDeviceAnswersForItself) {
  {
    base::AutoLock locker(devices_.lock);
    devices_.status = media::OUTPUT_DEVICE_STATUS_ERROR_NOT_AUTHORIZED;
  }
  scoped_refptr<media::AudioRendererSink> sink = CreateSink("some-device");
  const media::OutputDeviceInfo info = sink->GetOutputDeviceInfo();
  EXPECT_EQ(info.device_status(),
            media::OUTPUT_DEVICE_STATUS_ERROR_NOT_AUTHORIZED);
  EXPECT_EQ(info.output_params().sample_rate(), 48000);
  EXPECT_EQ(info.output_params().frames_per_buffer(), 480);
  sink->Initialize(MixerParams(), &client_);
  sink->Start();
  RunFor(base::Milliseconds(100));
  sink->Stop();
  EXPECT_EQ(client_.errors(), 1);
}

TEST_F(MoltNetAudioSinkTest, PauseAndPlay) {
  scoped_refptr<media::AudioRendererSink> sink = CreateSink();
  sink->Initialize(MixerParams(), &client_);
  sink->Start();
  RunFor(base::Milliseconds(80));
  sink->Pause();
  const int paused_at = client_.renders();
  RunFor(base::Milliseconds(80));
  EXPECT_EQ(client_.renders(), paused_at);
  sink->Play();
  RunFor(base::Milliseconds(80));
  EXPECT_GT(client_.renders(), paused_at);
  sink->Stop();
}

}  // namespace
}  // namespace blink::molt_fingerprinting
