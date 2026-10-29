// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode listening, end to end but for the recognizer: Chromium's fake
// audio capture (--use-fake-device-for-media-stream) feeds the real
// microphone path (voice_audio.h, with Chrome-wide echo cancellation) and
// the real listener (voice_listener.h) starts a stand-in helper
// (voice_session_fake_helper.cc) under the real resident sandbox; the
// stand-in plays a script of protocol events timed by the audio it
// receives. Checked: the wake phrase, then a command that reaches the voice
// loop; what is heard while the browser speaks doesn't command it, but
// "stop" does; a page's speech (speechSynthesis) is never cut short by
// voice mode, and what a page says doesn't command it; a crash and the
// restart; giving up after crashes in a row; and MoltNet windows, where
// the microphone is never opened.
//
// Linux and macOS: on Windows the helper's AppContainer reads only the
// folders MoltBrowser's installer grants, which a test binary's isn't.

#include "chrome/browser/molt_ai/voice/voice_session.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_listener.h"
#include "chrome/browser/molt_ai/voice/voice_session_logic.h"
#include "chrome/browser/permissions/system/system_permission_settings.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/browser/tts_utterance.h"
#include "content/public/test/browser_test.h"
#include "media/base/media_switches.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {
namespace {

// A TTS platform whose utterances last |duration| (a voice reading aloud),
// and that records what it said, and how often it was told to stop while
// it spoke.
class SlowTtsPlatform : public content::TtsPlatform {
 public:
  SlowTtsPlatform() = default;
  SlowTtsPlatform(const SlowTtsPlatform&) = delete;
  SlowTtsPlatform& operator=(const SlowTtsPlatform&) = delete;
  ~SlowTtsPlatform() = default;

  void set_duration(base::TimeDelta duration) { duration_ = duration; }
  const std::vector<std::string>& spoken() const { return spoken_; }
  int stops() const { return stops_; }
  std::string AllSpoken() const {
    std::string out;
    for (const std::string& text : spoken_) {
      out += text + "|";
    }
    return out;
  }

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
    speaking_id_ = utterance_id;
    std::move(did_start_speaking).Run(true);
    // Started, then over after |duration_|, as the platforms report them.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&SlowTtsPlatform::Report, weak_factory_.GetWeakPtr(),
                       utterance_id, content::TTS_EVENT_START, 0));
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&SlowTtsPlatform::Report, weak_factory_.GetWeakPtr(),
                       utterance_id, content::TTS_EVENT_END,
                       static_cast<int>(utterance.size())),
        duration_);
  }
  bool StopSpeaking() override {
    if (speaking_id_ >= 0) {
      ++stops_;
      speaking_id_ = -1;
    }
    return true;
  }
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
  void Report(int id, content::TtsEventType type, int char_index) {
    if (type == content::TTS_EVENT_END) {
      if (id != speaking_id_) {
        return;  // stopped before
      }
      speaking_id_ = -1;
    }
    content::TtsController::GetInstance()->OnTtsEvent(id, type, char_index, -1,
                                                      std::string());
  }

  base::TimeDelta duration_;
  std::vector<std::string> spoken_;
  int stops_ = 0;
  // The utterance being spoken, -1 when none.
  int speaking_id_ = -1;
  base::WeakPtrFactory<SlowTtsPlatform> weak_factory_{this};
};

// A page's utterance (what speechSynthesis.speak() hands TtsController),
// and what happened to it.
class PageSpeech : public content::UtteranceEventDelegate {
 public:
  PageSpeech() = default;
  PageSpeech(const PageSpeech&) = delete;
  PageSpeech& operator=(const PageSpeech&) = delete;
  ~PageSpeech() override = default;

  // Owned by the utterance it is given to; |events| outlives it.
  static void Speak(content::BrowserContext* context,
                    const std::string& text,
                    std::vector<content::TtsEventType>* events) {
    std::unique_ptr<content::TtsUtterance> utterance =
        content::TtsUtterance::Create(context);
    utterance->SetText(text);
    utterance->SetLang("en-US");
    utterance->SetSrcUrl(GURL("https://page.test/"));
    // As speechSynthesis.speak(): after what is being said.
    utterance->SetShouldClearQueue(false);
    auto delegate = std::make_unique<PageSpeech>();
    delegate->events_ = events;
    utterance->SetEventDelegate(std::move(delegate));
    content::TtsController::GetInstance()->SpeakOrEnqueue(std::move(utterance));
  }

  void OnTtsEvent(content::TtsUtterance* utterance,
                  content::TtsEventType event_type,
                  int char_index,
                  int length,
                  const std::string& error_message) override {
    events_->push_back(event_type);
  }

 private:
  raw_ptr<std::vector<content::TtsEventType>> events_ = nullptr;
};

