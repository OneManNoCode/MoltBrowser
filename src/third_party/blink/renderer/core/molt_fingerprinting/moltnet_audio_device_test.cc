// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_audio_device.h"

#include <atomic>
#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/synchronization/lock.h"
#include "base/test/test_timeouts.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/output_device_info.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_audio_device.h"
#include "third_party/blink/public/platform/web_audio_latency_hint.h"
#include "third_party/blink/public/platform/web_audio_sink_descriptor.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"
#include "third_party/blink/renderer/platform/audio/audio_destination.h"
#include "third_party/blink/renderer/platform/audio/audio_io_callback.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/testing/testing_platform_support.h"

namespace blink::molt_fingerprinting {
namespace {

const LocalFrameToken kFrameToken;

// The real output device: whatever rate and buffer the test gives it.
class FakeOutputDevice final : public WebAudioDevice {
 public:
  FakeOutputDevice(double sample_rate,
                   int frames_per_buffer,
                   media::OutputDeviceStatus status)
      : sample_rate_(sample_rate),
        frames_per_buffer_(frames_per_buffer),
        status_(status) {}

  void Start() override { ++starts; }
  void Stop() override { ++stops; }
  void Pause() override { ++pauses; }
  void Resume() override { ++resumes; }
  double SampleRate() override { return sample_rate_; }
  int FramesPerBuffer() override { return frames_per_buffer_; }
  int MaxChannelCount() override { return 8; }
  void SetDetectSilence(bool detect_silence) override {}
  media::OutputDeviceStatus MaybeCreateSinkAndGetStatus() override {
    return status_;
  }

  int starts = 0;
  int stops = 0;
  int pauses = 0;
  int resumes = 0;

 private:
  const double sample_rate_;
  const int frames_per_buffer_;
  const media::OutputDeviceStatus status_;
};

class TestPlatform final : public TestingPlatformSupport {
 public:
  std::unique_ptr<WebAudioDevice> CreateAudioDevice(
      const WebAudioSinkDescriptor& sink_descriptor,
      unsigned number_of_output_channels,
      const WebAudioLatencyHint& latency_hint,
      std::optional<float> context_sample_rate,
      media::AudioRendererSink::RenderCallback* callback) override {
    ++devices_created;
    last_callback = callback;
    auto device = std::make_unique<FakeOutputDevice>(sample_rate, frames,
                                                     status);
    last_device = device.get();
    return device;
  }

  double sample_rate = 44100;
  int frames = 1024;
  media::OutputDeviceStatus status = media::OUTPUT_DEVICE_STATUS_OK;
  int devices_created = 0;
  raw_ptr<media::AudioRendererSink::RenderCallback> last_callback = nullptr;
  raw_ptr<FakeOutputDevice> last_device = nullptr;
};

// The context's graph: counts render quanta and writes a ramp.
class Graph final : public AudioIOCallback {
 public:
  void Render(AudioBus* bus,
              uint32_t frames,
              const AudioIOPosition&,
              const AudioCallbackMetric&,
              base::TimeDelta delay,
              const media::AudioGlitchInfo& glitch_info) override {
    for (unsigned c = 0; c < bus->NumberOfChannels(); ++c) {
      float value = static_cast<float>(rendered_frames_.load());
      for (float& sample : bus->Channel(c)->MutableSpan().first(frames)) {
        sample = value;
        value += 1;
      }
    }
    rendered_frames_ += frames;
    ++quanta_;
    base::AutoLock locker(lock_);
    glitches_ += glitch_info.count;
  }
  void OnRenderError() override { ++errors_; }

  int quanta() const { return quanta_.load(); }
  int errors() const { return errors_; }
  int glitches() {
    base::AutoLock locker(lock_);
    return glitches_;
  }

 private:
  std::atomic<int> quanta_{0};
  std::atomic<uint32_t> rendered_frames_{0};
  int errors_ = 0;
  base::Lock lock_;
  int glitches_ = 0;
};

class MoltNetAudioDeviceTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetMediaForTesting(true);
  }
  void TearDown() override {
    SetMoltNetMediaForTesting(false);
    ResetForTesting();
  }

  scoped_refptr<AudioDestination> CreateDestination(
      const WebAudioSinkDescriptor& sink,
      WebAudioLatencyHint hint =
          WebAudioLatencyHint(WebAudioLatencyHint::kCategoryInteractive),
      std::optional<float> rate = 48000.0f) {
    return AudioDestination::Create(graph_, sink, 2, hint, rate, 128);
  }

