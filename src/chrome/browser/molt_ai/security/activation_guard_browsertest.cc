// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The activation guard (activation_guard.h,
// docs/design/TOUCHLESS_BROWSING.md §7.5) on a page that uses the user
// activation of whatever clicks it (chrome/test/data/molt_voice/
// activation.html), with a scripted model in place of the on-device one
// and a fake TTS platform:
//   - the web agent presses a button whose page opens another site in a new
//     tab: refused, then asked in the run, and opened on "Allow";
//   - voice mode presses a download link: held and asked ("Say yes to
//     download it"); "yes" starts it, and only then;
//   - a person's own click after an agent's action opens the tab as always;
//   - a permission prompt after voice mode's click is dismissed and said,
//     never granted, and the site isn't blocked for later;
//   - full screen nobody asked for is refused, and said; full screen asked
//     for is not.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/function_ref.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/molt_ai/agents/agent_features.h"
#include "chrome/browser/molt_ai/agents/web_agent.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/security/activation_guard.h"
#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/download/public/common/download_item.h"
#include "components/permissions/permission_request_manager.h"
#include "components/permissions/test/mock_permission_prompt_factory.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace {

using ActionResult = voice::VoicePageController::ActionResult;

// A TTS platform that records what it is asked to say and finishes each
// utterance at once (as voice_controller_browsertest.cc's).
class FakeTtsPlatform : public content::TtsPlatform {
 public:
  FakeTtsPlatform() = default;
  FakeTtsPlatform(const FakeTtsPlatform&) = delete;
  FakeTtsPlatform& operator=(const FakeTtsPlatform&) = delete;
  ~FakeTtsPlatform() = default;

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

// What voice mode says, as it starts saying it (whatever speaks it).
class SpeechLog : public voice::VoiceController::Observer {
 public:
  void OnVoiceSpeech(const std::string& text) override {
    if (!all_.empty()) {
      all_.push_back('|');
    }
    all_.append(text);
  }
  const std::string& all() const { return all_; }
  bool Heard(std::string_view text) const {
    return all_.find(text) != std::string::npos;
  }

 private:
  std::string all_;
};

// Counts the downloads the profile starts.
class DownloadCounter : public content::DownloadManager::Observer {
 public:
  explicit DownloadCounter(content::DownloadManager* manager) {
    observation_.Observe(manager);
  }
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override {
    ++created_;
  }
  int created() const { return created_; }

 private:
  int created_ = 0;
  base::ScopedObservation<content::DownloadManager,
                          content::DownloadManager::Observer>
      observation_{this};
};

// An asker that records the guard's questions and notices, answering
// nothing by itself.
class RecordingAsker : public ActivationGuard::Asker {
 public:
  bool AskAboutPageRequest(int id,
                           const activation::Question& question) override {
    asked.push_back(question.said);
    last_id = id;
    return true;
  }
  void TellAboutPageRequest(const std::string& text) override {
    told.push_back(text);
  }

  std::vector<std::string> asked;
  std::vector<std::string> told;
  int last_id = 0;
};

// The current page of a prompt, and the id of its ELEMENTS line with
// |needle| (as agent_page_map_browsertest.cc's).
std::string ElementWith(const std::string& prompt, std::string_view needle) {
  const size_t at = prompt.rfind("CURRENT PAGE:");
  const std::string page =
      at == std::string::npos ? std::string() : prompt.substr(at);
  const size_t list = page.rfind("ELEMENTS:\n");
  if (list == std::string::npos) {
    return std::string();
  }
  size_t start = list + 10;
  while (start < page.size()) {
    size_t end = page.find('\n', start);
    if (end == std::string::npos) {
      end = page.size();
    }
    const std::string line = page.substr(start, end - start);
    if (line.find(needle) != std::string::npos) {
      return line.substr(0, line.find(' '));
    }
    start = end + 1;
  }
  return std::string();
}

GenerationResult Reply(std::string json) {
  GenerationResult result;
  result.success = true;
  result.text = std::move(json);
  return result;
}

class ActivationGuardBrowserTest : public InProcessBrowserTest {
 public:
  ActivationGuardBrowserTest() {
    // One action at a time (no plan): the scripted replies need no "step".
    features_.InitWithFeatures(
        {agent_features::kMoltAgentPageMap, voice::kMoltVoiceMode},
        {agent_features::kMoltAgentPlanner});
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
  }

