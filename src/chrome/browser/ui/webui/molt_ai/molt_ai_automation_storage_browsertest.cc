// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt://ai-automation and the AI chat's saves answer their pages from the
// shared storage's sequences (automation_shared_tasks.h), never reading or
// writing a file on the UI thread (where blocking is disallowed in a
// browser test, so a file access there would fail a check); and every
// file they touch is in the test's own ~/.moltbrowser
// (ScopedMoltHomeDirForTesting), which the profile's first listing of its
// routines reads too, before any test code runs.

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/common/molt_home_dir.h"
#include "chrome/browser/molt_ai/common/molt_home_dir_test_util.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_learning.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace {

using automation::AutomationStorage;
using automation::Script;

class AutomationPageStorageBrowserTest : public InProcessBrowserTest {
 public:
  AutomationPageStorageBrowserTest() {
    // Before the browser starts: a routine from before routines belonged
    // to a profile, in the test's own shared storage. The profile's first
    // listing finds it there, never in the real ~/.moltbrowser.
    base::ScopedAllowBlockingForTesting allow_blocking;
    voice::ParseContext context;
    std::optional<voice::RoutineStep> open = voice::RoutineStepFor(
        voice::ParseVoiceCommand("open YouTube", context), "", 0, {}, false);
    EXPECT_TRUE(open);
    if (open) {
      open->url = "https://www.youtube.com/";
      voice::BuiltRoutine built =
          voice::BuildRoutine("voice_1", "my videos", {*open}, 1'700'000'000);
      EXPECT_TRUE(built.script);
      AutomationStorage shared;
      EXPECT_EQ(shared.directory(),
                molt_home_.path().AppendASCII("automations"));
      if (built.script) {
        EXPECT_TRUE(shared.Save(*built.script));
      }
    }
  }

 protected:
  content::WebContents* page() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }
  // The page's answer to |message|(|args|), as JSON.
  std::string Ask(const std::string& message, const std::string& args = "") {
    return content::EvalJs(page(), "sendWithPromise('" + message + "'" +
                                       (args.empty() ? "" : ", " + args) +
                                       ").then(r => JSON.stringify(r))")
        .ExtractString();
  }
  // Sends |message| and doesn't wait: window.answer is set when it comes.
  void Send(const std::string& message, const std::string& args = "") {
    ASSERT_TRUE(content::ExecJs(
        page(), "window.answer = null; sendWithPromise('" + message + "'" +
                    (args.empty() ? "" : ", " + args) +
                    ").then(r => { window.answer = JSON.stringify(r); });"));
  }
  bool Answered() {
    return content::EvalJs(page(), "window.answer !== null").ExtractBool();
  }
  // Holds |sequence| until the returned event is signaled.
  static std::unique_ptr<base::WaitableEvent> Hold(
      const scoped_refptr<base::SequencedTaskRunner>& sequence) {
    auto release = std::make_unique<base::WaitableEvent>();
    sequence->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WaitableEvent* event) {
              base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
              event->Wait();
            },
            release.get()));
    return release;
  }
  base::FilePath Automations() {
    return molt_home_.path().AppendASCII("automations");
  }

  // Declared first: in place before the browser starts.
  ScopedMoltHomeDirForTesting molt_home_;
};

IN_PROC_BROWSER_TEST_F(AutomationPageStorageBrowserTest,
                       TheFirstListingReadsTheTestsHome) {
  EXPECT_EQ(MoltHomeDir(), molt_home_.path());
  voice::VoiceLearning* learning =
      voice::VoiceLearning::Get(browser()->GetProfile());
  ASSERT_TRUE(learning);
  ASSERT_TRUE(base::test::RunUntil([&]() { return learning->loaded(); }));
  ASSERT_EQ(learning->unclaimed().size(), 1u);
  EXPECT_EQ(learning->unclaimed()[0].name, "my videos");
}

