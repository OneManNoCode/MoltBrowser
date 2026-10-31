// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode learning from use (docs/design/TOUCHLESS_BROWSING.md §6.8) in
// the browser:
//   - a routine taught by voice on the fixture video site
//     (chrome/test/data/molt_voice/), saved as a workflow after a spoken
//     "yes", listed, run again from another page (each step said, the
//     video playing with sound), and forgotten;
//   - a correction ("no, I said Queen" after "Green" was heard) changing
//     what is recognized next time, through the stand-in speech helper and
//     the real listening session;
//   - nothing learned, nor used, in a MoltNet window;
//   - routines that belong to the profile that taught them: two profiles,
//     each hearing, running and seeing in the Agent studio only its own; a
//     name in use replaced on "yes"; a routine from before routines
//     belonged to a profile kept by the first profile that says it is
//     theirs, and by no other after that (profile-routines).

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/path_service.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_learning.h"
#include "chrome/browser/molt_ai/voice/voice_listener.h"
#include "chrome/browser/molt_ai/voice/voice_routine_store.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "chrome/browser/molt_ai/voice/voice_session.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"
#include "chrome/browser/permissions/system/system_permission_settings.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/media_switches.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {
namespace {

// A TTS platform that records what it is asked to say and finishes each
// utterance at once.
class RecordingTtsPlatform : public content::TtsPlatform {
 public:
  std::string All() const {
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
};

class VoiceLearningBrowserTest : public InProcessBrowserTest {
 public:
  VoiceLearningBrowserTest() { features_.InitAndEnableFeature(kMoltVoiceMode); }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(
        switches::kAutoplayPolicy,
        switches::autoplay::kDocumentUserActivationRequiredPolicy);
    // The audio service's fake microphone, for the listening session.
    command_line->AppendSwitch(switches::kUseFakeDeviceForMediaStream);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      ASSERT_TRUE(automations_.CreateUniqueTempDir());
      ASSERT_TRUE(models_.CreateUniqueTempDir());
      base::FilePath exe_dir;
      ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
      VoiceListener::SetPathsForTesting(VoiceListener::Paths{
          exe_dir.AppendASCII("molt_fake_speech_helper"), models_.GetPath()});
    }
    // Routines go to a temporary automations directory.
    learning()->SetAutomationsRootForTesting(automations_.GetPath());
    ASSERT_TRUE(base::test::RunUntil([&]() { return learning()->loaded(); }));
    video_home_ = Url("video.test", "/molt_voice/video_home.html").spec();
    video_search_ =
        Url("video.test", "/molt_voice/video_results.html?search_query=")
            .spec();
    test_site_ = std::make_unique<ScopedTestSite>(
        KnownSite{"VideoSite", video_home_, video_search_, {"videosite"},
                  {"video.test"}});
  }

  void TearDownOnMainThread() override {
    if (VoiceSession* session = VoiceSession::FromBrowser(browser())) {
      session->Stop();
    }
    if (VoiceController* voice = VoiceController::FromBrowser(browser())) {
      voice->Stop();
    }
    VoiceListener::SetPathsForTesting(std::nullopt);
    test_site_.reset();
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  VoiceLearning* learning() {
    return VoiceLearning::Get(browser()->GetProfile());
  }
  content::WebContents* web_contents() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }
  VoiceController* voice(BrowserWindowInterface* window = nullptr) {
    return VoiceController::GetOrCreateForBrowser(window ? window : browser());
  }
  GURL Url(std::string_view host, std::string_view path) {
    return embedded_test_server()->GetURL(host, path);
  }

