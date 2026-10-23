// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_background_browser.h"

#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <memory>

#include <ctime>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/automation/agent_inbox_registry.h"
#include "chrome/browser/molt_ai/automation/automation_runner.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/base_window.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/gfx/geometry/rect.h"
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
  BackgroundRunHolder(BrowserWindowInterface* browser,
                      molt_ai::BrowserAIRuntime* ai_runtime,
                      std::unique_ptr<AutomationStorage> storage,
                      content::WebContents* contents)
      : browser_(browser->GetWeakPtr()),
        ai_runtime_(ai_runtime),
        storage_(std::move(storage)),
        runner_(std::make_unique<AutomationRunner>(contents,
                                                    ai_runtime_.get(),
                                                    storage_.get())) {}

  void Start(Script script, size_t start_index) {
    // Register with the Agent Inbox so the side panel can show a live
    // status row for this run.
    inbox_id_ = AgentInboxRegistry::Get()->NewId();
    AgentInboxEntry entry;
    entry.id = inbox_id_;
    entry.script_id = script.id;
    entry.script_name = script.name.empty() ? script.id : script.name;
    for (const auto& step : script.steps) {
      if (step.type == StepType::NAVIGATE && !step.target.empty()) {
        entry.start_url = step.target;
        break;
      }
    }
    entry.total_steps = static_cast<int>(script.steps.size());
    entry.current_step = static_cast<int>(start_index);
    entry.status_note = "Starting";
    entry.started_at_unix = static_cast<int64_t>(time(nullptr));
    AgentInboxRegistry::Get()->Add(std::move(entry));

    const int64_t my_id = inbox_id_;
    runner_->Run(
        std::move(script),
        base::BindRepeating(
            [](int64_t id, const StepProgress& sp) {
              if (!sp.starting) {
                return;
              }
              AgentInboxRegistry::Get()->UpdateStep(
                  id, sp.index + 1, sp.description);
            },
            my_id),
        base::BindOnce(&BackgroundRunHolder::OnComplete,
                       base::Unretained(this)),
        start_index);
  }

 private:
  void OnComplete(RunResult result) {
    LOG(INFO) << "[MoltAutomation] background run finished success="
              << result.success
              << " steps=" << result.steps_executed
              << "/" << result.total_steps;
    // Mark finished in the inbox so the UI shows the final status briefly,
    // then prune the entry after 30s so the tray doesn't accumulate cruft.
    AgentInboxRegistry::Get()->Finish(inbox_id_, result.success);
    const int64_t id_to_clear = inbox_id_;
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(
            [](int64_t id) { AgentInboxRegistry::Get()->Remove(id); },
            id_to_clear),
        base::Seconds(30));
    // The user may have closed the run window already.
    if (browser_ && browser_->GetWindow())
      browser_->GetWindow()->Close();
    delete this;  // owns nothing else; safe to suicide here
  }

  base::WeakPtr<BrowserWindowInterface> browser_;
  // NOT owned — the runtime is a process-wide singleton
  // (BrowserAIRuntime::GetInstance). This holder does `delete this` when the run
  // completes; owning the runtime here meant a background run that finished
  // while a load/generation task was still in flight freed it under the task.
  raw_ptr<molt_ai::BrowserAIRuntime> ai_runtime_;
  std::unique_ptr<AutomationStorage> storage_;
  std::unique_ptr<AutomationRunner> runner_;
  int64_t inbox_id_ = 0;
};

}  // namespace

void RunScriptInBackgroundBrowser(
    Profile* profile,
    const Script& script,
    bool minimize,
    size_t start_index,
    const std::map<std::string, std::string>& variable_overrides) {
  if (!profile) {
    LOG(WARNING) << "[MoltAutomation] no profile, skipping background run";
    return;
  }

  // Pick the start URL from the first NAVIGATE step, fallback to
  // about:blank so we always have something to attach a runner to. This
  // load happens before the runner's policy checks, so it gets the same
  // http(s)-only rule (a .molt file must not open javascript:/file: URLs).
  GURL start_url("about:blank");
  // An unconfirmed draft (a web agent run, agent_run_draft.h) opens nothing:
  // the runner refuses it on about:blank, and the studio hears why.
  for (const auto& step : script.steps) {
    if (script.security.sites_unconfirmed)
      break;
    if (step.type == StepType::NAVIGATE && !step.target.empty()) {
      GURL g(step.target);
      if (agent_safety::IsAllowedNavigationURL(g)) {
        start_url = g;
      }
      break;
    }
  }

  BrowserWindowCreateParams params(BrowserWindowInterface::TYPE_POPUP, profile,
                                   /*from_user_gesture=*/true);
  params.is_trusted_source = true;
  // Replay at a desktop size — recordings are made on the desktop layout, and a
  // small/odd run window can trip a site's responsive layout so the recorded
  // (desktop) elements render hidden and won't match on replay.
  params.initial_bounds = gfx::Rect(60, 60, 1360, 900);
  // Auto mode: start minimized so the run window never flashes onto the
  // screen. Watch mode leaves the default show state (visible).
  if (minimize)
    params.initial_show_state = ui::mojom::WindowShowState::kMinimized;
  BrowserWindowInterface* browser = CreateBrowserWindow(std::move(params));
  if (!browser) {
    LOG(WARNING) << "[MoltAutomation] failed to create background browser";
    return;
  }

  NavigateParams nav(browser, start_url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  nav.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  // Don't force-show the window in Auto mode, or it flashes before Minimize().
  nav.window_action = minimize ? NavigateParams::WindowAction::kNoAction
                               : NavigateParams::WindowAction::kShowWindow;
  Navigate(&nav);
  content::WebContents* contents = nav.navigated_or_inserted_contents;
  if (!contents) {
    LOG(WARNING) << "[MoltAutomation] background navigate failed";
    browser->GetWindow()->Close();
    return;
  }

  if (minimize)
    browser->GetWindow()->Minimize();
  else
    browser->GetWindow()->Show();

  // Use the process-wide runtime singleton (shared with the chat UI). Only one
  // model fits in RAM at a time and its load/generation are serialized
  // internally, so sharing one instance is both cheaper and safer than the old
  // per-run instance, which could be freed under an in-flight background task.
  molt_ai::BrowserAIRuntime* ai_runtime = &molt_ai::BrowserAIRuntime::GetInstance();
  ai_runtime->Initialize();
  auto storage = std::make_unique<AutomationStorage>();
  storage->EnsureDirectory();

  // Script is move-only, so we reload a fresh copy from disk by id.
  auto fresh = storage->Load(script.id);
  if (!fresh) {
    LOG(WARNING) << "[MoltAutomation] failed to reload script id=" << script.id;
    browser->GetWindow()->Close();
    return;
  }

  // MoltBrowser: apply one-off run-time variable overrides ("Run with these
  // values" from the studio) ON TOP of the saved defaults, after the reload so
  // they are not lost. The saved workflow on disk is untouched; only this run
  // sees them. The scheduler passes no overrides → uses the saved defaults.
  for (const auto& kv : variable_overrides) {
    fresh->default_variables[kv.first] = kv.second;
  }

  // The holder self-destructs when the run finishes.
  auto* holder = new BackgroundRunHolder(browser, ai_runtime,
                                         std::move(storage), contents);
  holder->Start(std::move(*fresh), start_index);
}

}  // namespace automation
}  // namespace molt_ai
