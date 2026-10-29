// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Touchless browsing's Phase 0 exit criterion through the whole voice loop
// (docs/design/TOUCHLESS_BROWSING.md §2, §10): typed utterances, as the
// speech service will hand them over, drive the fixture video site
// (chrome/test/data/molt_voice/) by name: open it, search, hear the results
// (spoken through a fake TTS platform that records them), play the fourth
// one with sound, pause, go back, and play "the live one by Queen" after
// the browser asks which. Also: a purchase that waits for a spoken "yes",
// a MoltNet window where voice mode stays off, and a site the browser
// doesn't know, searched for with the default search engine.

#include "chrome/browser/molt_ai/voice/voice_controller.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/function_ref.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/search_test_utils.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_data.h"
#include "components/search_engines/template_url_service.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/media_switches.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {
namespace {

// A TTS platform that records what it is asked to say and finishes each
// utterance at once, as a very fast voice would.
class FakeTtsPlatform : public content::TtsPlatform {
 public:
  FakeTtsPlatform() = default;
  FakeTtsPlatform(const FakeTtsPlatform&) = delete;
  FakeTtsPlatform& operator=(const FakeTtsPlatform&) = delete;
  ~FakeTtsPlatform() = default;

  const std::vector<std::string>& spoken() const { return spoken_; }
  // Everything said since the last call, joined by "|".
  std::string TakeSpoken() {
    std::string out;
    for (size_t i = taken_; i < spoken_.size(); ++i) {
      if (!out.empty()) {
        out.push_back('|');
      }
      out.append(spoken_[i]);
    }
    taken_ = spoken_.size();
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
    std::move(did_start_speaking).Run(true);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](int id, int length) {
                         content::TtsController::GetInstance()->OnTtsEvent(
                             id, content::TTS_EVENT_END, length, 0,
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
  size_t taken_ = 0;
};

class VoiceControllerBrowserTest : public InProcessBrowserTest {
 public:
  VoiceControllerBrowserTest() {
    features_.InitAndEnableFeature(kMoltVoiceMode);
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // Unmuted media may start only after user activation, so the video
    // test shows the voice loop's click gave the page activation.
    command_line->AppendSwitchASCII(
        switches::kAutoplayPolicy,
        switches::autoplay::kDocumentUserActivationRequiredPolicy);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);

    // The fixture video site, under the name "VideoSite".
    video_home_ =
        Url("video.test", "/molt_voice/video_home.html").spec();
    video_search_ =
        Url("video.test", "/molt_voice/video_results.html?search_query=")
            .spec();
    test_site_ = std::make_unique<ScopedTestSite>(
        KnownSite{"VideoSite", video_home_, video_search_, {"videosite"},
                  {"video.test"}});
  }

  void TearDownOnMainThread() override {
    if (VoiceController* voice = VoiceController::FromBrowser(browser())) {
      voice->Stop();
    }
    test_site_.reset();
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }

  VoiceController* voice(BrowserWindowInterface* window = nullptr) {
    return VoiceController::GetOrCreateForBrowser(window ? window : browser());
  }

  GURL Url(std::string_view host, std::string_view path) {
    return embedded_test_server()->GetURL(host, path);
  }

  // Says |text| and waits until the browser has done it and said all it
  // says (waits for pages included).
  void Say(std::string_view text, BrowserWindowInterface* window = nullptr) {
    VoiceController* controller = voice(window);
    ASSERT_TRUE(controller);
    controller->HandleUtterance(std::string(text));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return !controller->busy() &&
             controller->activity() != VoiceActivity::kSpeaking;
    })) << "still busy after \"" << text << "\"";
  }

  // Waits until the active tab's map has what |ready| looks for.
  void WaitForMap(base::FunctionRef<bool(const PageMap&)> ready) {
    VoicePageController* page =
        VoicePageController::FromWebContents(web_contents());
    ASSERT_TRUE(page);
    ASSERT_TRUE(
        base::test::RunUntil([&]() { return ready(page->BuildMap()); }));
  }

  bool WaitForJs(std::string_view expression) {
    const std::string script = base::StrCat(
        {"new Promise(resolve => { const check = () => { if (", expression,
         ") { resolve(true); } else { setTimeout(check, 20); } }; "
         "check(); })"});
    return content::EvalJs(web_contents(), script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE) == true;
  }

  content::EvalJsResult Js(std::string_view script) {
    return content::EvalJs(web_contents(), script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE);
  }

  // The watch page plays the video with sound, after a trusted click that
  // gave the page user activation.
  void ExpectPlayingWithSound() {
    EXPECT_EQ("playing", Js("whenPlaying()"));
    EXPECT_EQ(true, Js("clickInfo.trusted && clickInfo.active"));
    EXPECT_EQ(true, Js("navigator.userActivation.hasBeenActive"));
    EXPECT_EQ(true, Js("const p = document.getElementById('player'); "
                       "!p.paused && !p.muted && p.volume > 0"));
  }

  FakeTtsPlatform tts_;

 private:
  base::test::ScopedFeatureList features_;
  std::string video_home_;
  std::string video_search_;
  std::unique_ptr<ScopedTestSite> test_site_;
};

