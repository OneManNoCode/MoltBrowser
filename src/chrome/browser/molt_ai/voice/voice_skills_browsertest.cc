// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site skills and reading aloud through the whole voice loop
// (docs/design/TOUCHLESS_BROWSING.md §5.4, §6.7; docs/design/
// VOICE_SKILLS.md), on the stored copies of the sites' pages
// (chrome/test/data/molt_voice/skills/) served under test hosts that the
// skills are bound to: "search YouTube for ..." fills the fixture's box
// through its skill and the results are read as the skill names them;
// "skip the ad" presses the player's button; "read the page" reads the
// article's text (captured from a fake TTS platform), leaving out what the
// skill leaves out; "next heading" and "next paragraph" move and read;
// "read the selection" reads what is selected.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/site_skills.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
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
namespace voice {
namespace {

// A TTS platform that records what it is asked to say and finishes each
// utterance at once (as voice_controller_browsertest.cc's).
class FakeTtsPlatform : public content::TtsPlatform {
 public:
  FakeTtsPlatform() = default;
  FakeTtsPlatform(const FakeTtsPlatform&) = delete;
  FakeTtsPlatform& operator=(const FakeTtsPlatform&) = delete;
  ~FakeTtsPlatform() = default;

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

 private:
  std::vector<std::string> spoken_;
  size_t taken_ = 0;
};

constexpr char kYouTubeHost[] = "youtube.test";
constexpr char kWikipediaHost[] = "wikipedia.test";

class VoiceSkillsBrowserTest : public InProcessBrowserTest {
 public:
  VoiceSkillsBrowserTest() { features_.InitAndEnableFeature(kMoltVoiceMode); }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
  }

  void TearDownOnMainThread() override {
    if (VoiceController* voice = VoiceController::FromBrowser(browser())) {
      voice->Stop();
    }
    test_site_.reset();
    skill_host_.reset();
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }

  VoiceController* voice() {
    return VoiceController::GetOrCreateForBrowser(browser());
  }

  GURL Url(std::string_view host, std::string_view file) {
    return embedded_test_server()->GetURL(
        host, base::StrCat({"/molt_voice/skills/", file}));
  }

  // The fixture site |host| is the skill |skill|'s site, under the name
  // |name| (for "search YouTube for ...").
  void BindSkill(std::string_view host,
                 std::string_view skill,
                 std::string_view name,
                 std::string_view home,
                 std::string_view search) {
    skill_host_ =
        std::make_unique<ScopedSkillHostForTesting>(std::string(host), skill);
    home_ = Url(host, home).spec();
    search_ = Url(host, search).spec();
    name_ = std::string(name);
    folded_name_ = std::string(skill);
    host_ = std::string(host);
    test_site_ = std::make_unique<ScopedTestSite>(
        KnownSite{name_, home_, search_, {folded_name_}, {host_}});
  }

  void Open(const GURL& url) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
    ASSERT_TRUE(voice()->Start());
    VoicePageController* page =
        VoicePageController::FromWebContents(web_contents());
    ASSERT_TRUE(page);
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return !page->BuildMap().items.empty();
    }));
  }

  // Says |text| and waits until the browser has done it and said all it
  // says (a reading to its end included).
  void Say(std::string_view text) {
    VoiceController* controller = voice();
    ASSERT_TRUE(controller);
    controller->HandleUtterance(std::string(text));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return !controller->busy() &&
             controller->activity() != VoiceActivity::kSpeaking;
    })) << "still busy after \"" << text << "\"";
  }

  content::EvalJsResult Js(std::string_view script) {
    return content::EvalJs(web_contents(), script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE);
  }

  FakeTtsPlatform tts_;

 private:
  base::test::ScopedFeatureList features_;
  std::unique_ptr<ScopedSkillHostForTesting> skill_host_;
  std::unique_ptr<ScopedTestSite> test_site_;
  std::string home_;
  std::string search_;
  std::string name_;
  std::string folded_name_;
  std::string host_;
};

IN_PROC_BROWSER_TEST_F(VoiceSkillsBrowserTest, SearchYouTubeThroughItsSkill) {
  BindSkill(kYouTubeHost, "youtube", "YouTube", "youtube_home.html",
            "youtube_results.html?search_query=");
  Open(Url(kYouTubeHost, "youtube_home.html"));
  // The skill names the site's search box and its button.
  const PageMap home =
      VoicePageController::FromWebContents(web_contents())->BuildMap();
  ASSERT_TRUE(home.site.search_field.valid());
  ASSERT_TRUE(home.site.search_submit.valid());
  ASSERT_TRUE(home.site.results);
  tts_.TakeSpoken();

  // "Search YouTube for ...", on YouTube: its own box, its own button.
  Say("search YouTube for bohemian rhapsody");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().path(),
            "/molt_voice/skills/youtube_results.html");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(),
            "search_query=bohemian+rhapsody");
  const std::string said = tts_.TakeSpoken();
  EXPECT_EQ(said.find("10 results."), 0u) << said;
  EXPECT_NE(said.find("1: Queen \xE2\x80\x93 Bohemian Rhapsody (Official "
                      "Video Remastered), Queen Official, 6 minutes."),
            std::string::npos)
      << said;

  // "Play the fourth one": the fourth video's page.
  Say("play the fourth one");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().path(),
            "/molt_voice/skills/youtube_watch.html");
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=v4");
}