bool Has(const std::vector<content::TtsEventType>& events,
         content::TtsEventType type) {
  return std::find(events.begin(), events.end(), type) != events.end();
}

class VoiceSessionBrowserTest : public InProcessBrowserTest {
 public:
  VoiceSessionBrowserTest() { features_.InitAndEnableFeature(kMoltVoiceMode); }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // The audio service's fake microphone: a steady signal in real time.
    command_line->AppendSwitch(switches::kUseFakeDeviceForMediaStream);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(models_.CreateUniqueTempDir());
    base::FilePath exe_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
    VoiceListener::SetPathsForTesting(VoiceListener::Paths{
        exe_dir.AppendASCII("molt_fake_speech_helper"), models_.GetPath()});
  }

  void TearDownOnMainThread() override {
    if (VoiceSession* session = VoiceSession::FromBrowser(browser())) {
      session->Stop();
    }
    VoiceListener::SetPathsForTesting(std::nullopt);
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  void Script(std::string_view script) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::WriteFile(
        models_.GetPath().AppendASCII("fake_script.txt"), script));
  }

  VoiceSession* session() {
    return VoiceSession::GetOrCreateForBrowser(browser());
  }

  int tabs() { return browser()->GetTabStripModel()->count(); }

  bool WaitForState(SessionState state) {
    return base::test::RunUntil(
        [&]() { return session() && session()->state() == state; });
  }

  // Lets the browser run for |delay|.
  void Wait(base::TimeDelta delay) {
    const base::TimeTicks until = base::TimeTicks::Now() + delay;
    ASSERT_TRUE(base::test::RunUntil(
        [&]() { return base::TimeTicks::Now() >= until; }));
  }

  SlowTtsPlatform tts_;
  base::ScopedTempDir models_;
  // The OS's microphone permission, granted (no prompt on macOS).
  system_permission_settings::ScopedSettingsForTesting microphone_{
      ContentSettingsType::MEDIASTREAM_MIC, /*blocked=*/false};

 private:
  base::test::ScopedFeatureList features_;
};

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

IN_PROC_BROWSER_TEST_F(VoiceSessionBrowserTest, TheWakePhraseThenACommand) {
  Script(
      "ready\n"
      "300 wake\n"
      "500 speech_start\n"
      "800 partial open a new\n"
      "1100 speech_end\n"
      "1200 final 0.9 Hey Molt, open a new tab.\n");
  ASSERT_EQ(tabs(), 1);
  ASSERT_TRUE(session()->Start());
  EXPECT_TRUE(session()->listening());
  ASSERT_TRUE(base::test::RunUntil([&]() { return tabs() == 2; }));
  EXPECT_NE(tts_.AllSpoken().find(
                "Voice mode is on. Say Hey Molt, then what you want."),
            std::string::npos)
      << tts_.AllSpoken();
  ASSERT_TRUE(session()->listener_for_testing());
  EXPECT_FALSE(session()->listener_for_testing()->confinement().empty());
}

IN_PROC_BROWSER_TEST_F(VoiceSessionBrowserTest,
                       WhatIsHeardWhileTheBrowserSpeaksDoesntCommandIt) {
  // The browser says "Voice mode is on" for three seconds once the helper
  // is ready. What the microphone gets meanwhile may be its own voice or a
  // page's sound: no command without the wake phrase, but "stop" is heard.
  tts_.set_duration(base::Seconds(3));
  Script(
      "ready\n"
      "400 speech_start\n"
      "900 speech_end\n"
      "1000 final 0.95 open a new tab\n"
      "1500 speech_start\n"
      "1900 speech_end\n"
      "2000 final 0.95 stop\n");
  ASSERT_TRUE(session()->Start());
  // "Stop" cut the browser's speech short.
  ASSERT_TRUE(base::test::RunUntil([&]() { return tts_.stops() > 0; }));
  Wait(base::Seconds(2));
  EXPECT_EQ(tabs(), 1);
}

