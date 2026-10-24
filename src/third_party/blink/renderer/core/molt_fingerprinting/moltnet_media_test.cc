// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"

#include <memory>
#include <optional>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_audio_latency_hint.h"
#include "third_party/blink/public/platform/web_content_decryption_module_access.h"
#include "third_party/blink/public/platform/web_encrypted_media_client.h"
#include "third_party/blink/public/platform/web_encrypted_media_request.h"
#include "third_party/blink/public/platform/web_media_key_system_configuration.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/platform/encrypted_media_request.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

namespace blink::molt_fingerprinting {
namespace {

// A requestMediaKeySystemAccess() call for Clear Key, recording the answer.
class FakeRequest final : public EncryptedMediaRequest {
 public:
  WebString KeySystem() const override {
    return WebString::FromUtf8("org.w3.clearkey");
  }
  const std::vector<WebMediaKeySystemConfiguration>& SupportedConfigurations()
      const override {
    return configurations_;
  }
  const SecurityOrigin* GetSecurityOrigin() const override { return nullptr; }
  void RequestSucceeded(
      std::unique_ptr<WebContentDecryptionModuleAccess>) override {
    ++successes;
  }
  void RequestNotSupported(const WebString& error_message) override {
    ++refusals;
    message = error_message.Utf8();
  }

  int successes = 0;
  int refusals = 0;
  std::string message;

 private:
  std::vector<WebMediaKeySystemConfiguration> configurations_ =
      std::vector<WebMediaKeySystemConfiguration>(1);
};

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet is simulated with
// SetMoltNetMediaForTesting().
class MoltNetMediaTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetMediaForTesting(false);
  }
  void TearDown() override {
    SetMoltNetMediaForTesting(false);
    ResetForTesting();
  }

  test::TaskEnvironment task_environment_;
};

TEST_F(MoltNetMediaTest, NormalWindowsUseTheDevice) {
  for (const Mode mode : {Mode::kOff, Mode::kFarble}) {
    ResetForTesting();
    Configure(mode);
    EXPECT_EQ(UniformAudioContextSampleRate(std::nullopt), std::nullopt);
    EXPECT_EQ(UniformAudioContextSampleRate(44100.0f), 44100.0f);
    EXPECT_EQ(UniformRenderQuantumFrames(std::nullopt), std::nullopt);
    EXPECT_EQ(UniformAudioCallbackFrames(
                  WebAudioLatencyHint(WebAudioLatencyHint::kCategoryPlayback),
                  48000),
              std::nullopt);
    EXPECT_EQ(UniformAudioMaxChannelCount(6), 6u);
    EXPECT_EQ(UniformAudioOutputLatency(), std::nullopt);
    EXPECT_EQ(UniformAudioPlayoutDelay(), std::nullopt);
    EXPECT_FALSE(UniformAudioOutputPosition(1.0).has_value());
    EXPECT_EQ(MoltNetEncryptedMediaClient(), nullptr);
    EXPECT_FALSE(UniformVideoDecodingInfo(true).has_value());
    EXPECT_FALSE(UniformWebrtcCodingInfo().has_value());
    EXPECT_TRUE(UsesPlatformMediaCodecs());
  }
}

TEST_F(MoltNetMediaTest, MoltNetRunsOneStereoDevice) {
  SetMoltNetMediaForTesting(true);
  EXPECT_EQ(UniformAudioContextSampleRate(std::nullopt), 48000.0f);
  // A rate the page asks for is the page's own choice.
  EXPECT_EQ(UniformAudioContextSampleRate(22050.0f), 22050.0f);
  EXPECT_EQ(UniformRenderQuantumFrames(std::nullopt), 128u);
  EXPECT_EQ(UniformRenderQuantumFrames(256u), 256u);
  EXPECT_EQ(UniformAudioMaxChannelCount(8), 2u);
  EXPECT_EQ(UniformAudioMaxChannelCount(1), 2u);
}

TEST_F(MoltNetMediaTest, MoltNetCallbackBuffersForEveryLatencyHint) {
  SetMoltNetMediaForTesting(true);
  EXPECT_EQ(UniformAudioCallbackFrames(
                WebAudioLatencyHint(WebAudioLatencyHint::kCategoryInteractive),
                48000),
            480u);
  EXPECT_EQ(UniformAudioCallbackFrames(
                WebAudioLatencyHint(WebAudioLatencyHint::kCategoryBalanced),
                48000),
            480u);
  EXPECT_EQ(UniformAudioCallbackFrames(
                WebAudioLatencyHint(WebAudioLatencyHint::kCategoryPlayback),
                48000),
            1024u);
  EXPECT_EQ(UniformAudioCallbackFrames(WebAudioLatencyHint(0.0), 48000), 128u);
  EXPECT_EQ(UniformAudioCallbackFrames(WebAudioLatencyHint(0.02), 48000),
            1024u);
  EXPECT_EQ(UniformAudioCallbackFrames(
                WebAudioLatencyHint(WebAudioLatencyHint::kCategoryInteractive),
                44100),
            441u);
  // No context has an invalid rate; the real value is kept then.
  EXPECT_EQ(UniformAudioCallbackFrames(
                WebAudioLatencyHint(WebAudioLatencyHint::kCategoryInteractive),
                0),
            std::nullopt);
}

TEST_F(MoltNetMediaTest, MoltNetOutputLatencyAndPosition) {
  SetMoltNetMediaForTesting(true);
  EXPECT_EQ(UniformAudioOutputLatency(), 0.024);
  EXPECT_EQ(UniformAudioPlayoutDelay(), base::Milliseconds(24));
  const auto before = UniformAudioOutputPosition(0);
  ASSERT_TRUE(before.has_value());
  EXPECT_FALSE(before->playing);
  EXPECT_EQ(before->context_time, 0);
  const auto later = UniformAudioOutputPosition(3);
  ASSERT_TRUE(later.has_value());
  EXPECT_TRUE(later->playing);
  EXPECT_DOUBLE_EQ(later->context_time, 2.976);
}

TEST_F(MoltNetMediaTest, MoltNetRefusesEveryKeySystem) {
  SetMoltNetMediaForTesting(true);
  WebEncryptedMediaClient* client = MoltNetEncryptedMediaClient();
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(client, MoltNetEncryptedMediaClient());
  auto* request = MakeGarbageCollected<FakeRequest>();
  client->RequestMediaKeySystemAccess(WebEncryptedMediaRequest(request));
  EXPECT_EQ(request->successes, 0);
  EXPECT_EQ(request->refusals, 1);
  EXPECT_EQ(request->message,
            "Unsupported keySystem or supportedConfigurations.");
}

TEST_F(MoltNetMediaTest, MoltNetMediaCapabilitiesAndCodecs) {
  SetMoltNetMediaForTesting(true);
  const auto built_in = UniformVideoDecodingInfo(true);
  ASSERT_TRUE(built_in.has_value());
  EXPECT_TRUE(built_in->supported);
  EXPECT_TRUE(built_in->smooth);
  EXPECT_FALSE(built_in->power_efficient);
  const auto platform_only = UniformVideoDecodingInfo(false);
  ASSERT_TRUE(platform_only.has_value());
  EXPECT_FALSE(platform_only->supported);
  const auto webrtc = UniformWebrtcCodingInfo();
  ASSERT_TRUE(webrtc.has_value());
  EXPECT_FALSE(webrtc->supported);
  EXPECT_FALSE(UsesPlatformMediaCodecs());
}

}  // namespace
}  // namespace blink::molt_fingerprinting