IN_PROC_BROWSER_TEST_F(VoiceSkillsBrowserTest, SkipTheAdAndCaptions) {
  BindSkill(kYouTubeHost, "youtube", "YouTube", "youtube_home.html",
            "youtube_results.html?search_query=");
  Open(Url(kYouTubeHost, "youtube_watch.html?v=v1"));
  Say("skip the ad");
  EXPECT_EQ(true, Js("window.skipped"));
  EXPECT_EQ(tts_.TakeSpoken(), "Skipped the ad.");
  Say("turn on captions");
  EXPECT_EQ("true", Js("document.querySelector('.ytp-subtitles-button')"
                       ".getAttribute('aria-pressed')"));
  // Once the ad is gone, there's nothing to skip.
  VoicePageController* page =
      VoicePageController::FromWebContents(web_contents());
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return !page->BuildMap().site.skip_ad.valid(); }));
  tts_.TakeSpoken();
  Say("skip the ad");
  EXPECT_EQ(tts_.TakeSpoken().find("There's no skip button now."), 0u);
}

IN_PROC_BROWSER_TEST_F(VoiceSkillsBrowserTest, ReadThePage) {
  BindSkill(kWikipediaHost, "wikipedia", "Wikipedia", "wikipedia_article.html",
            "wikipedia_search.html?search=");
  Open(Url(kWikipediaHost, "wikipedia_article.html"));
  tts_.TakeSpoken();
  Say("read the page");
  const std::string said = tts_.TakeSpoken();
  // The article's text, in order, without what the skill leaves out.
  const size_t lead = said.find(
      "Queen stands in for the article's subject here. This first sentence "
      "stands in for the lead.");
  const size_t history = said.find("History.");
  const size_t legacy = said.find("The legacy section is the last one read.");
  ASSERT_NE(lead, std::string::npos) << said;
  ASSERT_NE(history, std::string::npos) << said;
  ASSERT_NE(legacy, std::string::npos) << said;
  EXPECT_LT(lead, history);
  EXPECT_LT(history, legacy);
  EXPECT_EQ(said.find("edit"), std::string::npos) << said;
  EXPECT_EQ(said.find("Search Wikipedia"), std::string::npos) << said;
  EXPECT_EQ(said.find("stand-in for the infobox"), std::string::npos) << said;
  EXPECT_EQ(said.find("Main page"), std::string::npos) << said;
  EXPECT_NE(said.find("That's the end of the page."), std::string::npos);
}

IN_PROC_BROWSER_TEST_F(VoiceSkillsBrowserTest, HeadingsSectionsSelection) {
  BindSkill(kWikipediaHost, "wikipedia", "Wikipedia", "wikipedia_article.html",
            "wikipedia_search.html?search=");
  Open(Url(kWikipediaHost, "wikipedia_article.html"));
  tts_.TakeSpoken();
  Say("next heading");
  EXPECT_EQ(tts_.TakeSpoken(), "Heading level 1: Queen (band).");
  Say("next heading");
  EXPECT_EQ(tts_.TakeSpoken(), "Heading level 2: History.");
  Say("next paragraph");
  EXPECT_EQ(tts_.TakeSpoken(),
            "The history section starts with this sentence. It has a second "
            "sentence too.");
  Say("read the section on legacy");
  EXPECT_EQ(tts_.TakeSpoken(),
            "Legacy.|The legacy section is the last one read. Its text ends "
            "the article.|That's the end of the section.");

  // The selection, once the page reports it.
  ASSERT_EQ(true, Js("const p = [...document.querySelectorAll('p')].find("
                     "  e => e.textContent.startsWith('The style'));"
                     "getSelection().selectAllChildren(p); true"));
  VoicePageController* page =
      VoicePageController::FromWebContents(web_contents());
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return page->BuildTree(VoicePageController::kMaxReadingStringBytes)
        .selection.valid();
  }));
  Say("read the selection");
  EXPECT_EQ(tts_.TakeSpoken(),
            "The style section has one paragraph. It has two sentences.");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