  void Say(std::string_view text, BrowserWindowInterface* window = nullptr) {
    VoiceController* controller = voice(window);
    ASSERT_TRUE(controller);
    controller->HandleUtterance(std::string(text));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return !controller->busy() &&
             controller->activity() != VoiceActivity::kSpeaking;
    })) << "still busy after \"" << text << "\"";
  }

  bool Spoke(std::string_view text) {
    return base::test::RunUntil([&]() {
      return tts_.All().find(std::string(text)) != std::string::npos;
    });
  }

  content::EvalJsResult Js(std::string_view script) {
    return content::EvalJs(web_contents(), script,
                           content::EXECUTE_SCRIPT_NO_USER_GESTURE);
  }

  // A second profile of the machine, with a window; the storage its
  // routines from before profiles are in is this test's.
  BrowserWindowInterface* SecondProfileWindow() {
    ProfileManager* manager = g_browser_process->profile_manager();
    Profile& other = profiles::testing::CreateProfileSync(
        manager, manager->GenerateNextProfileDirectoryPath());
    VoiceLearning* other_learning = VoiceLearning::Get(&other);
    EXPECT_TRUE(other_learning);
    other_learning->SetAutomationsRootForTesting(automations_.GetPath());
    EXPECT_TRUE(
        base::test::RunUntil([&]() { return other_learning->loaded(); }));
    return CreateBrowser(&other);
  }

  // A routine as round 41 and 42 saved it: in the storage every profile
  // shares, opening the fixture video site.
  void SaveOldRoutine(const std::string& id, const std::string& name) {
    ParseContext context;
    std::optional<RoutineStep> open = RoutineStepFor(
        ParseVoiceCommand("open VideoSite", context), "", 0, {}, false);
    ASSERT_TRUE(open);
    open->url = Url("video.test", "/molt_voice/video_home.html").spec();
    BuiltRoutine built = BuildRoutine(id, name, {*open}, 1'700'000'000);
    ASSERT_TRUE(built.script);
    base::ScopedAllowBlockingForTesting allow_blocking;
    automation::AutomationStorage shared;
    shared.SetRootForTesting(automations_.GetPath());
    ASSERT_TRUE(shared.Save(*built.script));
  }

  // What the Agent studio of |window| lists (chrome://molt-ai-agent/ in a
  // new tab of it): its voice routines' names, then after "/" the routines
  // from before profiles it offers.
  std::string Studio(BrowserWindowInterface* window) {
    EXPECT_TRUE(ui_test_utils::NavigateToURLWithDisposition(
        window, GURL("chrome://molt-ai-agent/"),
        WindowOpenDisposition::NEW_FOREGROUND_TAB,
        ui_test_utils::BROWSER_TEST_WAIT_FOR_LOAD_STOP));
    content::WebContents* tab =
        window->GetTabStripModel()->GetActiveWebContents();
    return content::EvalJs(
               tab,
               "sendWithPromise('listWorkflows').then(r => "
               "r.workflows.filter(w => w.voice).map(w => w.name).join('|') "
               "+ '/' + r.unclaimed.map(u => u.name).join('|'))")
        .ExtractString();
  }

  void Script(std::string_view script) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::WriteFile(
        models_.GetPath().AppendASCII("fake_script.txt"), script));
  }

  RecordingTtsPlatform tts_;
  base::ScopedTempDir automations_;
  base::ScopedTempDir models_;
  system_permission_settings::ScopedSettingsForTesting microphone_{
      ContentSettingsType::MEDIASTREAM_MIC, /*blocked=*/false};

 private:
  base::test::ScopedFeatureList features_;
  std::string video_home_;
  std::string video_search_;
  std::unique_ptr<ScopedTestSite> test_site_;
};

