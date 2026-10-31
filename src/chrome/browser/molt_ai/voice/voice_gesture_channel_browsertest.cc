// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice gesture word's channel (voice_gesture_channel.h,
// docs/design/TOUCHLESS_BROWSING.md §7.3) with real captures, on
// chrome/test/data/molt_voice/capture.html: a page that captures the
// microphone (getUserMedia, a fake device) and one that shares the screen
// (getDisplayMedia, the picker choosing the screen by itself), while it asks
// for a permission answered by voice. A fake TTS platform records every
// utterance; the listening session's part is played by the test
// (VoiceController::HandleHeardUtterance()).
//   - A microphone captured: the word is shown, never spoken; heard but not
//     cleanly, it changes, and the old one neither allows nor refuses.
//   - The screen shared: no word at all (neither spoken nor shown), the
//     prompt says why, and "no" still blocks; a word shown when the share
//     begins is taken back, and the prompt gets one again when it ends.

#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "build/build_config.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/molt_ai/voice/voice_browser_prompts.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "chrome/browser/molt_ai/voice/voice_gesture_channel.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/permissions/permission_request_manager.h"
#include "components/permissions/test/mock_permission_prompt_factory.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/media_switches.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {
namespace {

// Finishes each utterance at once, and keeps what it was asked to say.
class RecordingTtsPlatform : public content::TtsPlatform {
 public:
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
                         tts->OnTtsEvent(id, content::TTS_EVENT_END, length, 0,
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

  // Whether the platform's voice was ever asked to say |text|.
  bool Spoke(std::string_view text) const {
    for (const std::string& said : spoken_) {
      if (said.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<std::string> spoken_;
};

class VoiceGestureChannelBrowserTest : public InProcessBrowserTest {
 public:
  VoiceGestureChannelBrowserTest() {
    features_.InitAndEnableFeature(kMoltVoiceMode);
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // A fake microphone and camera; the screen picker chooses the screen
    // by itself (as Chromium's own getDisplayMedia browser tests do).
    command_line->AppendSwitch(switches::kUseFakeDeviceForMediaStream);
    command_line->AppendSwitchASCII(switches::kAutoSelectDesktopCaptureSource,
#if BUILDFLAG(IS_CHROMEOS)
                                    "Display");
#else
                                    "Entire screen");
#endif
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(embedded_test_server()->Start());
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
  }

  void TearDownOnMainThread() override {
    if (VoiceController* voice = VoiceController::FromBrowser(browser())) {
      voice->Stop();
    }
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

  content::WebContents* tab() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }

  // The page, then voice mode on it, listening with an echo canceller.
  VoiceController* Start() {
    const GURL page =
        embedded_test_server()->GetURL("localhost", "/molt_voice/capture.html");
    // The page may use the microphone without being asked.
    HostContentSettingsMapFactory::GetForProfile(browser()->GetProfile())
        ->SetContentSettingDefaultScope(page, page,
                                        ContentSettingsType::MEDIASTREAM_MIC,
                                        CONTENT_SETTING_ALLOW);
    EXPECT_TRUE(ui_test_utils::NavigateToURL(browser(), page));
    VoiceController* voice = VoiceController::GetOrCreateForBrowser(browser());
    EXPECT_TRUE(voice);
    EXPECT_TRUE(voice->Start());
    VoiceController::ListeningCaption caption;
    caption.listening = true;
    voice->SetListening(caption);
    voice->SetWordChannelSettings(/*echo_cancelled=*/true,
                                  /*say_aloud_anyway=*/false);
    return voice;
  }

  bool WaitForLog(std::string_view entry) {
    return base::test::RunUntil([&]() {
      return content::EvalJs(tab(), "moltLog.join(' ')",
                             content::EXECUTE_SCRIPT_NO_USER_GESTURE)
                 .ExtractString()
                 .find(entry) != std::string::npos;
    });
  }

  bool WaitForSpeech(std::string_view text) {
    return base::test::RunUntil([&]() { return tts_.Spoke(text); });
  }

  VoiceBrowserPrompts* Prompts(VoiceController* voice) {
    return voice->browser_prompts_for_testing();
  }

  // Asks for notifications, and waits until voice mode has read it.
  void AskNotifications() {
    EXPECT_TRUE(content::ExecJs(tab(), "askNotifications()",
                                content::EXECUTE_SCRIPT_NO_USER_GESTURE));
    EXPECT_TRUE(WaitForSpeech("localhost wants to show notifications."));
  }

  // No word of the list was ever said by the platform's voice.
  bool NoWordSpoken() {
    for (std::string_view word : ConfirmationWords()) {
      if (tts_.Spoke(" say " + std::string(word) + ".")) {
        return false;
      }
    }
    return true;
  }

  RecordingTtsPlatform tts_;

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(VoiceGestureChannelBrowserTest,
                       AMicrophoneCapturedSeesTheWordShownNeverSpoken) {
  VoiceController* voice = Start();
  auto* manager = permissions::PermissionRequestManager::FromWebContents(tab());
  ASSERT_TRUE(manager);
  permissions::MockPermissionPromptFactory factory(manager);

  ASSERT_TRUE(content::ExecJs(tab(), "captureAudio()"));
  ASSERT_TRUE(WaitForLog("audio:on"));
  ASSERT_TRUE(Prompts(voice));
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return Prompts(voice)->word_channel_inputs_for_testing().audio_captured;
  }));

  AskNotifications();
  const std::string word = Prompts(voice)->current_word_for_testing();
  ASSERT_TRUE(MatchConfirmationWord(word)) << word;
  // Shown, not spoken: the browser says where it is.
  EXPECT_EQ(Prompts(voice)->ShownWord(), word);
  EXPECT_TRUE(WaitForSpeech("To allow it, say the word on your screen."));
  EXPECT_FALSE(tts_.Spoke(word));
  // "yes" says where it is again, not the word.
  voice->HandleHeardUtterance("yes", /*heard_cleanly=*/true);
  EXPECT_TRUE(base::test::RunUntil(
      [&]() { return Prompts(voice)->current_word_for_testing() == word; }));
  EXPECT_FALSE(tts_.Spoke(word));

  // Heard, but not cleanly, while the page listens: the word changes.
  voice->HandleHeardUtterance(word, /*heard_cleanly=*/false);
  EXPECT_TRUE(WaitForSpeech("so I changed it."));
  const std::string next = Prompts(voice)->current_word_for_testing();
  ASSERT_TRUE(MatchConfirmationWord(next)) << next;
  EXPECT_NE(next, word);
  EXPECT_EQ(Prompts(voice)->ShownWord(), next);
  // The old one, even heard cleanly (a replay), neither allows nor refuses.
  voice->HandleHeardUtterance(word, /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForSpeech("That word changed."));
  EXPECT_TRUE(manager->IsRequestInProgress());
  EXPECT_FALSE(content::EvalJs(tab(), "moltLog.includes('notify:granted')")
                   .ExtractBool());
  // The new one, heard cleanly, allows it.
  voice->HandleHeardUtterance(next, /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForLog("notify:granted"));
  EXPECT_TRUE(NoWordSpoken());
}

IN_PROC_BROWSER_TEST_F(VoiceGestureChannelBrowserTest,
                       NoWordWhileTheScreenIsSharedAndNoStillBlocks) {
#if BUILDFLAG(IS_WIN)
  // As Chromium's own screen capture tests: Windows bots have no capturer.
  GTEST_SKIP() << "No screen capture on Windows bots";
#else
  VoiceController* voice = Start();
  auto* manager = permissions::PermissionRequestManager::FromWebContents(tab());
  ASSERT_TRUE(manager);
  permissions::MockPermissionPromptFactory factory(manager);

  // getDisplayMedia() needs the user's activation: the test's script has it.
  ASSERT_TRUE(content::ExecJs(tab(), "shareScreen()"));
  ASSERT_TRUE(WaitForLog("screen:"));
  if (!content::EvalJs(tab(), "moltLog.includes('screen:on')").ExtractBool()) {
    GTEST_SKIP() << "No screen to capture here";
  }
  ASSERT_TRUE(Prompts(voice));
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return Prompts(voice)->word_channel_inputs_for_testing().screen_captured;
  }));

  AskNotifications();
  EXPECT_TRUE(WaitForSpeech(
      "I can't take the word while your screen is shared, so to allow it, "
      "use the mouse or keyboard. To block it, say no."));
  EXPECT_EQ(Prompts(voice)->current_word_for_testing(), "");
  EXPECT_EQ(Prompts(voice)->ShownWord(), "");
  EXPECT_TRUE(NoWordSpoken());
  // Any word of the list allows nothing.
  voice->HandleHeardUtterance(std::string(ConfirmationWords()[0]),
                              /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForSpeech(
      "I can't take the word while your screen is shared. Use the mouse or "
      "keyboard, or say no."));
  EXPECT_TRUE(manager->IsRequestInProgress());
  // "no" still blocks.
  voice->HandleHeardUtterance("no", /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForLog("notify:denied"));
