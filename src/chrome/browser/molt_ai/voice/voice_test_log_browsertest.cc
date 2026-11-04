// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's test log (voice_test_log.h) in a real browser, with
// --molt-voice-test-log: what was typed, what it was understood as, what
// voice mode said and its activity, one JSON line each; never a
// confirmation word; nothing from a MoltNet window.

#include "chrome/browser/molt_ai/voice/voice_test_log.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/location.h"
#include "base/strings/string_split.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/tts_controller.h"
#include "content/public/browser/tts_platform.h"
#include "content/public/test/browser_test.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

// A TTS platform that finishes each utterance at once.
class InstantTtsPlatform : public content::TtsPlatform {
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
    voice.name = "Instant voice";
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

class VoiceTestLogBrowserTest : public InProcessBrowserTest {
 public:
  VoiceTestLogBrowserTest() { features_.InitAndEnableFeature(kMoltVoiceMode); }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    log_path_ = dir_.GetPath().AppendASCII("voice.jsonl");
    command_line->AppendSwitchPath(kVoiceTestLogSwitch, log_path_);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    content::TtsController::GetInstance()->SetTtsPlatform(&tts_);
    home_ = embedded_test_server()
                ->GetURL("video.test", "/molt_voice/video_home.html")
                .spec();
    search_ = embedded_test_server()
                  ->GetURL("video.test",
                           "/molt_voice/video_results.html?search_query=")
                  .spec();
    site_ = std::make_unique<ScopedTestSite>(
        KnownSite{"VideoSite", home_, search_, {"videosite"}, {"video.test"}});
  }

  void TearDownOnMainThread() override {
    if (VoiceController* voice = VoiceController::FromBrowser(browser())) {
      voice->Stop();
    }
    site_.reset();
    content::TtsController::GetInstance()->SetTtsPlatform(
        content::TtsPlatform::GetInstance());
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  void Say(std::string_view text, BrowserWindowInterface* window = nullptr) {
    VoiceController* controller =
        VoiceController::GetOrCreateForBrowser(window ? window : browser());
    ASSERT_TRUE(controller);
    controller->HandleUtterance(std::string(text));
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return !controller->busy() &&
             controller->activity() != VoiceActivity::kSpeaking;
    }));
  }

  std::string ReadLog() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string contents;
    base::ReadFileToString(log_path_, &contents);
    return contents;
  }

  // Waits until the log holds |needle|.
  bool WaitForLog(std::string_view needle) {
    return base::test::RunUntil(
        [&]() { return ReadLog().find(needle) != std::string::npos; });
  }

  // Every line of the log, parsed (a line that isn't a JSON object fails).
  std::vector<base::DictValue> Lines() {
    std::vector<base::DictValue> out;
    for (const std::string& line :
         base::SplitString(ReadLog(), "\n", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      std::optional<base::Value> value =
          base::JSONReader::Read(line, base::JSON_PARSE_RFC);
      EXPECT_TRUE(value && value->is_dict()) << line;
      if (value && value->is_dict()) {
        out.push_back(std::move(*value).TakeDict());
      }
    }
    return out;
  }

  base::FilePath log_path_;

 private:
  base::test::ScopedFeatureList features_;
  base::ScopedTempDir dir_;
  InstantTtsPlatform tts_;
  std::string home_;
  std::string search_;
  std::unique_ptr<ScopedTestSite> site_;
};

IN_PROC_BROWSER_TEST_F(VoiceTestLogBrowserTest, LogsWhatWasHeardAndSaid) {
  Say("Hey Molt, open VideoSite");
  ASSERT_TRUE(WaitForLog("VideoSite is open."));

  const std::vector<base::DictValue> lines = Lines();
  ASSERT_FALSE(lines.empty());
  // The window's first line lists the grammar's commands.
  const base::DictValue& start = lines.front();
  EXPECT_EQ(*start.FindString("e"), "start");
  EXPECT_EQ(start.FindInt("w"), 1);
  const base::ListValue* kinds = start.FindList("kinds");
  ASSERT_TRUE(kinds);
  bool open_site = false;
  for (const base::Value& kind : *kinds) {
    open_site = open_site || kind == base::Value("open_site");
  }
  EXPECT_TRUE(open_site);

  bool heard = false;
  bool said = false;
  bool activity = false;
  for (const base::DictValue& line : lines) {
    EXPECT_TRUE(line.FindDouble("t").has_value());
    const std::string* event = line.FindString("e");
    ASSERT_TRUE(event);
    if (*event == "heard") {
      heard = true;
      EXPECT_EQ(*line.FindString("text"), "Hey Molt, open VideoSite");
      EXPECT_EQ(*line.FindString("source"), "typed");
      EXPECT_EQ(line.FindBool("clean"), false);
      EXPECT_EQ(*line.FindString("intent"), "open_site");
    } else if (*event == "said") {
      said = said || *line.FindString("text") == "VideoSite is open.";
    } else if (*event == "activity") {
      activity = true;
    }
  }
  EXPECT_TRUE(heard);
  EXPECT_TRUE(said);
  EXPECT_TRUE(activity);
}

IN_PROC_BROWSER_TEST_F(VoiceTestLogBrowserTest, NeverLogsAConfirmationWord) {
  Say("harbor");
  Say("yes, say maple please");
  Say("open the lanterns");
  ASSERT_TRUE(WaitForLog("open the [word]"));
  const std::string log = ReadLog();
  EXPECT_EQ(log.find("harbor"), std::string::npos) << log;
  EXPECT_EQ(log.find("maple"), std::string::npos) << log;
  EXPECT_EQ(log.find("lantern"), std::string::npos) << log;
  EXPECT_NE(log.find("\"text\":\"[word]\""), std::string::npos) << log;
  EXPECT_NE(log.find("yes, say [word] please"), std::string::npos) << log;
}

IN_PROC_BROWSER_TEST_F(VoiceTestLogBrowserTest, NothingFromMoltNetWindows) {
  Profile* moltnet = GetProfile()->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUnique("MoltNet::Tor"),
      /*create_if_needed=*/true);
  BrowserWindowInterface* window = CreateBrowser(moltnet);
  ASSERT_TRUE(window);
  Say("open VideoSite from the onion window", window);
  // Then something in the regular window, to know the log is up to date.
  Say("scroll down");
  ASSERT_TRUE(WaitForLog("scroll down"));
  EXPECT_EQ(ReadLog().find("onion window"), std::string::npos);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
