// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Agent studio (chrome://molt-ai-agent/) answers its page from the
// storages' sequences (molt_ai_agent_storage.h), never reading or writing
// a file on the UI thread (where blocking is disallowed in a browser test,
// so a file access there would fail a check): every storage message is
// sent through the page, and a message isn't answered while the sequence of
// the storage it reads is busy, then is once it is free; and an answer
// meant for a page that reloaded meanwhile never reaches the new one.

#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/common/molt_home_dir_test_util.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_learning.h"
#include "chrome/browser/molt_ai/voice/voice_routine_store.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace {

using automation::AutomationStorage;
using automation::Script;

Script Workflow(const std::string& id, const std::string& name) {
  Script script;
  script.id = id;
  script.name = name;
  script.created_at_unix = 1'800'000'000;
  automation::Step step;
  step.type = automation::StepType::NAVIGATE;
  step.target = "https://news.example/";
  script.steps.push_back(std::move(step));
  return script;
}

class AgentStudioStorageBrowserTest : public InProcessBrowserTest {
 protected:
  // MoltBrowser's own directory is the test's, from before the browser
  // starts: never the real ~/.moltbrowser.
  molt_ai::ScopedMoltHomeDirForTesting molt_home_;

 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      ASSERT_TRUE(automations_.CreateUniqueTempDir());
    }
    // The studio's shared storage is this test's (the automation service
    // isn't made in browser tests).
    learning()->SetAutomationsRootForTesting(automations_.GetPath());
    ASSERT_TRUE(base::test::RunUntil([&]() { return learning()->loaded(); }));
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      AutomationStorage shared(
          AutomationStorage::SharedDirectory(automations_.GetPath()));
      ASSERT_TRUE(shared.Save(Workflow("wf_1", "Prices")));
    }
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(),
                                             GURL("chrome://molt-ai-agent/")));
  }

  voice::VoiceLearning* learning() {
    return voice::VoiceLearning::Get(browser()->GetProfile());
  }
  content::WebContents* studio() {
    return browser()->GetTabStripModel()->GetActiveWebContents();
  }
  // The page's answer to |message|(|args|), as JSON.
  std::string Ask(const std::string& message, const std::string& args = "") {
    return content::EvalJs(studio(), "sendWithPromise('" + message + "'" +
                                         (args.empty() ? "" : ", " + args) +
                                         ").then(r => JSON.stringify(r))")
        .ExtractString();
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

  // Sends |message| and doesn't wait: window.answer is set when it comes.
  void Send(const std::string& message, const std::string& args = "") {
    ASSERT_TRUE(content::ExecJs(
        studio(), "window.answer = null; sendWithPromise('" + message + "'" +
                      (args.empty() ? "" : ", " + args) +
                      ").then(r => { window.answer = JSON.stringify(r); });",
        // Not waited for: the script's value is the reply's promise.
        content::EXECUTE_SCRIPT_NO_RESOLVE_PROMISES));
  }
  bool Answered() {
    return content::EvalJs(studio(), "window.answer !== null").ExtractBool();
  }

  base::ScopedTempDir automations_;
};

IN_PROC_BROWSER_TEST_F(AgentStudioStorageBrowserTest,
                       EveryStorageMessageIsAnsweredOffTheUIThread) {
  // Listed: the shared storage's workflow.
  EXPECT_NE(Ask("listWorkflows").find("\"name\":\"Prices\""),
            std::string::npos);
  EXPECT_NE(Ask("getWorkflow", "'wf_1'").find("\"name\":\"Prices\""),
            std::string::npos);
  EXPECT_EQ(Ask("getRuns", "'wf_1'"), "{\"id\":\"wf_1\",\"runs\":[]}");
  // Saved in the shared storage, Casual.
  const std::string saved =
      Ask("saveWorkflow",
          "{name: 'Mail', steps: [{type: 'navigate', target: "
          "'https://mail.example/'}]}");
  EXPECT_NE(saved.find("\"trust\":\"casual\""), std::string::npos) << saved;
  const std::string copied = Ask("duplicateWorkflow", "'wf_1', 'Prices 2'");
  EXPECT_NE(copied.find("\"name\":\"Prices 2\""), std::string::npos) << copied;
  EXPECT_EQ(Ask("deleteWorkflow", "'wf_1'"), "{\"ok\":true}");
  EXPECT_EQ(Ask("deleteWorkflow", "'wf_1'"), "{\"ok\":false}");
  // Without the automation service (as in browser tests), nothing runs or
  // is scheduled, as before.
  EXPECT_EQ(Ask("runWorkflow", "'wf_1', {}, 'auto'"), "{\"ok\":false}");
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    AutomationStorage shared(
        AutomationStorage::SharedDirectory(automations_.GetPath()));
    EXPECT_FALSE(shared.Load("wf_1"));
    EXPECT_EQ(shared.ListIds().size(), 2u);
  }

  // A routine taught by voice (the profile's), and a copy of it brought in
  // by a save: the profile's too, its name made its own beside it.
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    voice::VoiceRoutineStore store(learning()->routines_dir(),
                                   learning()->shared_root());
    voice::ParseContext context;
    std::optional<voice::RoutineStep> open = voice::RoutineStepFor(
        voice::ParseVoiceCommand("open YouTube", context), "", 0, {}, false);
    ASSERT_TRUE(open);
    open->url = "https://www.youtube.com/";
    ASSERT_TRUE(
        store.Save("voice_1", "my news", {*open}, false, 1'800'000'000).saved);
  }
  EXPECT_NE(Ask("listWorkflows").find("\"voice\":true"), std::string::npos);
  const std::string routine = Ask("getWorkflow", "'voice_1'");
  ASSERT_NE(routine.find("\"name\":\"my news\""), std::string::npos) << routine;
  const std::string imported =
      Ask("saveWorkflow",
          "Object.assign(" + routine + ", {id: 'wf_import', name: 'my news'})");
  EXPECT_NE(imported.find("\"name\":\"my news (2)\""), std::string::npos)
      << imported;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    AutomationStorage mine(learning()->routines_dir());
    EXPECT_TRUE(mine.Load("wf_import"));
    AutomationStorage shared(
        AutomationStorage::SharedDirectory(automations_.GetPath()));
    EXPECT_FALSE(shared.Load("wf_import"));
  }
  EXPECT_EQ(Ask("deleteWorkflow", "'wf_import'"), "{\"ok\":true}");
}