  void TearDownOnMainThread() override {
    WebAgent::SetModelForTesting(WebAgent::ModelForTesting());
    agent_.reset();
    if (voice::VoiceController* voice =
            voice::VoiceController::FromBrowser(browser())) {
      if (observing_) {
        voice->RemoveObserver(&speech_);
      }
      voice->Stop();
    }
    if (page_) {
      ActivationGuard::RemoveAsker(page_, &asker_);
      if (holds_page_) {
        if (auto* controller =
                voice::VoicePageController::FromWebContents(page_)) {
          controller->Stop(voice::VoicePageController::Holder::kWebAgent);
        }
      }
    }
    page_ = nullptr;
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 public:
  content::WebContents* active_tab() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }

  // Opens the fixture page in the active tab, which the test then keeps to
  // (a tab the page opens becomes the active one). On the test server's
  // "localhost": a secure context, as geolocation needs.
  void OpenPage() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL(
                       "localhost", "/molt_voice/activation.html")));
    page_ = active_tab();
  }

  content::EvalJsResult Js(std::string_view script) {
    return content::EvalJs(page_.get(), script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE);
  }

  // Waits until the page's log has |entry|.
  bool WaitForLog(std::string_view entry) {
    return base::test::RunUntil([&]() {
      return Js("moltLog.join(' ')").ExtractString().find(entry) !=
             std::string::npos;
    });
  }

  // The page's controller, held as an agent run holds it.
  voice::VoicePageController* HoldPage() {
    voice::VoicePageController::CreateForWebContents(page_);
    auto* controller = voice::VoicePageController::FromWebContents(page_);
    controller->Start(voice::VoicePageController::Holder::kWebAgent);
    holds_page_ = true;
    return controller;
  }

  // The recording asker answers for |who| on the page's tab.
  void UseAsker(activation::Asker who) {
    ActivationGuard::SetAsker(page_, who, &asker_);
  }

  // The item of |kind| named |label|, once the map has it.
  std::optional<voice::PageItem> WaitForItem(
      voice::VoicePageController* controller,
      voice::ItemKind kind,
      std::string_view label) {
    std::optional<voice::PageItem> found;
    EXPECT_TRUE(base::test::RunUntil([&]() {
      const voice::PageMap map = controller->BuildMap();
      for (const voice::PageItem& item : map.items) {
        if (item.kind == kind && item.label == label) {
          found = item;
          return true;
        }
      }
      return false;
    })) << "no " << label;
    return found;
  }

  voice::VoiceController* voice() {
    voice::VoiceController* controller =
        voice::VoiceController::GetOrCreateForBrowser(browser());
    if (controller && !observing_) {
      controller->AddObserver(&speech_);
      observing_ = true;
    }
    return controller;
  }

  // Says |text| and waits until the browser has done it and said all it
  // says.
  void Say(std::string_view text) {
    voice::VoiceController* controller = voice();
    ASSERT_TRUE(controller);
    controller->HandleUtterance(std::string(text));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return !controller->busy() &&
             controller->activity() != voice::VoiceActivity::kSpeaking;
    })) << "still busy after \"" << text << "\"";
  }

  FakeTtsPlatform tts_;
  SpeechLog speech_;
  RecordingAsker asker_;
  raw_ptr<content::WebContents> page_ = nullptr;
  bool observing_ = false;
  bool holds_page_ = false;
  std::unique_ptr<WebAgent> agent_;

 private:
  base::test::ScopedFeatureList features_;
};

