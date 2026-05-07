// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_background_browser.h"

#include <memory>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {

namespace {

// Holds the runner + auxiliaries alive for the duration of one run, then
// self-destructs and closes the browser window.
class BackgroundRunHolder {
 public:
  BackgroundRunHolder(Browser* browser,
                      std::unique_ptr<molt_ai::BrowserAIRuntime> ai_runtime,
                      std::unique_ptr<AutomationStorage> storage,
                      content::WebContents* contents)
      : browser_(browser),
        ai_runtime_(std::move(ai_runtime)),
        storage_(std::move(storage)),
        runner_(std::make_unique<AutomationRunner>(contents,
                                                    ai_runtime_.get(),
                                                    storage_.get())) {}

  void Start(Script script) {
    runner_->Run(std::move(script),
                 /*on_step=*/base::DoNothing(),
                 base::BindOnce(&BackgroundRunHolder::OnComplete,
                                base::Unretained(this)));
  }

 private:
  void OnComplete(RunResult result) {
    LOG(INFO) << "[MoltAutomation] background run finished success="
              << result.success
              << " steps=" << result.steps_executed
              << "/" << result.total_steps;
    if (browser_ && browser_->window())
      browser_->window()->Close();
    delete this;  // owns nothing else; safe to suicide here
  }

  raw_ptr<Browser> browser_;
  std::unique_ptr<molt_ai::BrowserAIRuntime> ai_runtime_;
  std::unique_ptr<AutomationStorage> storage_;
  std::unique_ptr<AutomationRunner> runner_;
};

}  // namespace

void RunScriptInBackgroundBrowser(Profile* profile,
                                  const Script& script,
                                  bool minimize) {
  if (!profile) {
    LOG(WARNING) << "[MoltAutomation] no profile, skipping background run";
    return;
  }

  // Pick the start URL from the first NAVIGATE step, fallback to
  // about:blank so we always have something to attach a runner to.
  GURL start_url("about:blank");
  for (const auto& step : script.steps) {
    if (step.type == StepType::NAVIGATE && !step.target.empty()) {
      GURL g(step.target);
      if (g.is_valid()) {
        start_url = g;
      }
      break;
    }
  }

  Browser::CreateParams params(Browser::TYPE_POPUP, profile,
                               /*user_gesture=*/true);
  params.trusted_source = true;
  Browser* browser = Browser::Create(params);
  if (!browser) {
    LOG(WARNING) << "[MoltAutomation] failed to create background browser";
    return;
  }

  NavigateParams nav(browser, start_url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  nav.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  nav.window_action = NavigateParams::WindowAction::kShowWindow;
  Navigate(&nav);
  content::WebContents* contents = nav.navigated_or_inserted_contents;
  if (!contents) {
    LOG(WARNING) << "[MoltAutomation] background navigate failed";
    browser->window()->Close();
    return;
  }

  if (minimize)
    browser->window()->Minimize();
  else
    browser->window()->Show();

  // Spin up a fresh BrowserAIRuntime + AutomationStorage for this run.
  // (Profile-scoped sharing would be nice but the runtime currently
  // is owned per-handler, so a fresh instance is consistent.)
  auto ai_runtime = std::make_unique<molt_ai::BrowserAIRuntime>();
  auto storage = std::make_unique<AutomationStorage>();
  storage->EnsureDirectory();

  // Script is move-only, so we reload a fresh copy from disk by id.
  auto fresh = storage->Load(script.id);
  if (!fresh) {
    LOG(WARNING) << "[MoltAutomation] failed to reload script id=" << script.id;
    browser->window()->Close();
    return;
  }

  // The holder self-destructs when the run finishes.
  auto* holder = new BackgroundRunHolder(browser, std::move(ai_runtime),
                                         std::move(storage), contents);
  holder->Start(std::move(*fresh));
}

}  // namespace automation
}  // namespace molt_ai
