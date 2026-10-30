// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The web agent on the accessibility page map, and voice mode's planner
// fallback (docs/design/AGENT_ACTIONS.md §2.10,
// docs/design/TOUCHLESS_BROWSING.md §6.2), on the fixture pages in
// chrome/test/data/molt_voice/, with a scripted model in place of the
// on-device one (WebAgent::SetModelForTesting()):
//   - the agent searches the fixture video site, chooses the Live Aid video
//     from the results and plays it, all through accessibility actions,
//     and the video plays with sound;
//   - on a page whose accessibility tree has nothing to act on, it falls
//     back to the element snapshot;
//   - in a MoltNet window it never takes the accessibility mode;
//   - a frame of the page's origin that another site's frame embeds isn't
//     listed;
//   - voice mode hands what the grammar can't do to the planner (a stub
//     here) and speaks its steps; its question waits for "yes", "stop"
//     ends it;
//   - through the web agent, placing an order waits for a spoken "yes";
//   - the planner's steps remembered as a routine (planner-routines,
//     TOUCHLESS_BROWSING.md §6.8): saved as a draft naming its sites,
//     confirmed before its first run, replayed step by step without the
//     model, stopped at a step whose element isn't on the page, and made a
//     draft again when a step in its file uses another site.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/molt_ai/agents/agent_features.h"
#include "chrome/browser/molt_ai/agents/web_agent.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_learning.h"
#include "chrome/browser/molt_ai/voice/voice_page_controller.h"
#include "chrome/browser/molt_ai/voice/voice_planner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
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
namespace {

// A TTS platform that records what it is asked to say and finishes each
// utterance at once (as voice_controller_browsertest.cc's).
class FakeTtsPlatform : public content::TtsPlatform {
 public:
  FakeTtsPlatform() = default;
  FakeTtsPlatform(const FakeTtsPlatform&) = delete;
  FakeTtsPlatform& operator=(const FakeTtsPlatform&) = delete;
  ~FakeTtsPlatform() = default;

  // Everything said since TakeSpoken() last took it, joined by "|".
  std::string Untaken() const {
    std::string out;
    for (size_t i = taken_; i < spoken_.size(); ++i) {
      out += (out.empty() ? "" : "|") + spoken_[i];
    }
    return out;
  }

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

// The current page of a prompt, and its ELEMENTS lines.
std::string CurrentPage(const std::string& prompt) {
  const size_t at = prompt.rfind("CURRENT PAGE:");
  return at == std::string::npos ? std::string() : prompt.substr(at);
}

// The id of the ELEMENTS line of the current page that has |needle|, or "".
std::string ElementWith(const std::string& prompt, std::string_view needle) {
  const std::string page = CurrentPage(prompt);
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

std::string Click(const std::string& id) {
  return "{\"reason\":\"\",\"action\":\"CLICK\",\"target\":\"" + id + "\"}";
}

std::string Done(const std::string& result) {
  return "{\"reason\":\"\",\"action\":\"DONE\",\"result\":\"" + result + "\"}";
}

class AgentPageMapBrowserTest : public InProcessBrowserTest {
 public:
  AgentPageMapBrowserTest() {
    // One action at a time (no plan), so the scripted model's replies need
    // no "step".
    features_.InitWithFeatures(
        {agent_features::kMoltAgentPageMap, voice::kMoltVoiceMode},
        {agent_features::kMoltAgentPlanner});
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // Unmuted media may start only after user activation: the video shows
    // the accessibility click gave the page activation.
    command_line->AppendSwitchASCII(
        switches::kAutoplayPolicy,
        switches::autoplay::kDocumentUserActivationRequiredPolicy);
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
      voice->Stop();
    }
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  content::WebContents* web_contents() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }

  GURL Url(std::string_view host, std::string_view path) {
    return embedded_test_server()->GetURL(host, path);
  }

  content::EvalJsResult Js(std::string_view script) {
    return content::EvalJs(web_contents(), script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE);
  }

  // Runs the agent on |contents| to its end; every question is allowed.
  void RunAgent(content::WebContents* contents, const std::string& goal) {
    done_ = false;
    agent_ =
        std::make_unique<WebAgent>(contents, &BrowserAIRuntime::GetInstance());
    agent_->Start(
        goal,
        base::BindRepeating(
            [](std::vector<WebAgent::Step>* steps, const WebAgent::Step& step) {
              steps->push_back(step);
            },
            &steps_),
        base::BindOnce(
            [](AgentPageMapBrowserTest* self, bool success,
               std::string result) {
              self->done_ = true;
              self->success_ = success;
              self->result_ = std::move(result);
            },
            base::Unretained(this)),
        base::BindRepeating(
            [](AgentPageMapBrowserTest* self,
               const WebAgent::ConfirmRequest& request) {
              self->questions_.push_back(request.action + ": " +
                                         request.message);
              base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                  FROM_HERE, base::BindOnce(
                                 [](AgentPageMapBrowserTest* self, int id) {
                                   if (self->agent_) {
                                     self->agent_->ResolveConfirmation(id,
                                                                       true);
                                   }
                                 },
                                 base::Unretained(self), request.id));
            },
            base::Unretained(this)),
        WebAgent::PlanCallback());
    ASSERT_TRUE(base::test::RunUntil([&]() { return done_; }));
  }

