// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's prompts and a page's dialogs answered by voice
// (voice_browser_prompts.h, docs/design/TOUCHLESS_BROWSING.md §7.3), on
// chrome/test/data/molt_voice/prompts.html, with a fake TTS platform and
// the listening session's part played by the test
// (VoiceController::HandleHeardUtterance()):
//   - a permission prompt is read with a confirmation word (shown, not said:
//     the test speaks with a platform voice, voice_gesture_channel.h); the
//     word, heard cleanly, allows it; "no" blocks it;
//   - the word typed, heard over other speech, said again for the next
//     prompt (a replay) or said after the prompt's window is refused;
//   - confirm() is answered by "yes" and "no"; prompt() by a dictated
//     answer and "yes"; beforeunload by "stay" and "leave".

#include <string>
#include <string_view>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "chrome/browser/download/download_item_warning_data.h"
#include "chrome/browser/molt_ai/voice/voice_browser_prompts.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/passwords/manage_passwords_ui_controller.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/download/public/common/download_danger_type.h"
#include "components/download/public/common/download_item.h"
#include "components/password_manager/core/browser/mock_password_form_manager_for_ui.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/common/password_manager_ui.h"
#include "components/permissions/permission_request_manager.h"
#include "components/permissions/test/mock_permission_prompt_factory.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/download_test_observer.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {
namespace {

// Finishes each utterance at once (as voice_controller_browsertest.cc's).
class FakeTtsPlatform : public content::TtsPlatform {
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
};

// What voice mode says.
class SpeechLog : public VoiceController::Observer {
 public:
  void OnVoiceSpeech(const std::string& text) override {
    said_.push_back(text);
  }
  bool Heard(std::string_view text) const {
    for (const std::string& said : said_) {
      if (said.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  // The confirmation word the last prompt said ("... say harbor. ...").
  std::string LastWord(std::string_view before = "To allow it, say ") const {
    for (auto it = said_.rbegin(); it != said_.rend(); ++it) {
      const size_t at = it->find(before);
      if (at != std::string::npos) {
        const size_t start = at + before.size();
        return it->substr(start, it->find('.', start) - start);
      }
    }
    return std::string();
  }
  size_t count() const { return said_.size(); }

 private:
  std::vector<std::string> said_;
};

class VoiceBrowserPromptsBrowserTest : public InProcessBrowserTest {
 public:
  VoiceBrowserPromptsBrowserTest() {
    features_.InitAndEnableFeature(kMoltVoiceMode);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(embedded_test_server()->Start());
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
  }

  void TearDownOnMainThread() override {
    if (VoiceController* voice = VoiceController::FromBrowser(browser())) {
      voice->RemoveObserver(&speech_);
      voice->Stop();
    }
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

  content::WebContents* tab() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }

  // The page, then voice mode on it, listening.
  VoiceController* Start() {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("localhost",
                                                  "/molt_voice/prompts.html")));
    VoiceController* voice = VoiceController::GetOrCreateForBrowser(browser());
    EXPECT_TRUE(voice);
    voice->AddObserver(&speech_);
    EXPECT_TRUE(voice->Start());
    VoiceController::ListeningCaption caption;
    caption.listening = true;
    voice->SetListening(caption);
    return voice;
  }

  content::EvalJsResult Js(std::string_view script) {
    return content::EvalJs(tab(), script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE);
  }

  bool WaitForLog(std::string_view entry) {
    return base::test::RunUntil([&]() {
      return Js("moltLog.join(' ')").ExtractString().find(entry) !=
             std::string::npos;
    });
  }

  bool WaitForSpeech(std::string_view text) {
    return base::test::RunUntil([&]() { return speech_.Heard(text); });
  }

  // Asks for notifications, and waits until voice mode has read it; its
  // word (shown in the caption bar, never said here).
  std::string AskNotifications() {
    const size_t before = speech_.count();
    EXPECT_TRUE(content::ExecJs(tab(), "askNotifications()",
                                content::EXECUTE_SCRIPT_NO_USER_GESTURE));
    EXPECT_TRUE(base::test::RunUntil([&]() {
      return speech_.count() > before &&
             speech_.Heard("localhost wants to show notifications.");
    }));
    return Word();
  }

  // The waiting prompt's word, as the caption bar shows it.
  std::string Word() {
    VoiceController* voice = VoiceController::FromBrowser(browser());
    VoiceBrowserPrompts* prompts =
        voice ? voice->browser_prompts_for_testing() : nullptr;
    EXPECT_TRUE(prompts);
    if (!prompts) {
      return std::string();
    }
    EXPECT_EQ(prompts->ShownWord(), prompts->current_word_for_testing());
    return prompts->current_word_for_testing();
  }

  // A file dangerous for its type (.swf, on every platform) downloaded in
  // the tab; the download system decides its danger (no Safe Browsing).
  download::DownloadItem* DownloadDangerousFile() {
    browser()->GetProfile()->GetPrefs()->SetBoolean(prefs::kSafeBrowsingEnabled,
                                                    false);
    content::DownloadManager* manager =
        browser()->GetProfile()->GetDownloadManager();
    content::DownloadTestObserverTerminal dangerous(
        manager, 1, content::DownloadTestObserver::ON_DANGEROUS_DOWNLOAD_QUIT);
    EXPECT_TRUE(content::ExecJs(
        tab(), "location.href = '/downloads/dangerous/dangerous.swf';"));
    dangerous.WaitForFinished();
    std::vector<raw_ptr<download::DownloadItem, VectorExperimental>> downloads;
    manager->GetAllDownloads(&downloads);
    EXPECT_EQ(downloads.size(), 1u);
    return downloads.empty() ? nullptr : downloads[0].get();
  }

  FakeTtsPlatform tts_;
  SpeechLog speech_;
  // The form the password manager offers to save (outlives the tab).
  password_manager::PasswordForm form_;

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       APermissionPromptAllowedByItsWord) {
  VoiceController* voice = Start();
  auto* prompts = permissions::PermissionRequestManager::FromWebContents(tab());
  ASSERT_TRUE(prompts);
  permissions::MockPermissionPromptFactory factory(prompts);

  const std::string word = AskNotifications();
  ASSERT_TRUE(MatchConfirmationWord(word)) << word;
  // Never said with the platform's voice, nor given to observers as text.
  EXPECT_TRUE(WaitForSpeech("To allow it, say the word on your screen."));
  EXPECT_FALSE(speech_.Heard(" say " + word + "."));
  // A plain "yes" doesn't allow it.
  voice->HandleHeardUtterance("yes", /*heard_cleanly=*/true);
  EXPECT_TRUE(base::test::RunUntil(
      [&]() { return speech_.LastWord() == "the word on your screen"; }));
  EXPECT_FALSE(speech_.Heard(" say " + word + "."));
  EXPECT_TRUE(prompts->IsRequestInProgress());

  voice->HandleHeardUtterance(word, /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForLog("notify:granted"));
  EXPECT_FALSE(prompts->IsRequestInProgress());
}

IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       APermissionPromptRefusedByNo) {
  VoiceController* voice = Start();
  auto* prompts = permissions::PermissionRequestManager::FromWebContents(tab());
  ASSERT_TRUE(prompts);
  permissions::MockPermissionPromptFactory factory(prompts);

  AskNotifications();
  voice->HandleHeardUtterance("no", /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForLog("notify:denied"));
}

IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       TheWordTypedUncleanReplayedOrLateIsRefused) {
  VoiceController* voice = Start();
  auto* prompts = permissions::PermissionRequestManager::FromWebContents(tab());
  ASSERT_TRUE(prompts);
  permissions::MockPermissionPromptFactory factory(prompts);

  const std::string first = AskNotifications();
  // Typed: never a gesture.
  voice->HandleUtterance(first);
  // Heard over other speech: say it again.
  voice->HandleHeardUtterance(first, /*heard_cleanly=*/false);
  EXPECT_TRUE(WaitForSpeech("I couldn't be sure that was you."));
  EXPECT_TRUE(prompts->IsRequestInProgress());
  EXPECT_EQ(false, Js("moltLog.includes('notify:granted')"));

  // Late: past the prompt's window, the word allows nothing.
  VoiceBrowserPrompts* browser_prompts = voice->browser_prompts_for_testing();
  ASSERT_TRUE(browser_prompts);
  const base::TimeTicks late =
      base::TimeTicks::Now() + base::Milliseconds(kGesturePromptWindowMs + 1);
  browser_prompts->SetClockForTesting(
      base::BindRepeating([](base::TimeTicks at) { return at; }, late));
  voice->HandleHeardUtterance(first, /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForSpeech("I can't take the word now."));
  EXPECT_EQ(false, Js("moltLog.includes('notify:granted')"));
  browser_prompts->SetClockForTesting(
      base::RepeatingCallback<base::TimeTicks()>());

  // The prompt is still on screen; dismiss it, and ask again: the old word
  // (a replay) is another word of the list now, which refuses the prompt.
  prompts->Dismiss(PromptOptions(std::monostate()));
  EXPECT_TRUE(WaitForLog("notify:default"));
  const std::string second = AskNotifications();
  ASSERT_NE(first, second);
  voice->HandleHeardUtterance(first, /*heard_cleanly=*/true);
  EXPECT_TRUE(WaitForSpeech("That wasn't the word, so I turned it down."));
  EXPECT_TRUE(
      base::test::RunUntil([&]() { return !prompts->IsRequestInProgress(); }));
  EXPECT_EQ(false, Js("moltLog.includes('notify:granted')"));
}

IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest, ConfirmAnsweredByVoice) {
  VoiceController* voice = Start();
  ASSERT_TRUE(content::ExecJs(tab(), "askConfirm('Empty the cart?')",
                              content::EXECUTE_SCRIPT_NO_USER_GESTURE));
  EXPECT_TRUE(WaitForSpeech("localhost asks: Empty the cart? Say yes or no."));
  voice->HandleHeardUtterance("yes", /*heard_cleanly=*/false);
  EXPECT_TRUE(WaitForLog("confirm:true"));

  ASSERT_TRUE(content::ExecJs(tab(), "askConfirm('Delete the list?')",
                              content::EXECUTE_SCRIPT_NO_USER_GESTURE));
  EXPECT_TRUE(WaitForSpeech("localhost asks: Delete the list?"));
  voice->HandleHeardUtterance("no", /*heard_cleanly=*/false);
  EXPECT_TRUE(WaitForLog("confirm:false"));
}

IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       PromptAnsweredByDictation) {
  VoiceController* voice = Start();
  ASSERT_TRUE(content::ExecJs(tab(), "askPrompt('Your name?')",
                              content::EXECUTE_SCRIPT_NO_USER_GESTURE));
  EXPECT_TRUE(WaitForSpeech("localhost asks: Your name?"));
  voice->HandleHeardUtterance("answer Ada Lovelace", /*heard_cleanly=*/false);
  EXPECT_TRUE(WaitForSpeech("Your answer: Ada Lovelace."));
  voice->HandleHeardUtterance("yes", /*heard_cleanly=*/false);
  EXPECT_TRUE(WaitForLog("prompt:Ada Lovelace"));
}

IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       BeforeUnloadAnsweredByVoice) {
  VoiceController* voice = Start();
  // The user's own input gives the page the sticky activation the dialog
  // needs.
  ASSERT_TRUE(content::ExecJs(tab(), "keepChanges()"));
  const GURL page = tab()->GetLastCommittedURL();
  const GURL away =
      embedded_test_server()->GetURL("localhost", "/molt_voice/offer.html");

  tab()->GetController().LoadURL(away, content::Referrer(),
                                 ui::PAGE_TRANSITION_TYPED, std::string());
  EXPECT_TRUE(WaitForSpeech(
      "Leave localhost? Changes you made may not be saved. Say leave or "
      "stay."));
  voice->HandleHeardUtterance("stay", /*heard_cleanly=*/false);
  EXPECT_TRUE(WaitForSpeech("Staying on the page."));
  EXPECT_TRUE(base::test::RunUntil([&]() { return !tab()->IsLoading(); }));
  EXPECT_EQ(tab()->GetLastCommittedURL(), page);

  tab()->GetController().LoadURL(away, content::Referrer(),
                                 ui::PAGE_TRANSITION_TYPED, std::string());
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return voice->state().prompt.kind == PromptKind::kBeforeUnload &&
           voice->state().prompt.id != 0;
  }));
  voice->HandleHeardUtterance("leave", /*heard_cleanly=*/false);
  EXPECT_TRUE(base::test::RunUntil(
      [&]() { return tab()->GetLastCommittedURL() == away; }));
}