// The agent presses "Open offer"; the page opens another site with the
// activation that gave it. The tab isn't opened: the run is asked, and
// "Allow" opens it.
IN_PROC_BROWSER_TEST_F(ActivationGuardBrowserTest,
                       AnAgentsClickOpensNoOtherSiteUnasked) {
  OpenPage();
  int replies = 0;
  WebAgent::SetModelForTesting(base::BindRepeating(
      [](int* replies, const std::string& prompt,
         const PromptOptions& options) {
        if ((*replies)++ == 0) {
          const std::string id = ElementWith(prompt, "button \"Open offer\"");
          EXPECT_FALSE(id.empty()) << prompt;
          return Reply("{\"reason\":\"\",\"action\":\"CLICK\",\"target\":\"" +
                       id + "\"}");
        }
        return Reply(
            "{\"reason\":\"\",\"action\":\"DONE\",\"result\":\"Opened\"}");
      },
      &replies));

  std::vector<std::string> questions;
  int tabs_when_asked = -1;
  bool done = false;
  bool success = false;
  agent_ =
      std::make_unique<WebAgent>(page_, &BrowserAIRuntime::GetInstance());
  agent_->Start(
      "open the offer", WebAgent::StepCallback(),
      base::BindOnce(
          [](bool* done, bool* success, bool ok, std::string result) {
            *done = true;
            *success = ok;
          },
          &done, &success),
      base::BindRepeating(
          [](ActivationGuardBrowserTest* self,
             std::vector<std::string>* questions, int* tabs_when_asked,
             const WebAgent::ConfirmRequest& request) {
            questions->push_back(request.action + ": " + request.message);
            *tabs_when_asked = self->browser()->GetTabStripModel()->count();
            // "Allow", as the chip's button.
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(
                               [](ActivationGuardBrowserTest* self, int id) {
                                 if (self->agent_) {
                                   self->agent_->ResolveConfirmation(id, true);
                                 }
                               },
                               base::Unretained(self), request.id));
          },
          base::Unretained(this), &questions, &tabs_when_asked),
      WebAgent::PlanCallback());
  ASSERT_TRUE(base::test::RunUntil([&]() { return done; }));
  EXPECT_TRUE(success);

  // The page's own call got nothing: refused, then asked.
  EXPECT_TRUE(WaitForLog("open:null"));
  ASSERT_EQ(questions.size(), 1u);
  EXPECT_EQ(questions[0],
            "PAGE_REQUEST: The page tried to open a new window to ads.test.");
  EXPECT_EQ(tabs_when_asked, 1);
  // "Allow" opened it, from the popup blocker's list.
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return browser()->GetTabStripModel()->count() == 2; }));
  content::WebContents* opened =
      browser()->GetTabStripModel()->GetWebContentsAt(1);
  EXPECT_TRUE(content::WaitForLoadStop(opened));
  EXPECT_EQ(opened->GetLastCommittedURL().host(), "ads.test");
}

// Voice mode presses a download link: the download waits for a spoken
// "yes", and starts only then.
IN_PROC_BROWSER_TEST_F(ActivationGuardBrowserTest,
                       VoiceModesDownloadClickIsAsked) {
  OpenPage();
  DownloadCounter downloads(browser()->GetProfile()->GetDownloadManager());
  ASSERT_TRUE(voice()->Start());
  ASSERT_TRUE(base::test::RunUntil([&]() {
    auto* controller = voice::VoicePageController::FromWebContents(page_);
    return controller && controller->HasPageTree();
  }));

  Say("click download report");
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return speech_.Heard(
        "The page tried to download a file from localhost.|Say yes to "
        "download it, or no to cancel.");
  })) << speech_.all();
  // Held: nothing was downloaded.
  EXPECT_EQ(downloads.created(), 0);
  ActivationGuard* guard = ActivationGuard::FromWebContents(page_);
  ASSERT_TRUE(guard);
  EXPECT_NE(guard->pending_question(), 0);

  Say("yes");
  ASSERT_TRUE(base::test::RunUntil([&]() { return downloads.created() == 1; }));
  EXPECT_EQ(guard->pending_question(), 0);
}

// After an agent's action, the person's own click is theirs: the page
// opens its tab as it always would.
IN_PROC_BROWSER_TEST_F(ActivationGuardBrowserTest,
                       APersonsOwnClickIsUnchanged) {
  OpenPage();
  voice::VoicePageController* controller = HoldPage();
  UseAsker(activation::Asker::kWebAgent);
  const std::optional<voice::PageItem> locate =
      WaitForItem(controller, voice::ItemKind::kButton, "Find stores");
  ASSERT_TRUE(locate);
  // An action: the tab acts for the agent.
  ASSERT_EQ(controller->Focus(locate->handle), ActionResult::kDone);
  ActivationGuard* guard = ActivationGuard::FromWebContents(page_);
  ASSERT_TRUE(guard);
  EXPECT_TRUE(guard->IsActing());

  // The person clicks "Open offer" themselves.
  content::SimulateMouseClickOrTapElementWithId(page_, "offer");
  EXPECT_TRUE(WaitForLog("open:window"));
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return browser()->GetTabStripModel()->count() == 2; }));
  EXPECT_FALSE(guard->IsActing());
  EXPECT_TRUE(asker_.asked.empty());
}

