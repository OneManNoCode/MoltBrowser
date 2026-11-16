// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A run's time limit (AutomationSecurity::RunTimeLimit()) is a deadline:
// AutomationRunner ends the run with kMaxRuntimeExceeded when it passes,
// in the middle of a step too (a wait-for that would poll for minutes, a
// wait, a question waiting on the user), whoever owns the runner (the
// chat's runner kept by its own callback, the run window's holder, which
// deletes it in its callback). A run that ends in time completes. A run
// window's run at its limit closes its window and ends its Agent Inbox
// entry, so the overlap guard lets the next fire run, and its record says
// where it stopped.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/automation/agent_inbox_registry.h"
#include "chrome/browser/molt_ai/automation/automation_background_browser.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_shared_tasks.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/run_questions.h"
#include "chrome/browser/molt_ai/common/molt_home_dir_test_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai {
namespace automation {
namespace {

constexpr char kScriptId[] = "deadline-test";

// One button, which AQuestionWaitingAtTheLimitIsWithdrawn finds only by
// its weak aria-label locator.
std::unique_ptr<net::test_server::HttpResponse> ServeButton(
    const net::test_server::HttpRequest& request) {
  if (request.relative_url != "/button.html") {
    return nullptr;
  }
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_code(net::HTTP_OK);
  response->set_content_type("text/html");
  response->set_content(
      "<!doctype html><button aria-label=\"Send it\">Send</button>");
  return response;
}

Step MakeStep(StepType type, std::string target, std::string value = "") {
  Step step;
  step.type = type;
  step.target = std::move(target);
  step.value = std::move(value);
  return step;
}

// A workflow of |step| alone at |trust|, limited to |limit_seconds|.
Script OneStep(Step step, TrustLevel trust, int limit_seconds) {
  Script script;
  script.id = kScriptId;
  script.name = "Deadline test";
  script.security.trust = trust;
  script.security.max_runtime_seconds = limit_seconds;
  script.steps.push_back(std::move(step));
  return script;
}

constexpr char kBackgroundId[] = "deadline-bg";

// A run window's workflow: Casual, run by hand, limited to two seconds,
// with a wait-for that would poll for ten minutes.
Script BackgroundScript() {
  Step wait_for = MakeStep(StepType::WAIT_FOR, "#never");
  wait_for.timeout_ms = 600000;
  Script script = OneStep(std::move(wait_for), TrustLevel::CASUAL, 2);
  script.id = kBackgroundId;
  return script;
}

class AutomationRunnerDeadlineBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(
        base::BindRepeating(&ServeButton));
    ASSERT_TRUE(embedded_test_server()->Start());
    page_ = embedded_test_server()->GetURL("/button.html");
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), page_));
  }

  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  // A runner that keeps nothing on disk.
  std::unique_ptr<AutomationRunner> MakeRunner() {
    return std::make_unique<AutomationRunner>(
        web_contents(), /*ai_runtime=*/nullptr, base::FilePath());
  }

  GURL page_;
  // In place before the browser starts (the fixture is made first).
  ScopedMoltHomeDirForTesting molt_home_;
};

// A wait-for that would poll for ten minutes ends at the run's two-second
// limit (TRUSTED keeps its own), its runner kept alive by its own
// completion callback as the chat's is, and freed once that has run.
IN_PROC_BROWSER_TEST_F(AutomationRunnerDeadlineBrowserTest,
                       ALongStepEndsAtTheLimit) {
  Step wait_for = MakeStep(StepType::WAIT_FOR, "#never");
  wait_for.timeout_ms = 600000;
  bool freed = false;
  std::shared_ptr<AutomationRunner> runner(MakeRunner().release(),
                                           [&freed](AutomationRunner* r) {
                                             freed = true;
                                             delete r;
                                           });
  std::optional<RunResult> result;
  AutomationRunner* raw = runner.get();
  raw->Run(OneStep(std::move(wait_for), TrustLevel::TRUSTED, 2),
           base::DoNothing(),
           base::BindOnce([](std::optional<RunResult>* out,
                             std::shared_ptr<AutomationRunner> keep_alive,
                             RunResult r) { *out = std::move(r); },
                          &result, runner));
  runner.reset();
  ASSERT_TRUE(base::test::RunUntil([&] { return result.has_value(); }));
  EXPECT_FALSE(result->success);
  EXPECT_EQ(result->message, kMaxRuntimeExceeded);
  EXPECT_EQ(result->steps_executed, 0);
  EXPECT_GE(result->duration_ms, 2000);
  EXPECT_LT(result->duration_ms, 5000);
  EXPECT_TRUE(freed);
}

// A 30 s wait ends at a one-second limit (below TRUSTED, a shorter limit
// is kept), its runner deleted inside its own completion callback, as the
// run window's holder deletes it.
IN_PROC_BROWSER_TEST_F(AutomationRunnerDeadlineBrowserTest,
                       AWaitEndsAtTheLimit) {
  std::unique_ptr<AutomationRunner> runner = MakeRunner();
  std::optional<RunResult> result;
  runner->Run(
      OneStep(MakeStep(StepType::WAIT, "", "30"), TrustLevel::CASUAL, 1),
      base::DoNothing(), base::BindLambdaForTesting([&](RunResult r) {
        result = std::move(r);
        runner.reset();
      }));
  ASSERT_TRUE(base::test::RunUntil([&] { return result.has_value(); }));
  EXPECT_FALSE(result->success);
  EXPECT_EQ(result->message, kMaxRuntimeExceeded);
  EXPECT_LT(result->duration_ms, 4000);
  EXPECT_FALSE(runner);
}