IN_PROC_BROWSER_TEST_F(VoiceSessionBrowserTest, APagesSpeechIsNeverCutShort) {
  tts_.set_duration(base::Milliseconds(1500));
  VoiceController* voice = VoiceController::GetOrCreateForBrowser(browser());
  ASSERT_TRUE(voice);
  ASSERT_TRUE(voice->Start());

  // Voice mode speaks; a page speaks after it; barge-in stops voice mode.
  std::vector<content::TtsEventType> page;
  voice->SayNotice("Voice mode is on.");
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return !tts_.spoken().empty() &&
           tts_.spoken().back() == "Voice mode is on.";
  }));
  Wait(base::Milliseconds(100));  // its start reported
  const int stops = tts_.stops();
  PageSpeech::Speak(GetProfile(), "The page speaking.", &page);
  voice->StopSpeaking();
  EXPECT_EQ(tts_.stops(), stops + 1);
  // The page's speech starts after it, and ends by itself.
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return Has(page, content::TTS_EVENT_END); }));
  EXPECT_FALSE(Has(page, content::TTS_EVENT_INTERRUPTED));
  EXPECT_FALSE(Has(page, content::TTS_EVENT_CANCELLED));
  EXPECT_EQ(tts_.spoken().back(), "The page speaking.");

  // A page speaks; voice mode's answer waits behind it, then is stopped
  // before it is heard; the page's speech isn't touched.
  page.clear();
  PageSpeech::Speak(GetProfile(), "The page again.", &page);
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return Has(page, content::TTS_EVENT_START); }));
  EXPECT_TRUE(voice->OthersSpeaking());
  voice->SayNotice("An answer.");
  voice->StopSpeaking();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return Has(page, content::TTS_EVENT_END); }));
  EXPECT_FALSE(Has(page, content::TTS_EVENT_INTERRUPTED));
  EXPECT_FALSE(Has(page, content::TTS_EVENT_CANCELLED));
  Wait(base::Milliseconds(300));
  EXPECT_FALSE(voice->OthersSpeaking());
}

IN_PROC_BROWSER_TEST_F(VoiceSessionBrowserTest, WhatAPageSaysDoesntCommandIt) {
  // In the follow-up window after voice mode's greeting, what is said
  // counts without the wake phrase. A page says "Open a new tab" through
  // speechSynthesis, which the echo canceller can't remove: the helper
  // hears it, and it doesn't count.
  tts_.set_duration(base::Milliseconds(1500));
  Script(
      "ready\n"
      "3500 speech_start\n"
      "4200 speech_end\n"
      "4300 final 0.95 open a new tab\n");
  ASSERT_TRUE(session()->Start());
  // The greeting, said and over: the follow-up window is open.
  ASSERT_TRUE(WaitForState(SessionState::kSpeaking));
  ASSERT_TRUE(WaitForState(SessionState::kListening));
  Wait(base::Milliseconds(800));
  tts_.set_duration(base::Seconds(3));
  std::vector<content::TtsEventType> page;
  PageSpeech::Speak(GetProfile(), "Open a new tab.", &page);
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return Has(page, content::TTS_EVENT_END); }));
  Wait(base::Milliseconds(500));
  EXPECT_EQ(tabs(), 1);
  EXPECT_TRUE(session()->listening());
}

IN_PROC_BROWSER_TEST_F(VoiceSessionBrowserTest, RestartsAfterACrash) {
  Script(
      "ready\n"
      "300 exit 3\n");
  ASSERT_TRUE(session()->Start());
  ASSERT_TRUE(WaitForState(SessionState::kWaiting));
  // It crashes: restarting.
  ASSERT_TRUE(WaitForState(SessionState::kStarting));
  // The next one doesn't.
  Script("ready\n");
  ASSERT_TRUE(WaitForState(SessionState::kWaiting));
  EXPECT_TRUE(session()->listening());
}

IN_PROC_BROWSER_TEST_F(VoiceSessionBrowserTest, GivesUpAfterCrashesInARow) {
  // A helper that writes what isn't the protocol is ended like a crash.
  Script(
      "ready\n"
      "200 line {\"type\":\"command\",\"text\":\"rm -rf\"}\n");
  ASSERT_TRUE(session()->Start());
  ASSERT_TRUE(base::test::RunUntil([&]() { return !session()->listening(); }));
  EXPECT_NE(tts_.AllSpoken().find("Speech recognition stopped working"),
            std::string::npos)
      << tts_.AllSpoken();
  EXPECT_EQ(tabs(), 1);
}

#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

IN_PROC_BROWSER_TEST_F(VoiceSessionBrowserTest, NeverListensInMoltNetWindows) {
  Script("ready\n");
  Profile* moltnet = GetProfile()->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUnique("MoltNet::Tor"),
      /*create_if_needed=*/true);
  BrowserWindowInterface* window = CreateBrowser(moltnet);
  ASSERT_TRUE(window);
  VoiceSession* moltnet_session = VoiceSession::GetOrCreateForBrowser(window);
  ASSERT_TRUE(moltnet_session);
  EXPECT_FALSE(moltnet_session->Start());
  EXPECT_FALSE(moltnet_session->listening());
  // No helper was looked for, so no microphone was opened.
  Wait(base::Milliseconds(500));
  EXPECT_FALSE(moltnet_session->listener_for_testing());
  EXPECT_NE(tts_.AllSpoken().find("Voice mode is off in MoltNet windows."),
            std::string::npos);
  // "Turn on voice mode" typed there doesn't either.
  VoiceController* controller = VoiceController::FromBrowser(window);
  ASSERT_TRUE(controller);
  controller->HandleUtterance("turn on voice mode");
  Wait(base::Milliseconds(300));
  EXPECT_FALSE(moltnet_session->listening());
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