IN_PROC_BROWSER_TEST_F(VoiceControllerBrowserTest, TheOwnersScenario) {
  // "Open VideoSite": the browser opens it and says so.
  Say("Hey Molt, open VideoSite");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().path(),
            "/molt_voice/video_home.html");
  EXPECT_EQ(tts_.TakeSpoken(), "VideoSite is open.");
  EXPECT_TRUE(voice()->active());
  EXPECT_TRUE(web_contents()->GetAccessibilityMode().has_mode(
      VoicePageController::AccessibilityMode().flags()));

  // "Search for Bohemian Rhapsody": typed into the site's own search box,
  // submitted, and the results read.
  Say("search for Bohemian Rhapsody");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().path(),
            "/molt_voice/video_results.html");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(),
            "search_query=Bohemian%20Rhapsody");
  std::string said = tts_.TakeSpoken();
  EXPECT_EQ(said.find("20 results. 1: Queen \xE2\x80\x93 Bohemian Rhapsody "
                      "(Official Video Remastered), Queen Official, 6 "
                      "minutes. 2: Bohemian Rhapsody (Remastered 2011), "
                      "Queen - Topic, 6 minutes."),
            0u)
      << said;
  EXPECT_NE(said.find("4: Queen - Bohemian Rhapsody (Live Aid 1985), Queen "
                      "Official, 3 minutes."),
            std::string::npos);
  EXPECT_NE(said.find("Say a number or a name."), std::string::npos);

  // "Play the fourth one": opened, then its video plays with sound.
  Say("play the fourth one");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");
  ExpectPlayingWithSound();
  EXPECT_EQ(tts_.TakeSpoken(),
            "Playing Queen - Bohemian Rhapsody (Live Aid 1985).");

  // "Pause", as the media keys do.
  Say("pause");
  EXPECT_TRUE(WaitForJs("document.getElementById('player').paused"));
  EXPECT_EQ(tts_.TakeSpoken(), "Paused.");

  // "Go back": the results again, ready to choose from.
  Say("go back");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().path(),
            "/molt_voice/video_results.html");
  EXPECT_EQ(tts_.TakeSpoken(),
            "Bohemian Rhapsody - VideoSite.|20 items. Say a number or a "
            "name.");

  // "Play the live one by Queen": three fit, so the browser asks...
  Say("play the live one by Queen");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().path(),
            "/molt_voice/video_results.html");
  EXPECT_EQ(tts_.TakeSpoken(),
            "Did you mean 1: Queen - Bohemian Rhapsody (Live at Rock "
            "Montreal 1981), Queen Official, 6 minutes, 2: Queen - Bohemian "
            "Rhapsody (Live Aid 1985), Queen Official, 3 minutes, or 3: "
            "Queen - Bohemian Rhapsody (Live at Wembley Stadium 1986), Queen "
            "Official, 6 minutes? Say a number or a name.");
  // ... and "the Live Aid one" plays it.
  Say("the Live Aid one");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");
  ExpectPlayingWithSound();
  EXPECT_EQ(tts_.TakeSpoken(),
            "Playing Queen - Bohemian Rhapsody (Live Aid 1985).");

  // "Stop" silences it; voice mode stays on. Off releases the page.
  Say("stop");
  EXPECT_TRUE(voice()->active());
  voice()->Stop();
  EXPECT_FALSE(web_contents()->GetAccessibilityMode().has_mode(
      VoicePageController::AccessibilityMode().flags()));
}