IN_PROC_BROWSER_TEST_F(VoiceLearningBrowserTest,
                       TeachesARoutineByVoiceAndRunsItAgain) {
  // Done once, by voice.
  Say("open VideoSite");
  Say("search for Bohemian Rhapsody");
  Say("play the fourth one");
  ASSERT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");
  EXPECT_EQ("playing", Js("whenPlaying()"));

  // "Remember this as my videos": read back, then saved on "yes".
  Say("remember this as my videos");
  EXPECT_TRUE(Spoke("I'll remember: open VideoSite, search for Bohemian "
                    "Rhapsody and play the fourth one."));
  EXPECT_TRUE(Spoke("Say yes to save it as my videos, or no to cancel."));
  Say("yes");
  EXPECT_TRUE(Spoke("Saved. Say run my videos to do it again."));
  ASSERT_EQ(learning()->routines().size(), 1u);
  EXPECT_EQ(learning()->routines()[0].name, "my videos");

  // In the profile's own storage, as a workflow the user's own steps make
  // Approved on its site; none in the storage every profile shares.
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    automation::AutomationStorage storage(
        VoiceRoutinesDir(browser()->GetProfile()->GetPath()));
    automation::AutomationStorage shared;
    shared.SetRootForTesting(automations_.GetPath());
    EXPECT_TRUE(shared.ListIds().empty());
    std::optional<automation::Script> script =
        storage.Load(learning()->routines()[0].id);
    ASSERT_TRUE(script);
    EXPECT_TRUE(automation::HasVoiceSteps(*script));
    EXPECT_EQ(script->steps.size(), 3u);
    EXPECT_EQ(script->security.trust, automation::TrustLevel::APPROVED);
    EXPECT_FALSE(script->security.sites_unconfirmed);
    EXPECT_EQ(script->security.domain_whitelist,
              std::vector<std::string>{"video.test"});
  }

  // Listed by voice.
  Say("what are my routines");
  EXPECT_TRUE(
      Spoke("You have one routine: my videos. Say run, then its name."));

  // Run again from another page: each step said, the video playing.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  Say("run my videos");
  EXPECT_TRUE(Spoke("Running my videos. Step 1: open VideoSite."));
  EXPECT_TRUE(Spoke("Step 2: search for Bohemian Rhapsody."));
  EXPECT_TRUE(Spoke("Step 3: play the fourth one."));
  EXPECT_TRUE(Spoke("my videos is done."));
  EXPECT_EQ(web_contents()->GetLastCommittedURL().query(), "v=video4");
  EXPECT_EQ("playing", Js("whenPlaying()"));
  EXPECT_EQ(true, Js("clickInfo.trusted && clickInfo.active"));

  // The name alone runs it too; "stop" ends it.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  voice()->HandleUtterance("my videos");
  EXPECT_TRUE(Spoke("Running my videos."));
  Say("stop");

  // Forgotten after a spoken "yes".
  Say("forget my videos");
  EXPECT_TRUE(Spoke("Say yes to forget the routine my videos, or no to "
                    "cancel."));
  Say("yes");
  EXPECT_TRUE(Spoke("I forgot my videos."));
  EXPECT_TRUE(learning()->routines().empty());
}

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

IN_PROC_BROWSER_TEST_F(VoiceLearningBrowserTest,
                       ACorrectionChangesWhatIsRecognisedNextTime) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  // The stand-in helper "mishears" Queen as Green; the user corrects it;
  // the next time it writes Green, the browser searches for Queen.
  Script(
      "ready\n"
      "300 wake\n"
      "500 speech_start\n"
      "1100 speech_end\n"
      "1200 final 0.9 Hey Molt, search for Green\n"
      "4000 wake\n"
      "4200 speech_start\n"
      "4800 speech_end\n"
      "4900 final 0.9 Hey Molt, no, I said Queen\n"
      "8000 wake\n"
      "8200 speech_start\n"
      "8800 speech_end\n"
      "8900 final 0.9 Hey Molt, search for Green\n");
  VoiceSession* session = VoiceSession::GetOrCreateForBrowser(browser());
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->Start());
  // The first search: as heard.
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return web_contents()->GetLastCommittedURL().query() ==
           "search_query=Green";
  }));
  // The correction: the same command with what was meant.
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return web_contents()->GetLastCommittedURL().query() ==
           "search_query=Queen";
  }));
  bool learned = false;
  for (const VocabularyEntry& entry : learning()->vocabulary().entries()) {
    learned = learned || (entry.heard == "green" && entry.phrase == "Queen");
  }
  EXPECT_TRUE(learned);
  // The recognizer is biased toward it now.
  const std::vector<std::string> hotwords =
      learning()->vocabulary().HotwordPhrases(kMaxPersonalHotwords);
  EXPECT_NE(std::find(hotwords.begin(), hotwords.end(), "Queen"),
            hotwords.end());
  // Next time "Green" is heard, it is Queen: the page reloads with it.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), Url("video.test", "/molt_voice/video_home.html")));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return web_contents()->GetLastCommittedURL().query() ==
           "search_query=Queen";
  }));
  // "Forget the word Queen" takes it back.
  Say("forget the word Queen");
  Say("yes");
  EXPECT_TRUE(Spoke("I forgot the word Queen."));
  EXPECT_TRUE(learning()->vocabulary().empty());
}