#endif
}

IN_PROC_BROWSER_TEST_F(VoiceGestureChannelBrowserTest,
                       AShareThatBeginsTakesTheShownWordBack) {
#if BUILDFLAG(IS_WIN)
  GTEST_SKIP() << "No screen capture on Windows bots";
#else
  VoiceController* voice = Start();
  auto* manager = permissions::PermissionRequestManager::FromWebContents(tab());
  ASSERT_TRUE(manager);
  permissions::MockPermissionPromptFactory factory(manager);

  AskNotifications();
  const std::string word = Prompts(voice)->current_word_for_testing();
  ASSERT_TRUE(MatchConfirmationWord(word)) << word;
  EXPECT_EQ(Prompts(voice)->ShownWord(), word);

  ASSERT_TRUE(content::ExecJs(tab(), "shareScreen()"));
  ASSERT_TRUE(WaitForLog("screen:"));
  if (!content::EvalJs(tab(), "moltLog.includes('screen:on')").ExtractBool()) {
    GTEST_SKIP() << "No screen to capture here";
  }
  // Taken off the screen, and the prompt read again without a word.
  EXPECT_TRUE(base::test::RunUntil(
      [&]() { return Prompts(voice)->ShownWord().empty(); }));
  EXPECT_TRUE(
      WaitForSpeech("I can't take the word while your screen is "
                    "shared"));
  // The word the capture may have seen allows nothing.
  voice->HandleHeardUtterance(word, /*heard_cleanly=*/true);
  EXPECT_TRUE(manager->IsRequestInProgress());

  // The share ends: a new word, shown again.
  ASSERT_TRUE(content::ExecJs(tab(), "stopAll()"));
  EXPECT_TRUE(WaitForSpeech("Your screen is no longer shared."));
  const std::string next = Prompts(voice)->current_word_for_testing();
  ASSERT_TRUE(MatchConfirmationWord(next)) << next;
  EXPECT_NE(next, word);
  EXPECT_EQ(Prompts(voice)->ShownWord(), next);
  // The word seen during the share is retired: it neither allows nor
  // refuses.
  voice->HandleHeardUtterance(word, /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForSpeech("That word changed."));
  EXPECT_TRUE(manager->IsRequestInProgress());
  voice->HandleHeardUtterance(next, /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForLog("notify:granted"));
  EXPECT_TRUE(NoWordSpoken());
#endif
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
