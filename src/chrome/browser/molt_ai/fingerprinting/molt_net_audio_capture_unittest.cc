// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser decides whether a renderer's microphone streams are processed
// CPU-independently (molt_net_audio_capture.h), and its decision replaces
// the renderer's in each stream's processing config
// (content/browser/renderer_host/media/molt_audio_capture_processing.h).

#include "chrome/browser/molt_ai/fingerprinting/molt_net_audio_capture.h"

#include "content/browser/renderer_host/media/molt_audio_capture_processing.h"
#include "media/base/audio_processing.h"
#include "media/mojo/mojom/audio_processing.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

// MoltNet's streams only; not while the kill switch is off.
TEST(MoltNetAudioCaptureTest, MoltNetProcessesCpuIndependently) {
  EXPECT_TRUE(ProcessesAudioCaptureCpuIndependently(RendererMode::kUniform));
  EXPECT_FALSE(ProcessesAudioCaptureCpuIndependently(RendererMode::kFarble));
  EXPECT_FALSE(ProcessesAudioCaptureCpuIndependently(RendererMode::kOff));
}

media::mojom::AudioProcessingConfigPtr RendererConfig(bool cpu_independent) {
  media::AudioProcessingSettings settings;
  settings.echo_cancellation = true;
  settings.noise_suppression = false;
  settings.automatic_gain_control = true;
  settings.multi_channel_capture_processing = true;
  settings.cpu_independent = cpu_independent;
  return media::mojom::AudioProcessingConfig::New(mojo::NullReceiver(),
                                                  settings);
}

// Whatever the renderer sent, the stream is processed as the browser
// decided for the renderer's profile, both ways; the other settings stay
// the renderer's.
TEST(MoltNetAudioCaptureTest, TheBrowserDecidesNotTheRenderer) {
  for (const bool renderer : {false, true}) {
    for (const bool browser : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "renderer " << renderer << " browser " << browser);
      media::mojom::AudioProcessingConfigPtr config = RendererConfig(renderer);
      content::MoltApplyCpuIndependentProcessing(browser, config.get());
      EXPECT_EQ(config->settings.cpu_independent, browser);
      media::AudioProcessingSettings expected =
          RendererConfig(renderer)->settings;
      expected.cpu_independent = browser;
      EXPECT_TRUE(config->settings == expected);
    }
  }
}

// A stream the audio service doesn't process has no config to set.
TEST(MoltNetAudioCaptureTest, NoConfigStaysNone) {
  media::mojom::AudioProcessingConfigPtr config;
  content::MoltApplyCpuIndependentProcessing(true, config.get());
  EXPECT_FALSE(config);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