  FakeTtsPlatform tts_;
  std::unique_ptr<WebAgent> agent_;
  std::vector<WebAgent::Step> steps_;
  std::vector<std::string> questions_;
  bool done_ = false;
  bool success_ = false;
  std::string result_;

 private:
  base::test::ScopedFeatureList features_;
};

// Search, choose and play, through accessibility actions only.
IN_PROC_BROWSER_TEST_F(AgentPageMapBrowserTest, SearchesChoosesAndPlays) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  int typed = 0;
  bool clicked_video = false;
  WebAgent::SetModelForTesting(base::BindRepeating(
      [](int* typed, bool* clicked_video, const std::string& prompt,
         const PromptOptions& options) {
        const std::string page = CurrentPage(prompt);
        // Observed through the page map.
        EXPECT_NE(page.find("SOURCE: the page's accessibility tree"),
                  std::string::npos);
        if (page.find("video_home.html") != std::string::npos) {
          if ((*typed)++ == 0) {
            const std::string box = ElementWith(prompt, "(text field)");
            return Reply("{\"reason\":\"\",\"action\":\"TYPE\",\"target\":\"" +
                         box + "\",\"text\":\"Bohemian Rhapsody\"}");
          }
          return Reply(Click(ElementWith(prompt, "button \"Search\"")));
        }
        if (page.find("video_results.html") != std::string::npos) {
          return Reply(Click(ElementWith(prompt, "Live Aid 1985")));
        }
        if (page.find("watch.html") != std::string::npos && !*clicked_video) {
          *clicked_video = true;
          return Reply(Click(ElementWith(prompt, "media")));
        }
        return Reply(Done("Playing it"));
      },
      &typed, &clicked_video));

  RunAgent(web_contents(), "play the live aid version of bohemian rhapsody");
  EXPECT_TRUE(success_) << result_;
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");
  // The video plays with sound, after a trusted click with activation.
  EXPECT_EQ("playing", Js("whenPlaying()"));
  EXPECT_EQ(true, Js("clickInfo.trusted && clickInfo.active"));
  EXPECT_EQ(true, Js("const p = document.getElementById('player'); "
                     "!p.paused && !p.muted && p.volume > 0"));
  // Every observation came from the page map; every step acted through it.
  EXPECT_GE(agent_->observation_counts().page_map, 4);
  EXPECT_EQ(agent_->observation_counts().element_snapshot, 0);
  for (const WebAgent::Step& step : steps_) {
    EXPECT_EQ(step.observed_with, WebAgent::ObservationSource::kPageMap);
  }
  // The search button submits the site's search form: the agent asked.
  ASSERT_EQ(questions_.size(), 1u);
  EXPECT_EQ(questions_[0].rfind("CLICK: Click \"Search\"", 0), 0u)
      << questions_[0];
  // The run is over: the accessibility mode goes with it (no voice mode).
  agent_.reset();
  EXPECT_FALSE(web_contents()->GetAccessibilityMode().has_mode(
      voice::VoicePageController::AccessibilityMode().flags()));
}