// A message is answered on the sequence of the storage it reads: not while
// that sequence is busy, and as soon as it is free.
IN_PROC_BROWSER_TEST_F(AgentStudioStorageBrowserTest,
                       AnswersComeFromTheStoragesSequences) {
  // The profile's routines' sequence, voice mode's: listing waits for it.
  std::unique_ptr<base::WaitableEvent> routines =
      Hold(learning()->routines_task_runner());
  Send("listWorkflows");
  EXPECT_FALSE(Answered());
  routines->Signal();
  ASSERT_TRUE(base::test::RunUntil([&]() { return Answered(); }));

  // The shared storage's: a workflow there waits for it.
  std::unique_ptr<base::WaitableEvent> shared =
      Hold(AutomationStorage::SharedSequence());
  Send("getWorkflow", "'wf_1'");
  EXPECT_FALSE(Answered());
  shared->Signal();
  ASSERT_TRUE(base::test::RunUntil([&]() { return Answered(); }));
  EXPECT_NE(content::EvalJs(studio(), "window.answer")
                .ExtractString()
                .find("\"name\":\"Prices\""),
            std::string::npos);
}

// The page reloads while an answer is on its way: that answer is dropped,
// and the new page's request of the same callback id (ids start again
// with the page) gets its own.
IN_PROC_BROWSER_TEST_F(AgentStudioStorageBrowserTest,
                       AnAnswerForTheReloadedPageIsDropped) {
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    AutomationStorage shared(
        AutomationStorage::SharedDirectory(automations_.GetPath()));
    ASSERT_TRUE(shared.Save(Workflow("wf_2", "Weather")));
  }
  // getWorkflow(|id|) sent as "getWorkflow_101" (whatever the page sent
  // before), not waited for.
  const auto send_as_101 = [&](const std::string& id) {
    return content::ExecJs(
        studio(),
        "idCounter = 100; window.answer = null; sendWithPromise("
        "'getWorkflow', '" +
            id + "').then(r => { window.answer = JSON.stringify(r); });",
        content::EXECUTE_SCRIPT_NO_RESOLVE_PROMISES);
  };
  std::unique_ptr<base::WaitableEvent> shared =
      Hold(AutomationStorage::SharedSequence());
  ASSERT_TRUE(send_as_101("wf_1"));
  EXPECT_FALSE(Answered());

  content::TestNavigationObserver reloaded(studio(), 1);
  ASSERT_TRUE(content::ExecJs(studio(), "location.reload();"));
  reloaded.Wait();
  ASSERT_TRUE(send_as_101("wf_2"));
  EXPECT_FALSE(Answered());
  shared->Signal();
  ASSERT_TRUE(base::test::RunUntil([&]() { return Answered(); }));
  const std::string answer =
      content::EvalJs(studio(), "window.answer").ExtractString();
  EXPECT_NE(answer.find("\"name\":\"Weather\""), std::string::npos) << answer;
  EXPECT_EQ(answer.find("Prices"), std::string::npos) << answer;
  // And the page goes on being answered.
  EXPECT_NE(Ask("getWorkflow", "'wf_1'").find("\"name\":\"Prices\""),
            std::string::npos);
}

}  // namespace
}  // namespace molt_ai