IN_PROC_BROWSER_TEST_F(AutomationPageStorageBrowserTest,
                       EveryMessageIsAnsweredOffTheUIThread) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://molt-ai-automation/")));
  // The routine taught by voice isn't listed here.
  EXPECT_EQ(Ask("listScripts"), "{\"scripts\":[]}");
  // Two blank workflows in one second: two ids.
  const std::string first = Ask("createBlankScript");
  const std::string second = Ask("createBlankScript");
  EXPECT_NE(first.find("\"success\":true"), std::string::npos) << first;
  EXPECT_NE(second.find("\"success\":true"), std::string::npos) << second;
  EXPECT_NE(first, second);
  EXPECT_NE(Ask("createSampleScript")
                .find("\"script_id\":"
                      "\"sample-flight-watch\""),
            std::string::npos);
  // A save keeps the stored trust (Casual), whatever the page says.
  EXPECT_EQ(Ask("saveScript",
                "{id: 'sample-flight-watch', name: 'Flights', steps: [], "
                "security: {trust: 'admin'}}"),
            "{\"success\":true}");
  const std::string got = Ask("getScript", "'sample-flight-watch'");
  EXPECT_NE(got.find("\"name\":\"Flights\""), std::string::npos) << got;
  EXPECT_NE(got.find("\"trust\":\"casual\""), std::string::npos) << got;
  // An import never replaces it.
  const std::string imported =
      Ask("importScript", "{id: 'sample-flight-watch', name: 'Again'}");
  EXPECT_NE(imported.find("sample-flight-watch-imported-"), std::string::npos)
      << imported;
  EXPECT_EQ(Ask("getScript", "'voice_1'"), "{\"found\":false}");
  EXPECT_EQ(Ask("deleteScript", "'voice_1'"), "{\"success\":false}");
  EXPECT_EQ(Ask("deleteScript", "'sample-flight-watch'"), "{\"success\":true}");
  EXPECT_NE(Ask("auditTail").find("imported"), std::string::npos);
  // Every file in the test's home.
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    EXPECT_TRUE(base::PathExists(Automations().AppendASCII("voice_1.molt")));
    EXPECT_FALSE(base::PathExists(
        Automations().AppendASCII("sample-flight-watch.molt")));
  }

  // A read waits for the changes posted before it (the change sequence is
  // held), and is answered once they ran.
  std::unique_ptr<base::WaitableEvent> release =
      Hold(AutomationStorage::SharedSequence());
  Send("listScripts");
  ASSERT_TRUE(content::ExecJs(page(), "new Promise(r => setTimeout(r, 300))"));
  EXPECT_FALSE(Answered());
  release->Signal();
  ASSERT_TRUE(base::test::RunUntil([&]() { return Answered(); }));
}

IN_PROC_BROWSER_TEST_F(AutomationPageStorageBrowserTest,
                       TheChatsSavesRunOnTheStoragesSequence) {
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), GURL("chrome://molt-ai-chat/")));
  // Two plans in one second: two workflows, none over the other.
  const std::string plan =
      "{name: 'Trains', steps: ['navigate https://trains.example/']}";
  const std::string one = Ask("savePlanScript", plan);
  const std::string two = Ask("savePlanScript", plan);
  EXPECT_NE(one.find("\"success\":true"), std::string::npos) << one;
  EXPECT_NE(two.find("\"success\":true"), std::string::npos) << two;
  EXPECT_NE(one, two);
  const std::string watcher =
      Ask("createWatcher",
          "{url: 'https://news.example/', selector: 'h1', "
          "interval_seconds: 600}");
  EXPECT_NE(watcher.find("\"success\":true"), std::string::npos) << watcher;
  base::ScopedAllowBlockingForTesting allow_blocking;
  AutomationStorage shared;
  EXPECT_EQ(shared.ListIds().size(), 4u);  // voice_1, two plans, a watcher
}

}  // namespace
}  // namespace molt_ai