// No accessibility tree to act on: the element snapshot.
IN_PROC_BROWSER_TEST_F(AgentPageMapBrowserTest, FallsBackToTheSnapshot) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("plain.test", "/molt_voice/no_tree.html")));
  bool clicked = false;
  WebAgent::SetModelForTesting(base::BindRepeating(
      [](bool* clicked, const std::string& prompt,
         const PromptOptions& options) {
        EXPECT_EQ(prompt.find("SOURCE: the page's accessibility tree"),
                  std::string::npos);
        if (!*clicked) {
          *clicked = true;
          return Reply(Click(ElementWith(prompt, "\"Go\"")));
        }
        return Reply(Done("Went"));
      },
      &clicked));
  RunAgent(web_contents(), "press go");
  EXPECT_TRUE(success_) << result_;
  EXPECT_EQ(1, Js("window.went"));
  EXPECT_EQ(agent_->observation_counts().page_map, 0);
  EXPECT_GE(agent_->observation_counts().element_snapshot, 1);
}

// A frame of the page's own origin is listed; one inside another site's
// frame (which chose which of the page's URLs it shows) is not.
IN_PROC_BROWSER_TEST_F(AgentPageMapBrowserTest,
                       LeavesOutWhatAnotherSitesFrameEmbeds) {
  const GURL widget = Url("a.test", "/molt_voice/frame_widget.html");
  auto host = [&](std::string_view site, const GURL& frame) {
    return Url(site, base::StrCat({"/molt_voice/frame_host.html?frame=",
                                   base::EscapeQueryParamValue(
                                       frame.spec(), /*use_plus=*/true)}));
  };
  std::string seen;
  WebAgent::SetModelForTesting(base::BindRepeating(
      [](std::string* seen, const std::string& prompt,
         const PromptOptions& options) {
        *seen = CurrentPage(prompt);
        return Reply(Done("Looked"));
      },
      &seen));

  // a.test embeds a.test's widget: its Like button is listed.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), host("a.test", widget)));
  RunAgent(web_contents(), "like the widget");
  EXPECT_GE(agent_->observation_counts().page_map, 1);
  EXPECT_NE(seen.find("\"After frame\""), std::string::npos) << seen;
  EXPECT_NE(seen.find("\"Like\""), std::string::npos) << seen;

  // a.test embeds b.test, which embeds the same widget of a.test's.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), host("a.test", host("b.test", widget))));
  RunAgent(web_contents(), "like the widget");
  EXPECT_GE(agent_->observation_counts().page_map, 1);
  EXPECT_NE(seen.find("\"After frame\""), std::string::npos) << seen;
  EXPECT_EQ(seen.find("\"Like\""), std::string::npos) << seen;
  EXPECT_EQ(seen.find("\"Share\""), std::string::npos) << seen;
}

// MoltNet: no accessibility mode, the element snapshot only.
IN_PROC_BROWSER_TEST_F(AgentPageMapBrowserTest, NeverInMoltNetWindows) {
  Profile* moltnet = GetProfile()->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUnique("MoltNet::Tor"),
      /*create_if_needed=*/true);
  BrowserWindowInterface* window = CreateBrowser(moltnet);
  ASSERT_TRUE(window);
  content::WebContents* tab =
      window->GetTabStripModel()->GetActiveWebContents();
  WebAgent::SetModelForTesting(base::BindRepeating(
      [](const std::string& prompt, const PromptOptions& options) {
        return Reply(Done("Nothing to do"));
      }));
  RunAgent(tab, "look at the page");
  EXPECT_EQ(agent_->observation_counts().page_map, 0);
  EXPECT_FALSE(voice::VoicePageController::FromWebContents(tab) &&
               voice::VoicePageController::FromWebContents(tab)->active());
  EXPECT_FALSE(tab->GetAccessibilityMode().has_mode(
      voice::VoicePageController::AccessibilityMode().flags()));
}

// ---------------------------------------------------------------------------
// Voice mode's planner fallback
// ---------------------------------------------------------------------------

// A planner that says what the test scripts and records what it is told.
class StubPlanner : public voice::VoicePlanner {
 public:
  struct Log {
    std::vector<std::string> started;
    int map_items = -1;
    std::vector<std::string> answers;
    int cancelled = 0;
  };