// A file the bubble warns about for its type ("Download unverified file"):
// kept by its word as the bubble's warning keeps it, one word for its one
// click, with the warning's actions recorded as the bubble records them.
IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       ADangerousDownloadKeptAsTheBubbleKeepsIt) {
  VoiceController* voice = Start();
  download::DownloadItem* item = DownloadDangerousFile();
  ASSERT_TRUE(item);
  ASSERT_EQ(item->GetDangerType(),
            download::DOWNLOAD_DANGER_TYPE_DANGEROUS_FILE);

  ASSERT_TRUE(WaitForSpeech(
      "may be harmful. To keep it, say the word on your screen."));
  const std::string word = Word();
  ASSERT_TRUE(MatchConfirmationWord(word)) << word;
  EXPECT_FALSE(DownloadItemWarningData::WarningFirstShownTime(item).is_null());
  voice->HandleHeardUtterance(word, /*heard_cleanly=*/true);
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return item->GetDangerType() ==
           download::DOWNLOAD_DANGER_TYPE_USER_VALIDATED;
  }));
  bool proceeded = false;
  for (const DownloadItemWarningData::WarningActionEvent& event :
       DownloadItemWarningData::GetWarningActionEvents(item)) {
    proceeded =
        proceeded ||
        (event.surface ==
             DownloadItemWarningData::WarningSurface::BUBBLE_SUBPAGE &&
         event.action == DownloadItemWarningData::WarningAction::PROCEED);
  }
  EXPECT_TRUE(proceeded);
}

// The save bubble, whenever the password manager offers it (here well
// after the page loaded, with no navigation): read as its key icon changes,
// with the site only.
IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       ASaveBubbleOfferedLaterIsRead) {
  VoiceController* voice = Start();
  ASSERT_TRUE(base::test::RunUntil([&]() { return !tab()->IsLoading(); }));
  ManagePasswordsUIController* passwords =
      ManagePasswordsUIController::FromWebContents(tab());
  ASSERT_TRUE(passwords);
  ASSERT_NE(passwords->GetState(),
            password_manager::ui::PENDING_PASSWORD_STATE);

  form_.url = tab()->GetLastCommittedURL();
  form_.signon_realm = form_.url.GetWithEmptyPath().spec();
  form_.username_value = u"ada.lovelace";
  auto manager = std::make_unique<
      testing::NiceMock<password_manager::MockPasswordFormManagerForUI>>();
  ON_CALL(*manager, GetURL()).WillByDefault(testing::ReturnRef(form_.url));
  ON_CALL(*manager, GetPendingCredentials())
      .WillByDefault(testing::ReturnRef(form_));
  ON_CALL(*manager, IsFetchCompleted()).WillByDefault(testing::Return(true));
  passwords->OnPasswordSubmitted(std::move(manager));
  ASSERT_EQ(passwords->GetState(),
            password_manager::ui::PENDING_PASSWORD_STATE);

  EXPECT_TRUE(WaitForSpeech("Save your password for localhost?"));
  EXPECT_FALSE(speech_.Heard("ada"));
  voice->HandleHeardUtterance("not now", /*heard_cleanly=*/false);
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return passwords->GetState() !=
           password_manager::ui::PENDING_PASSWORD_STATE;
  }));
}

IN_PROC_BROWSER_TEST_F(VoiceBrowserPromptsBrowserTest,
                       ADangerousDownloadDiscardedByVoice) {
  VoiceController* voice = Start();
  download::DownloadItem* item = DownloadDangerousFile();
  ASSERT_TRUE(item);
  ASSERT_TRUE(WaitForSpeech("may be harmful."));
  voice->HandleHeardUtterance("discard", /*heard_cleanly=*/false);
  EXPECT_TRUE(base::test::RunUntil(
      [&]() { return item->GetState() == download::DownloadItem::CANCELLED; }));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