// A run whose step ends in time completes, and its limit passing later
// ends nothing again.
IN_PROC_BROWSER_TEST_F(AutomationRunnerDeadlineBrowserTest,
                       ARunThatEndsInTimeCompletes) {
  std::unique_ptr<AutomationRunner> runner = MakeRunner();
  int ends = 0;
  std::optional<RunResult> result;
  runner->Run(
      OneStep(MakeStep(StepType::WAIT, "", "1"), TrustLevel::TRUSTED, 3),
      base::DoNothing(), base::BindLambdaForTesting([&](RunResult r) {
        ++ends;
        result = std::move(r);
      }));
  ASSERT_TRUE(base::test::RunUntil([&] { return result.has_value(); }));
  EXPECT_TRUE(result->success);
  EXPECT_EQ(result->message, "Completed");
  // Past the limit: nothing more happens.
  base::RunLoop run_loop;
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Seconds(3));
  run_loop.Run();
  EXPECT_FALSE(runner->is_running());
  EXPECT_EQ(ends, 1);
}

// A step waiting on the user's answer at the run's limit ends there, and
// its question is withdrawn: never the two minutes a question waits.
IN_PROC_BROWSER_TEST_F(AutomationRunnerDeadlineBrowserTest,
                       AQuestionWaitingAtTheLimitIsWithdrawn) {
  // The recorded selector is gone; only a weak locator (the button's
  // aria-label) finds the element, so the run asks.
  Step click = MakeStep(StepType::CLICK, "#gone");
  locators::StepLocators recorded;
  recorded.origin = url::Origin::Create(page_).Serialize();
  recorded.element = {"button", "", "button"};
  locators::Locator aria;
  aria.by = locators::By::kAttribute;
  aria.name = "aria-label";
  aria.value = "Send it";
  recorded.list.push_back(std::move(aria));
  click.locators = std::move(recorded);

  std::unique_ptr<AutomationRunner> runner = MakeRunner();
  std::optional<RunResult> result;
  runner->Run(
      OneStep(std::move(click), TrustLevel::TRUSTED, 3), base::DoNothing(),
      base::BindLambdaForTesting([&](RunResult r) { result = std::move(r); }));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return RunQuestions::Get()->PendingFor(kScriptId).has_value() ||
           result.has_value();
  }));
  std::optional<RunQuestion> question =
      RunQuestions::Get()->PendingFor(kScriptId);
  ASSERT_TRUE(question);
  ASSERT_TRUE(base::test::RunUntil([&] { return result.has_value(); }));
  EXPECT_FALSE(result->success);
  EXPECT_EQ(result->message, kMaxRuntimeExceeded);
  EXPECT_LT(result->duration_ms, 6000);
  EXPECT_FALSE(RunQuestions::Get()->PendingFor(kScriptId));
  EXPECT_FALSE(
      RunQuestions::Get()->Answer(question->id, kScriptId, /*allowed=*/true));
}

// A run window's run at its limit: its Agent Inbox entry ends (the overlap
// guard's IsRunning() goes false, so the next fire runs), its window
// closes, and its record says it stopped at its first step.
IN_PROC_BROWSER_TEST_F(AutomationRunnerDeadlineBrowserTest,
                       ABackgroundRunEndsAtTheLimit) {
  // Saved by the browser, so its record vouches for it.
  base::test::TestFuture<bool> saved;
  PostSharedChange(AutomationStorage::SharedDirectory(),
                   base::BindOnce([](AutomationStorage& storage) {
                     return storage.Save(BackgroundScript());
                   }),
                   saved.GetCallback());
  ASSERT_TRUE(saved.Get());

  const size_t windows = GetAllBrowserWindowInterfaces().size();
  const base::TimeTicks start = base::TimeTicks::Now();
  // It runs the workflow as stored, read again by its id.
  RunScriptInBackgroundBrowser(browser()->profile(), BackgroundScript(),
                               /*minimize=*/true);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return AgentInboxRegistry::Get()->IsRunning(kBackgroundId); }));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !AgentInboxRegistry::Get()->IsRunning(kBackgroundId); }));
  EXPECT_LT(base::TimeTicks::Now() - start, base::Seconds(8));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return GetAllBrowserWindowInterfaces().size() == windows; }));

  base::test::TestFuture<std::optional<Script>> stored;
  PostSharedRead(AutomationStorage::SharedDirectory(),
                 base::BindOnce([](AutomationStorage& storage) {
                   return storage.Load(kBackgroundId);
                 }),
                 stored.GetCallback());
  std::optional<Script> after = stored.Take();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->stats.last_result, kMaxRuntimeExceeded);
  EXPECT_EQ(after->stats.last_failed_step_index, 0);
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