  explicit StubPlanner(Log* log) : log_(log) {}

  void Start(content::WebContents* contents,
             const std::string& heard,
             const voice::PageMap& map,
             Delegate* delegate) override {
    delegate_ = delegate;
    log_->started.push_back(heard);
    log_->map_items = static_cast<int>(map.items.size());
    delegate_->PlannerSays("I'll search the site for trains to Lyon.");
    delegate_->PlannerAsks(1, "open trains.example");
  }
  void Replay(content::WebContents* contents,
              const voice::PlannerStep& step,
              Delegate* delegate) override {
    delegate->PlannerReplayed(voice::PlannerReplay::kFailed);
    delegate->PlannerFinished();
  }
  void Answer(int question, bool yes) override {
    log_->answers.push_back(base::NumberToString(question) +
                            (yes ? " yes" : " no"));
    Delegate* delegate = delegate_;
    delegate_ = nullptr;
    delegate->PlannerSays(yes ? "Done. The 7:04 train." : "");
    delegate->PlannerFinished();
  }
  void Cancel() override {
    ++log_->cancelled;
    delegate_ = nullptr;
  }

 private:
  raw_ptr<Log> log_;
  raw_ptr<Delegate> delegate_ = nullptr;
};

class VoicePlannerBrowserTest : public AgentPageMapBrowserTest {
 protected:
  voice::VoiceController* voice() {
    return voice::VoiceController::GetOrCreateForBrowser(browser());
  }

  // Says |text| and waits until the browser has done it and said all it
  // says, or waits for an answer.
  void Say(std::string_view text) {
    voice::VoiceController* controller = voice();
    ASSERT_TRUE(controller);
    controller->HandleUtterance(std::string(text));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return !controller->busy() &&
             controller->activity() != voice::VoiceActivity::kSpeaking;
    })) << "still busy after \""
        << text << "\"";
  }

  // Waits until the browser has said |text| since TakeSpoken() last took
  // what it said.
  bool Heard(std::string_view text) {
    return base::test::RunUntil([&]() {
      return tts_.Untaken().find(std::string(text)) != std::string::npos;
    });
  }
};

// Routines with the planner's steps: the profile's learning keeps them in
// a temporary automations directory.
class VoicePlannerRoutinesBrowserTest : public VoicePlannerBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    VoicePlannerBrowserTest::SetUpOnMainThread();
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      ASSERT_TRUE(automations_.CreateUniqueTempDir());
    }
    learning()->SetAutomationsRootForTesting(automations_.GetPath());
    ASSERT_TRUE(base::test::RunUntil([&]() { return learning()->loaded(); }));
  }

  voice::VoiceLearning* learning() {
    return voice::VoiceLearning::Get(browser()->GetProfile());
  }

  // The routine as stored, changed by |change| and saved again (as an
  // edit of its file, or the Agent studio's click on "Confirm these
  // sites").
  template <typename Change>
  void ChangeStored(const std::string& id, Change change) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    automation::AutomationStorage storage;
    storage.SetRootForTesting(automations_.GetPath());
    std::optional<automation::Script> script = storage.Load(id);
    ASSERT_TRUE(script);
    change(*script);
    ASSERT_TRUE(storage.Save(*script));
  }

  std::optional<automation::Script> Stored(const std::string& id) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    automation::AutomationStorage storage;
    storage.SetRootForTesting(automations_.GetPath());
    return storage.Load(id);
  }

  // The scripted model: on the video site, types the search, presses the
  // search button, chooses the Live Aid video; then it's done.
  static void ScriptTheVideoSearch() {
    WebAgent::SetModelForTesting(base::BindRepeating(
        [](const std::string& prompt, const PromptOptions& options) {
          const std::string page = CurrentPage(prompt);
          if (page.find("video_home.html") != std::string::npos) {
            if (prompt.find("[TYPE]") == std::string::npos) {
              return Reply(
                  "{\"reason\":\"\",\"action\":\"TYPE\",\"target\":\"" +
                  ElementWith(prompt, "(text field)") +
                  "\",\"text\":\"Bohemian Rhapsody\"}");
            }
            return Reply(Click(ElementWith(prompt, "button \"Search\"")));
          }
          if (page.find("video_results.html") != std::string::npos) {
            return Reply(Click(ElementWith(prompt, "Live Aid 1985")));
          }
          return Reply(Done("Playing it"));
        }));
  }

  base::ScopedTempDir automations_;
};