  static WebAudioSinkDescriptor DefaultSink() {
    return WebAudioSinkDescriptor(WebString(""), kFrameToken);
  }

  // Runs the main thread for `time` of real time.
  void RunFor(base::TimeDelta time) {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), time);
    run_loop.Run();
  }

  test::TaskEnvironment task_environment_;
  ScopedTestingPlatformSupport<TestPlatform> platform_;
  Graph graph_;
};

TEST_F(MoltNetAudioDeviceTest, ReportsTheUniformDevice) {
  for (const auto& [rate, frames] :
       std::vector<std::pair<double, int>>{{48000, 128},
                                           {48000, 256},
                                           {44100, 441},
                                           {48000, 512},
                                           {96000, 1024},
                                           {48000, 2048}}) {
    platform_->sample_rate = rate;
    platform_->frames = frames;
    scoped_refptr<AudioDestination> destination =
        CreateDestination(DefaultSink());
    ASSERT_TRUE(destination);
    EXPECT_EQ(destination->SampleRate(), 48000);
    EXPECT_EQ(destination->CallbackBufferSize(), 480u);
    EXPECT_EQ(destination->FramesPerBuffer(), 480);
    EXPECT_EQ(destination->GetPlatformBufferDuration(), base::Milliseconds(10));
    EXPECT_EQ(destination->MaxChannelCount(), 2u);
    EXPECT_EQ(destination->MaybeCreateSinkAndGetStatus(),
              media::OUTPUT_DEVICE_STATUS_OK);
  }
  // Other latency hints and rates follow the uniform device's rule.
  scoped_refptr<AudioDestination> playback = CreateDestination(
      DefaultSink(), WebAudioLatencyHint(WebAudioLatencyHint::kCategoryPlayback));
  EXPECT_EQ(playback->FramesPerBuffer(), 1024);
  scoped_refptr<AudioDestination> cd = CreateDestination(
      DefaultSink(),
      WebAudioLatencyHint(WebAudioLatencyHint::kCategoryInteractive), 44100.0f);
  EXPECT_EQ(cd->FramesPerBuffer(), 441);
  EXPECT_EQ(cd->SampleRate(), 44100);
}

TEST_F(MoltNetAudioDeviceTest, RendersOnItsOwnClock) {
  platform_->sample_rate = 44100;
  platform_->frames = 2048;
  scoped_refptr<AudioDestination> destination =
      CreateDestination(DefaultSink());
  destination->Start();
  ASSERT_EQ(platform_->last_device->starts, 1);
  // The real device never asks for audio, yet the graph renders: 480 frames
  // every 10 ms, 3.75 quanta.
  RunFor(base::Milliseconds(300));
  destination->Stop();
  const int quanta = graph_.quanta();
  EXPECT_GE(quanta, 3.75 * 20);
  EXPECT_LE(quanta, 3.75 * 40);
  EXPECT_EQ(platform_->last_device->stops, 1);
  EXPECT_EQ(graph_.glitches(), 0);
  // Nothing renders after Stop().
  RunFor(base::Milliseconds(50));
  EXPECT_EQ(graph_.quanta(), quanta);
}

TEST_F(MoltNetAudioDeviceTest, TheRealDevicePlaysWhatWasRendered) {
  platform_->sample_rate = 48000;
  platform_->frames = 256;
  scoped_refptr<AudioDestination> destination =
      CreateDestination(DefaultSink());
  media::AudioRendererSink::RenderCallback* device_callback =
      platform_->last_callback;
  destination->Start();
  RunFor(base::Milliseconds(100));
  // Once the ring has filled, the device's callbacks get the rendered ramp
  // (through the resampler's filter, so only roughly the same values).
  std::unique_ptr<media::AudioBus> bus = media::AudioBus::Create(2, 256);
  int heard = 0;
  float first = 0;
  float last = 0;
  for (int i = 0; i < 40; ++i) {
    EXPECT_EQ(device_callback->Render(base::Milliseconds(20),
                                      base::TimeTicks::Now(), {}, bus.get()),
              256);
    for (const float sample : bus->channel(0)) {
      if (sample != 0) {
        first = heard ? first : sample;
        last = sample;
        ++heard;
      }
    }
    base::PlatformThread::Sleep(base::Microseconds(5333));
  }
  destination->Stop();
  EXPECT_GT(heard, 256 * 20);
  // About 200 ms of the ramp went by.
  EXPECT_GT(last - first, 48 * 100);
}