#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)

IN_PROC_BROWSER_TEST_F(VoiceLearningBrowserTest,
                       NothingIsLearnedInAMoltNetWindow) {
  Profile* moltnet = GetProfile()->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUnique("MoltNet::Tor"),
      /*create_if_needed=*/true);
  // No learning at all for it: nothing to learn into, nothing to use.
  EXPECT_FALSE(VoiceLearning::Get(moltnet));
  BrowserWindowInterface* window = CreateBrowser(moltnet);
  ASSERT_TRUE(window);
  Say("search for Green", window);
  Say("no, I said Queen", window);
  Say("remember this as secret things", window);
  Say("what are my routines", window);
  EXPECT_EQ(tts_.All().find("I'll remember"), std::string::npos);
  EXPECT_NE(tts_.All().find("Voice mode is off in MoltNet windows."),
            std::string::npos);
  // The regular profile learned nothing from it.
  EXPECT_TRUE(learning()->vocabulary().empty());
  EXPECT_TRUE(learning()->hints().empty());
  EXPECT_TRUE(learning()->routines().empty());
  // And what the regular profile learned isn't used there.
  Say("open VideoSite");
  Say("search for Queen");
  Say("search for Queen");
  Say("remember this as my queen", window);
  EXPECT_EQ(tts_.All().find("I'll remember"), std::string::npos);
}

IN_PROC_BROWSER_TEST_F(VoiceLearningBrowserTest,
                       RoutinesBelongToTheProfileThatTaughtThem) {
  Say("open VideoSite");
  Say("remember this as my videos");
  Say("yes");
  EXPECT_TRUE(Spoke("Saved. Say run my videos to do it again."));
  ASSERT_EQ(learning()->routines().size(), 1u);
  const std::string id = learning()->routines()[0].id;

  // Another profile of the machine neither hears, runs nor forgets it.
  BrowserWindowInterface* other = SecondProfileWindow();
  ASSERT_TRUE(other);
  VoiceLearning* other_learning = VoiceLearning::Get(other->GetProfile());
  EXPECT_TRUE(other_learning->routines().empty());
  EXPECT_TRUE(other_learning->unclaimed().empty());
  Say("run my videos", other);
  Say("forget my videos", other);
  EXPECT_FALSE(Spoke("Running my videos."));
  EXPECT_TRUE(Spoke("I don't have a routine called my videos."));
  ASSERT_EQ(learning()->routines().size(), 1u);

  // It teaches its own of the same name: no question, a routine of its
  // own.
  Say("open VideoSite", other);
  Say("search for Queen", other);
  Say("remember this as my videos", other);
  EXPECT_FALSE(Spoke("You already have a routine called my videos."));
  Say("yes", other);
  ASSERT_EQ(other_learning->routines().size(), 1u);
  EXPECT_EQ(other_learning->routines()[0].steps, 2);
  EXPECT_EQ(learning()->routines()[0].steps, 1);

  // Each Agent studio lists its own profile's.
  EXPECT_EQ(Studio(browser()), "my videos/");
  EXPECT_EQ(Studio(other), "my videos/");

  // Taught again here under the name in use: replaced on "yes", its id
  // kept.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  Say("open VideoSite");
  Say("search for Bohemian Rhapsody");
  Say("remember this as my videos");
  EXPECT_TRUE(Spoke("You already have a routine called my videos."));
  EXPECT_TRUE(Spoke("Say yes to replace it, or no to cancel."));
  Say("yes");
  EXPECT_TRUE(Spoke("Replaced my videos. Say run my videos to do it again."));
  ASSERT_EQ(learning()->routines().size(), 1u);
  EXPECT_EQ(learning()->routines()[0].id, id);
  EXPECT_EQ(learning()->routines()[0].steps, 2);
  // The other profile's is as it was.
  ASSERT_EQ(other_learning->routines().size(), 1u);
  EXPECT_NE(other_learning->routines()[0].id, std::string());

  // "Forget everything" here leaves the other profile's.
  Say("forget everything you learned");
  Say("yes");
  EXPECT_TRUE(Spoke("Done. I forgot everything I learned from you."));
  EXPECT_TRUE(learning()->routines().empty());
  other_learning->RefreshRoutines();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return other_learning->routines().size() == 1u; }));
}