IN_PROC_BROWSER_TEST_F(VoicePlannerBrowserTest, HandsTheRestToThePlanner) {
  StubPlanner::Log log;
  ASSERT_TRUE(voice()->Start());
  voice()->SetPlannerForTesting(std::make_unique<StubPlanner>(&log));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    voice::VoicePageController* page =
        voice::VoicePageController::FromWebContents(web_contents());
    return page && page->BuildMap().items.size() >= 3;
  }));
  tts_.TakeSpoken();

  // The utterance and the page map go to the planner; its step is said,
  // then its question, which waits for a "yes".
  Say("find me a train from Paris to Lyon tomorrow");
  ASSERT_EQ(log.started.size(), 1u);
  EXPECT_EQ(log.started[0], "find me a train from Paris to Lyon tomorrow");
  EXPECT_GE(log.map_items, 3);
  EXPECT_EQ(tts_.TakeSpoken(),
            "I'll search the site for trains to Lyon.|Say yes to open "
            "trains.example, or no to cancel.");
  EXPECT_TRUE(voice()->planner_running());
  Say("yes");
  EXPECT_EQ(log.answers, (std::vector<std::string>{"1 yes"}));
  EXPECT_EQ(tts_.TakeSpoken(), "Done. The 7:04 train.");
  EXPECT_FALSE(voice()->planner_running());

  // "No" answers no.
  Say("find me a train from Paris to Lyon tomorrow");
  Say("no");
  EXPECT_EQ(log.answers.back(), "1 no");
  EXPECT_FALSE(voice()->planner_running());
  tts_.TakeSpoken();

  // "Stop" (or anything else) ends the run: no answer goes through.
  Say("find me a train from Paris to Lyon tomorrow");
  const int cancelled = log.cancelled;
  Say("stop");
  EXPECT_EQ(log.cancelled, cancelled + 1);
  EXPECT_FALSE(voice()->planner_running());
  Say("yes");
  EXPECT_EQ(log.answers.size(), 2u);

  // Without a planner the browser says it can't do that yet.
  voice()->SetPlannerForTesting(nullptr);
  tts_.TakeSpoken();
  Say("find me a train from Paris to Lyon tomorrow");
  EXPECT_EQ(tts_.TakeSpoken(),
            "I can't do that yet: \"find me a train from Paris to Lyon "
            "tomorrow\". Say what can I say to hear what I can do.");
}

// Through the web agent: placing the order waits for a spoken "yes".
IN_PROC_BROWSER_TEST_F(VoicePlannerBrowserTest, AnOrderWaitsForAYes) {
  ASSERT_TRUE(voice()->Start());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("shop.test", "/molt_voice/checkout.html")));
  WebAgent::SetModelForTesting(base::BindRepeating(
      [](const std::string& prompt, const PromptOptions& options) {
        // Asked once (the run stops on "no"), or answered "yes".
        if (prompt.find("[CLICK]") == std::string::npos) {
          return Reply(Click(ElementWith(prompt, "button \"Place order\"")));
        }
        return Reply(Done("Your order is placed"));
      }));
  const std::string site =
      "shop.test:" + base::NumberToString(embedded_test_server()->port());
  tts_.TakeSpoken();

  // "No": nothing is ordered.
  Say("order the socks for me");
  EXPECT_EQ(tts_.TakeSpoken(),
            "Working on it.|Pressing Place order.|Say yes to click Place "
            "order, it submits a form on " +
                site + ", or no to cancel.");
  EXPECT_EQ(0, Js("window.ordered"));
  Say("no");
  EXPECT_EQ(tts_.TakeSpoken(), "Okay, I won't.");
  EXPECT_EQ(0, Js("window.ordered"));
  EXPECT_FALSE(voice()->planner_running());

  // "Yes": ordered, as a trusted click, and the result said.
  Say("order the socks for me");
  tts_.TakeSpoken();
  EXPECT_EQ(0, Js("window.ordered"));
  Say("yes");
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return !voice()->planner_running() && !voice()->busy(); }));
  EXPECT_EQ(1, Js("window.ordered"));
  EXPECT_EQ(true, Js("window.orderTrusted"));
  EXPECT_EQ(tts_.TakeSpoken(), "Done. Your order is placed.");
}