TEST_F(MoltNetAudioDeviceTest, ALostDefaultDeviceIsNoError) {
  platform_->status = media::OUTPUT_DEVICE_STATUS_ERROR_NOT_FOUND;
  scoped_refptr<AudioDestination> destination =
      CreateDestination(DefaultSink());
  auto* device = static_cast<MoltNetAudioDevice*>(
      static_cast<media::AudioRendererSink::RenderCallback*>(
          platform_->last_callback.get()));
  // A machine without a device answers like one with the uniform device.
  EXPECT_EQ(destination->MaybeCreateSinkAndGetStatus(),
            media::OUTPUT_DEVICE_STATUS_OK);
  destination->Start();
  EXPECT_EQ(device->OutputDeviceStartsForTesting(), 1);
  // The device fails: the page hears of nothing, the graph keeps rendering.
  device->OnRenderError();
  EXPECT_EQ(graph_.errors(), 0);
  EXPECT_EQ(platform_->last_device->stops, 1);
  const int before = graph_.quanta();
  // The device is opened again after a second.
  RunFor(base::Milliseconds(1300));
  EXPECT_EQ(device->OutputDeviceStartsForTesting(), 2);
  EXPECT_GT(graph_.quanta(), before + 3 * 100);
  destination->Stop();
  EXPECT_EQ(graph_.errors(), 0);
}

TEST_F(MoltNetAudioDeviceTest, AChosenDeviceAnswersForItself) {
  platform_->status = media::OUTPUT_DEVICE_STATUS_ERROR_NOT_AUTHORIZED;
  scoped_refptr<AudioDestination> destination = CreateDestination(
      WebAudioSinkDescriptor(WebString("some-device"), kFrameToken));
  EXPECT_EQ(destination->MaybeCreateSinkAndGetStatus(),
            media::OUTPUT_DEVICE_STATUS_ERROR_NOT_AUTHORIZED);
  destination->Start();
  static_cast<media::AudioRendererSink::RenderCallback*>(
      platform_->last_callback.get())
      ->OnRenderError();
  EXPECT_EQ(graph_.errors(), 1);
  destination->Stop();
}

TEST_F(MoltNetAudioDeviceTest, SilentSinksHaveNoDevice) {
  scoped_refptr<AudioDestination> destination =
      CreateDestination(WebAudioSinkDescriptor(kFrameToken));
  EXPECT_EQ(platform_->devices_created, 0);
  EXPECT_EQ(destination->FramesPerBuffer(), 480);
  EXPECT_EQ(destination->MaybeCreateSinkAndGetStatus(),
            media::OUTPUT_DEVICE_STATUS_OK);
  destination->Start();
  RunFor(base::Milliseconds(100));
  destination->Stop();
  EXPECT_GT(graph_.quanta(), 3 * 5);
}

TEST_F(MoltNetAudioDeviceTest, PauseAndResume) {
  scoped_refptr<AudioDestination> destination =
      CreateDestination(DefaultSink());
  destination->Start();
  RunFor(base::Milliseconds(50));
  destination->Pause();
  EXPECT_EQ(platform_->last_device->pauses, 1);
  const int paused_at = graph_.quanta();
  RunFor(base::Milliseconds(50));
  EXPECT_EQ(graph_.quanta(), paused_at);
  destination->Resume();
  EXPECT_EQ(platform_->last_device->resumes, 1);
  RunFor(base::Milliseconds(50));
  EXPECT_GT(graph_.quanta(), paused_at);
  destination->Stop();
}

TEST_F(MoltNetAudioDeviceTest, NormalWindowsUseTheDevice) {
  SetMoltNetMediaForTesting(false);
  platform_->sample_rate = 44100;
  platform_->frames = 512;
  scoped_refptr<AudioDestination> destination = CreateDestination(
      DefaultSink(),
      WebAudioLatencyHint(WebAudioLatencyHint::kCategoryInteractive),
      std::nullopt);
  EXPECT_EQ(destination->FramesPerBuffer(), 512);
  EXPECT_EQ(destination->SampleRate(), 44100);
  // The device drives the destination directly.
  EXPECT_EQ(platform_->last_callback.get(),
            static_cast<media::AudioRendererSink::RenderCallback*>(
                destination.get()));
}

}  // namespace
}  // namespace blink::molt_fingerprinting
