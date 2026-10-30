// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's own voice, end to end but for the voice: the real
// BundledVoice starts a stand-in helper (voice_speech_fake_helper.cc,
// molt_fake_voice_helper) under the real resident sandbox, checks its
// frames and plays its audio through the audio service (with
// --disable-audio-output, which renders it in real time to a fake output).
// Checked: an answer is spoken by the bundled voice and finishes when its
// audio has played, with the platform's voices untouched; voice mode's
// speech is then in the echo canceller's reference; "stop" silences it at
// once; a helper that crashes or breaks the protocol is restarted, what it
// hadn't said is said by the platform's voice, and after two failures the
// platform's voices speak; without a bundled voice, the platform's voices
// speak.
//
// Linux and macOS: on Windows the helper's AppContainer reads only the
// folders MoltBrowser's installer grants, which a test binary's isn't.

#include "chrome/browser/molt_ai/voice/voice_speech.h"

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/path_service.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/voice/voice_speech_bundled.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/test/browser_test.h"
#include "media/base/media_switches.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

// A TTS platform that says each utterance at once and records it.
class RecordingTtsPlatform : public content::TtsPlatform {
 public:
  RecordingTtsPlatform() = default;
  RecordingTtsPlatform(const RecordingTtsPlatform&) = delete;
  RecordingTtsPlatform& operator=(const RecordingTtsPlatform&) = delete;
  ~RecordingTtsPlatform() = default;

  const std::vector<std::string>& spoken() const { return spoken_; }

  // content::TtsPlatform:
  bool PlatformImplSupported() override { return true; }
  bool PlatformImplInitialized() override { return true; }
  void LoadBuiltInTtsEngine(content::BrowserContext* context) override {}
  void Speak(int utterance_id,
             const std::string& utterance,
             const std::string& lang,
             const content::VoiceData& voice,
             const content::UtteranceContinuousParameters& params,
             base::OnceCallback<void(bool)> did_start_speaking) override {
    spoken_.push_back(utterance);
    std::move(did_start_speaking).Run(true);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](int id, int length) {
                         content::TtsController* tts =
                             content::TtsController::GetInstance();
                         tts->OnTtsEvent(id, content::TTS_EVENT_START, 0, -1,
                                         std::string());
                         tts->OnTtsEvent(id, content::TTS_EVENT_END, length, -1,
                                         std::string());
                       },
                       utterance_id, static_cast<int>(utterance.size())));
  }
  bool StopSpeaking() override { return true; }
  bool IsSpeaking() override { return false; }
  void GetVoices(std::vector<content::VoiceData>* out_voices) override {
    content::VoiceData voice;
    voice.name = "Fake voice";
    voice.lang = "en-US";
    voice.native = true;
    voice.events = {content::TTS_EVENT_START, content::TTS_EVENT_END};
    out_voices->push_back(voice);
  }
  void Pause() override {}
  void Resume() override {}
  void WillSpeakUtteranceWithVoice(content::TtsUtterance* utterance,
                                   const content::VoiceData& voice) override {}
  std::string GetError() override { return std::string(); }
  void ClearError() override {}
  void SetError(const std::string& error) override {}
  void Shutdown() override {}
  void FinalizeVoiceOrdering(std::vector<content::VoiceData>& voices) override {
  }
  void RefreshVoices() override {}

 private:
  std::vector<std::string> spoken_;
};

class VoiceSpeechBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // The audio service renders output to a fake stream, in real time.
    command_line->AppendSwitch(switches::kDisableAudioOutput);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(models_.CreateUniqueTempDir());
    base::FilePath exe_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
    BundledVoice::SetPathsForTesting(BundledVoice::Paths{
        exe_dir.AppendASCII("molt_fake_voice_helper"), models_.GetPath()});
  }

  void TearDownOnMainThread() override {
    speech_.reset();
    BundledVoice::SetPathsForTesting(std::nullopt);
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  // A VoiceSpeech for the window's profile, active, its voice ready.
  VoiceSpeech* ReadySpeech() {
    speech_ = std::make_unique<VoiceSpeech>(browser()->profile());
    speech_->SetActive(true);
    EXPECT_TRUE(base::test::RunUntil([&]() {
      return speech_->bundled_for_testing() &&
             speech_->bundled_for_testing()->ready();
    }));
    return speech_.get();
  }

  RecordingTtsPlatform tts_;
  base::ScopedTempDir models_;
  std::unique_ptr<VoiceSpeech> speech_;
};

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