// The planner's steps, remembered as a routine: saved as a draft that names
// its sites, confirmed before its first run, then replayed step by step
// without the model, its question asked again.
IN_PROC_BROWSER_TEST_F(VoicePlannerRoutinesBrowserTest,
                       RemembersThePlannersStepsAndReplaysThem) {
  ASSERT_TRUE(voice()->Start());
  const GURL home = Url("video.test", "/molt_voice/video_home.html");
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), home));
  ScriptTheVideoSearch();
  const std::string site =
      "video.test:" + base::NumberToString(embedded_test_server()->port());

  // The request: the planner types, asks before the search form, chooses.
  Say("play the live aid version of bohemian rhapsody");
  ASSERT_TRUE(Heard("Say yes to click Search, it submits a form on " + site +
                    ", or no to cancel."));
  Say("yes");
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return !voice()->planner_running() && !voice()->busy(); }));
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");

  // "Remember this": the planner's steps, read back; saved as a draft
  // that names its site.
  tts_.TakeSpoken();
  Say("remember this as live aid");
  EXPECT_TRUE(
      Heard("I'll remember: type Bohemian Rhapsody in Search, click "
            "Search and click Queen - Bohemian Rhapsody (Live Aid "));
  EXPECT_TRUE(
      Heard("Some of these steps are the AI's, so you'll confirm the "
            "sites they use before it runs."));
  Say("yes");
  EXPECT_TRUE(
      Heard("Saved live aid. It uses video.test. Its sites need your "
            "confirmation the first time you run it."));
  ASSERT_EQ(learning()->routines().size(), 1u);
  const std::string id = learning()->routines()[0].id;
  EXPECT_TRUE(learning()->routines()[0].needs_sites_confirmed);
  {
    std::optional<automation::Script> script = Stored(id);
    ASSERT_TRUE(script);
    EXPECT_TRUE(automation::HasVoiceSteps(*script));
    // It starts on the page the request was made on.
    ASSERT_EQ(script->steps.size(), 4u);
    EXPECT_EQ(script->steps[0].type, automation::StepType::NAVIGATE);
    EXPECT_EQ(script->steps[0].target, home.spec());
    EXPECT_EQ(script->steps[1].type, automation::StepType::TYPE);
    EXPECT_EQ(script->steps[2].type, automation::StepType::CLICK);
    EXPECT_EQ(script->steps[3].type, automation::StepType::CLICK);
    // A draft's rules: Casual, no site until confirmed.
    EXPECT_EQ(script->security.trust, automation::TrustLevel::CASUAL);
    EXPECT_TRUE(script->security.sites_unconfirmed);
    EXPECT_TRUE(script->security.domain_whitelist.empty());
  }

  // Its first run asks to confirm its sites; this window doesn't listen,
  // so the Agent studio confirms them.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  tts_.TakeSpoken();
  Say("run live aid");
  EXPECT_TRUE(
      Heard("live aid has steps the AI planned, so the sites it uses "
            "need your confirmation. Confirm it in the Agent studio "
            "for now."));
  EXPECT_EQ(web_contents()->GetLastCommittedURL(), GURL("about:blank"));
  // The studio's "Confirm these sites", on exactly the sites its steps use.
  ChangeStored(id, [](automation::Script& script) {
    EXPECT_FALSE(automation::AutomationSecurity::ConfirmDraftSites(
        script, {"video.test", "other.test"}));
    EXPECT_TRUE(automation::AutomationSecurity::ConfirmDraftSites(
        script, {"video.test"}));
  });

  // Replayed without the model: each step said as it starts, the search
  // form's question asked again.
  WebAgent::SetModelForTesting(base::BindRepeating(
      [](const std::string& prompt, const PromptOptions& options) {
        ADD_FAILURE() << "a routine's step asked the model";
        return Reply(Done("no"));
      }));
  tts_.TakeSpoken();
  Say("run live aid");
  EXPECT_TRUE(Heard("Running live aid. Step 1: open video.test."));
  EXPECT_TRUE(Heard("Step 2: type Bohemian Rhapsody in Search."));
  EXPECT_TRUE(Heard("Step 3: click Search."));
  ASSERT_TRUE(Heard("Say yes to click Search, it submits a form on " + site +
                    ", or no to cancel."));
  Say("yes");
  EXPECT_TRUE(Heard("Step 4: click Queen - Bohemian Rhapsody (Live Aid "));
  EXPECT_TRUE(Heard("live aid is done."));
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");
}