IN_PROC_BROWSER_TEST_F(VoiceControllerBrowserTest, BuysOnlyAfterASpokenYes) {
  ASSERT_TRUE(voice()->Start());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("shop.test", "/molt_voice/shop.html")));
  WaitForMap([](const PageMap& map) { return map.items.size() >= 2; });

  // Something that can be undone is pressed at once.
  Say("click add to cart");
  EXPECT_TRUE(WaitForJs("window.added === 1"));
  EXPECT_EQ(tts_.TakeSpoken(), "Pressed Add to cart.");

  // Buying waits for a "yes"; "no" leaves it.
  Say("click buy now");
  EXPECT_EQ(tts_.TakeSpoken(), "Say yes to press Buy now, or no to cancel.");
  Say("no");
  EXPECT_EQ(tts_.TakeSpoken(), "Okay, I won't.");
  EXPECT_EQ(0, Js("window.bought"));

  // Another command in between drops the question.
  Say("click buy now");
  Say("scroll down");
  Say("yes");
  EXPECT_EQ(0, Js("window.bought"));
  tts_.TakeSpoken();

  // "Yes" right after the question presses it, as a trusted click.
  Say("click buy now");
  Say("yes");
  EXPECT_TRUE(WaitForJs("window.bought === 1"));
  EXPECT_EQ(true, Js("window.buyTrusted"));
  EXPECT_EQ(tts_.TakeSpoken(),
            "Say yes to press Buy now, or no to cancel.|Pressed Buy now.");
}

IN_PROC_BROWSER_TEST_F(VoiceControllerBrowserTest, StaysOffInMoltNetWindows) {
  Profile* moltnet = GetProfile()->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUnique("MoltNet::Tor"),
      /*create_if_needed=*/true);
  BrowserWindowInterface* window = CreateBrowser(moltnet);
  ASSERT_TRUE(window);
  content::WebContents* tab =
      window->GetTabStripModel()->GetActiveWebContents();
  const GURL before = tab->GetLastCommittedURL();

  VoiceController* controller = voice(window);
  ASSERT_TRUE(controller);
  EXPECT_FALSE(controller->Start());
  Say("open VideoSite", window);
  EXPECT_EQ(tts_.TakeSpoken(), "Voice mode is off in MoltNet windows.");
  EXPECT_FALSE(controller->active());
  EXPECT_EQ(tab->GetLastCommittedURL(), before);
  EXPECT_FALSE(VoicePageController::FromWebContents(tab) &&
               VoicePageController::FromWebContents(tab)->active());
  EXPECT_FALSE(tab->GetAccessibilityMode().has_mode(
      VoicePageController::AccessibilityMode().flags()));
}

IN_PROC_BROWSER_TEST_F(VoiceControllerBrowserTest,
                       SearchesForASiteItDoesntKnow) {
  // The default search engine is the fixture search engine.
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(GetProfile());
  search_test_utils::WaitForTemplateURLServiceToLoad(service);
  TemplateURLData data;
  data.SetShortName(u"Fixture search");
  data.SetKeyword(u"search.test");
  data.SetURL(base::StrCat(
      {"http://search.test:",
       base::NumberToString(embedded_test_server()->port()),
       "/molt_voice/search_results.html?q={searchTerms}"}));
  TemplateURL* engine = service->Add(std::make_unique<TemplateURL>(data));
  ASSERT_TRUE(engine);
  service->SetUserSelectedDefaultSearchProvider(engine);

  // Never a made-up domain: the name is searched for, and the browser
  // says so, then reads the results.
  Say("open Netflix");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().host(), "search.test");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "q=Netflix");
  const std::string said = tts_.TakeSpoken();
  EXPECT_EQ(said.find("I don't know a site called Netflix, so I searched for "
                      "it.|10 results. 1: Bohemian Rhapsody, Wikipedia."),
            0u)
      << said;
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