IN_PROC_BROWSER_TEST_F(VoiceSpeechBrowserTest, SpeaksWithItsOwnVoice) {
  VoiceSpeech* speech = ReadySpeech();
  EXPECT_TRUE(speech->InEchoReference());
  EXPECT_FALSE(speech->bundled_for_testing()->confinement().empty());
  const base::TimeTicks start = base::TimeTicks::Now();
  // 16 bytes: 160 ms of tone.
  speech->Speak("YouTube is open.", 1.0, /*interrupt=*/true);
  EXPECT_TRUE(speech->speaking());
  ASSERT_TRUE(base::test::RunUntil([&]() { return !speech->speaking(); }));
  // It lasted as long as its audio played.
  EXPECT_GE(base::TimeTicks::Now() - start, base::Milliseconds(150));
  EXPECT_TRUE(tts_.spoken().empty());
  // Several answers, one after the other.
  speech->Speak("One.", 1.0, false);
  speech->Speak("Two.", 1.0, false);
  speech->Speak("Three.", 1.0, false);
  speech->Speak("...", 1.0, false);  // nothing to say
  ASSERT_TRUE(base::test::RunUntil([&]() { return !speech->speaking(); }));
  EXPECT_TRUE(tts_.spoken().empty());
}

IN_PROC_BROWSER_TEST_F(VoiceSpeechBrowserTest, StopSilencesAtOnce) {
  VoiceSpeech* speech = ReadySpeech();
  // 3 s of tone.
  speech->Speak(std::string(400, 'a'), 1.0, true);
  speech->Speak("And then this.", 1.0, false);
  ASSERT_TRUE(speech->speaking());
  speech->Stop();
  EXPECT_FALSE(speech->speaking());
  // Nothing comes back afterwards; the voice works on.
  speech->Speak("Next.", 1.0, false);
  ASSERT_TRUE(base::test::RunUntil([&]() { return !speech->speaking(); }));
  EXPECT_TRUE(tts_.spoken().empty());
  EXPECT_TRUE(speech->bundled_for_testing()->ready());
}

IN_PROC_BROWSER_TEST_F(VoiceSpeechBrowserTest,
                       ACrashFallsBackThenTheVoiceComesBack) {
  VoiceSpeech* speech = ReadySpeech();
  speech->Speak("This will crash.", 1.0, true);
  // What it hadn't said is said by the platform's voice.
  ASSERT_TRUE(base::test::RunUntil([&]() { return !tts_.spoken().empty(); }));
  EXPECT_EQ("This will crash.", tts_.spoken()[0]);
  EXPECT_FALSE(speech->InEchoReference());
  // Restarted after a moment, it speaks again.
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return speech->bundled_for_testing()->ready(); }));
  speech->Speak("Back again.", 1.0, true);
  ASSERT_TRUE(base::test::RunUntil([&]() { return !speech->speaking(); }));
  EXPECT_EQ(1u, tts_.spoken().size());
}

IN_PROC_BROWSER_TEST_F(VoiceSpeechBrowserTest,
                       TwoFailuresAndThePlatformVoicesSpeak) {
  VoiceSpeech* speech = ReadySpeech();
  // A frame for a request that isn't being said ends the helper too.
  speech->Speak("Some garbage.", 1.0, true);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return tts_.spoken().size() == 1; }));
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return speech->bundled_for_testing()->ready(); }));
  speech->Speak("This will crash.", 1.0, true);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return tts_.spoken().size() == 2; }));
  // Given up: no restart; the platform's voices speak from now on.
  EXPECT_FALSE(speech->bundled_for_testing()->started());
  speech->Speak("Platform now.", 1.0, true);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return tts_.spoken().size() == 3; }));
  EXPECT_EQ("Platform now.", tts_.spoken()[2]);
  EXPECT_FALSE(speech->InEchoReference());
}

IN_PROC_BROWSER_TEST_F(VoiceSpeechBrowserTest, WithoutTheVoiceThePlatforms) {
  BundledVoice::SetPathsForTesting(std::nullopt);
  // DIR_EXE/voice/ doesn't exist next to the test binary.
  speech_ = std::make_unique<VoiceSpeech>(browser()->profile());
  speech_->SetActive(true);
  speech_->Speak("Voice mode is on.", 1.0, true);
  ASSERT_TRUE(base::test::RunUntil([&]() { return !tts_.spoken().empty(); }));
  EXPECT_EQ("Voice mode is on.", tts_.spoken()[0]);
  EXPECT_EQ(nullptr, speech_->bundled_for_testing());
  EXPECT_FALSE(speech_->InEchoReference());
}

#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

}  // namespace
}  // namespace voice
}  // namespace molt_ai