// A routine's planner step whose element isn't on the page stops the
// routine there; a step in its file that uses another site makes it a
// draft again.
IN_PROC_BROWSER_TEST_F(VoicePlannerRoutinesBrowserTest,
                       StopsWhereAStepCantBeFoundAndConfirmsNewSites) {
  ASSERT_TRUE(voice()->Start());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  ScriptTheVideoSearch();
  Say("play the live aid version of bohemian rhapsody");
  ASSERT_TRUE(Heard("Say yes to click Search"));
  Say("yes");
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return !voice()->planner_running() && !voice()->busy(); }));
  tts_.TakeSpoken();
  Say("remember this as live aid");
  Say("yes");
  ASSERT_TRUE(Heard("Saved live aid."));
  ASSERT_EQ(learning()->routines().size(), 1u);
  const std::string id = learning()->routines()[0].id;
  // Confirmed; then its last step renamed in the file (the same site).
  ChangeStored(id, [](automation::Script& script) {
    ASSERT_TRUE(automation::AutomationSecurity::ConfirmDraftSites(
        script, {"video.test"}));
    script.steps[3]
        .extra.GetDict()
        .FindDict(automation::kVoiceStepKey)
        ->Set("label", "Queen - Not On This Page");
  });

  tts_.TakeSpoken();
  Say("run live aid");
  ASSERT_TRUE(Heard("Say yes to click Search"));
  Say("yes");
  EXPECT_TRUE(
      Heard("I couldn't find Queen - Not On This Page on this page. "
            "I stopped live aid at step 4."));
  EXPECT_NE(web_contents()->GetLastCommittedURL().query(), "v=video4");

  // A step added in its file on another site: its sites need confirming
  // again, and the file is a draft again.
  ChangeStored(id, [](automation::Script& script) {
    automation::Step added;
    added.type = automation::StepType::CLICK;
    base::DictValue voice;
    voice.Set("agent", "click");
    voice.Set("kind", "button");
    voice.Set("label", "Pay");
    voice.Set("planned", true);
    base::DictValue extra;
    extra.Set(automation::kVoiceStepKey, std::move(voice));
    extra.Set(automation::kStepHostKey, "pay.test");
    added.extra = base::Value(std::move(extra));
    script.steps.push_back(std::move(added));
  });
  tts_.TakeSpoken();
  Say("run live aid");
  EXPECT_TRUE(
      Heard("live aid has steps the AI planned, so the sites it uses "
            "need your confirmation."));
  std::optional<automation::Script> script = Stored(id);
  ASSERT_TRUE(script);
  EXPECT_TRUE(script->security.sites_unconfirmed);
  EXPECT_TRUE(script->security.domain_whitelist.empty());
}

// While "Learn from what I say" is off, the planner's steps aren't kept.
IN_PROC_BROWSER_TEST_F(VoicePlannerRoutinesBrowserTest,
                       KeepsNothingOfThePlannerWhileLearningIsOff) {
  learning()->SetEnabled(false);
  ASSERT_TRUE(voice()->Start());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  ScriptTheVideoSearch();
  Say("play the live aid version of bohemian rhapsody");
  ASSERT_TRUE(Heard("Say yes to click Search"));
  Say("yes");
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return !voice()->planner_running() && !voice()->busy(); }));
  tts_.TakeSpoken();
  Say("remember this as live aid");
  EXPECT_TRUE(
      Heard("I can't remember this: the AI's steps aren't kept while "
            "Learn from what I say is off."));
  EXPECT_TRUE(learning()->routines().empty());
}

}  // namespace
}  // namespace molt_ai