// A permission prompt after voice mode's click: shown, then dismissed and
// said, never granted; nothing is remembered for the site.
IN_PROC_BROWSER_TEST_F(ActivationGuardBrowserTest,
                       APromptAfterAVoiceClickIsNotGrantedSilently) {
  OpenPage();
  auto* prompts = permissions::PermissionRequestManager::FromWebContents(page_);
  ASSERT_TRUE(prompts);
  // A prompt that stays until it is answered.
  permissions::MockPermissionPromptFactory prompt_factory(prompts);

  voice::VoicePageController* controller = HoldPage();
  UseAsker(activation::Asker::kVoiceMode);
  const std::optional<voice::PageItem> locate =
      WaitForItem(controller, voice::ItemKind::kButton, "Find stores");
  ASSERT_TRUE(locate);
  ASSERT_EQ(controller->Press(locate->handle), ActionResult::kDone);

  EXPECT_TRUE(WaitForLog("location:denied"));
  EXPECT_EQ(false, Js("moltLog.includes('location:granted')"));
  EXPECT_GE(prompt_factory.show_count(), 1);
  EXPECT_FALSE(prompts->IsRequestInProgress());
  ASSERT_EQ(asker_.told.size(), 1u);
  EXPECT_EQ(asker_.told[0],
            "The page asked for location permission. I can't take an answer "
            "to that by voice yet, so I turned it down.");
  // Dismissed, not denied: the site may ask again another time.
  const GURL url = page_->GetLastCommittedURL();
  HostContentSettingsMap* settings =
      HostContentSettingsMapFactory::GetForProfile(browser()->GetProfile());
  EXPECT_EQ(settings->GetContentSetting(url, url,
                                        ContentSettingsType::GEOLOCATION),
            CONTENT_SETTING_ASK);
}

// Full screen nobody asked for is refused (and voice mode says so); after a
// press for "full screen" it isn't.
IN_PROC_BROWSER_TEST_F(ActivationGuardBrowserTest,
                       FullScreenOnlyWhenAskedFor) {
  OpenPage();
  voice::VoicePageController* controller = HoldPage();
  UseAsker(activation::Asker::kVoiceMode);
  const std::optional<voice::PageItem> big =
      WaitForItem(controller, voice::ItemKind::kButton, "Show big");
  ASSERT_TRUE(big);

  ASSERT_EQ(controller->Press(big->handle), ActionResult::kDone);
  EXPECT_TRUE(WaitForLog("fullscreen:no"));
  EXPECT_FALSE(page_->IsFullscreen());
  ASSERT_EQ(asker_.told.size(), 1u);
  EXPECT_EQ(asker_.told[0],
            "The page tried to go full screen. I stopped it. Say full screen "
            "if you want it.");

  ASSERT_EQ(controller->Press(big->handle, activation::Intent::kFullScreen),
            ActionResult::kDone);
  EXPECT_TRUE(WaitForLog("fullscreen:yes"));
}

// One question per action: once the page's new window was asked about, its
// next request before the next action (a download it starts itself) is
// refused and said, not asked in the first one's place.
IN_PROC_BROWSER_TEST_F(ActivationGuardBrowserTest, OneQuestionPerAction) {
  OpenPage();
  DownloadCounter downloads(browser()->GetProfile()->GetDownloadManager());
  voice::VoicePageController* controller = HoldPage();
  UseAsker(activation::Asker::kWebAgent);
  const std::optional<voice::PageItem> offer =
      WaitForItem(controller, voice::ItemKind::kButton, "Open offer");
  ASSERT_TRUE(offer);
  ASSERT_EQ(controller->Press(offer->handle), ActionResult::kDone);
  ASSERT_TRUE(
      base::test::RunUntil([&]() { return asker_.asked.size() == 1u; }));
  ActivationGuard* guard = ActivationGuard::FromWebContents(page_);
  ASSERT_TRUE(guard);
  guard->Answer(asker_.last_id, false);
  ASSERT_TRUE(guard->IsActing());

  ASSERT_TRUE(content::ExecJs(page_,
                              "document.getElementById('report').click()",
                              content::EXECUTE_SCRIPT_NO_USER_GESTURE));
  ASSERT_TRUE(base::test::RunUntil([&]() { return !asker_.told.empty(); }));
  EXPECT_EQ(asker_.told[0],
            "The page tried to download a file. I stopped it.");
  EXPECT_EQ(asker_.asked.size(), 1u);
  EXPECT_EQ(guard->pending_question(), 0);
  EXPECT_EQ(downloads.created(), 0);
}

}  // namespace
}  // namespace molt_ai