IN_PROC_BROWSER_TEST_F(VoiceLearningBrowserTest,
                       ARoutineFromBeforeProfilesIsKeptOnce) {
  SaveOldRoutine("voice_1700000000_1", "my videos");
  learning()->RefreshRoutines();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return learning()->unclaimed().size() == 1u; }));
  BrowserWindowInterface* other = SecondProfileWindow();
  ASSERT_TRUE(other);
  VoiceLearning* other_learning = VoiceLearning::Get(other->GetProfile());
  ASSERT_EQ(other_learning->unclaimed().size(), 1u);
  // Offered in both Agent studios, neither profile's own.
  EXPECT_EQ(Studio(browser()), "/my videos");
  EXPECT_EQ(Studio(other), "/my videos");

  // Not run by its name before it is kept: the question is asked.
  Say("run my videos");
  EXPECT_FALSE(Spoke("Running my videos."));
  EXPECT_TRUE(
      Spoke("my videos was made before routines belonged to a profile."));
  EXPECT_TRUE(
      Spoke("One routine was made before routines belonged to a profile: my "
            "videos."));
  EXPECT_TRUE(Spoke("Say yes to keep it here, or no to cancel."));
  Say("yes");
  EXPECT_TRUE(Spoke("Kept it here."));
  ASSERT_EQ(learning()->routines().size(), 1u);
  EXPECT_EQ(learning()->routines()[0].id, "voice_1700000000_1");
  EXPECT_TRUE(learning()->unclaimed().empty());

  // Gone for the other profile: nothing to keep, nothing to run.
  other_learning->RefreshRoutines();
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return other_learning->unclaimed().empty(); }));
  EXPECT_TRUE(other_learning->routines().empty());
  EXPECT_EQ(Studio(other), "/");
  EXPECT_EQ(Studio(browser()), "my videos/");

  // It runs here now, as it was saved.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("about:blank")));
  Say("run my videos");
  EXPECT_TRUE(Spoke("Running my videos. Step 1: open VideoSite."));
  EXPECT_TRUE(Spoke("my videos is done."));

  // Another one, which the other profile says isn't theirs: not offered
  // there again, still offered here.
  SaveOldRoutine("voice_1700000000_2", "my music");
  other_learning->RefreshRoutines();
  learning()->RefreshRoutines();
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return other_learning->unclaimed().size() == 1u &&
           learning()->unclaimed().size() == 1u;
  }));
  Say("what are my routines", other);
  EXPECT_TRUE(
      Spoke("One routine was made before routines belonged to a profile: my "
            "music."));
  Say("no", other);
  EXPECT_TRUE(Spoke("Okay, I'll leave them for another profile."));
  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return other_learning->unclaimed().empty(); }));
  EXPECT_EQ(other_learning->declined_count(), 1u);
  EXPECT_EQ(learning()->unclaimed().size(), 1u);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
