// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/automation/automation_background_browser.h"
#include "chrome/browser/molt_ai/automation/automation_notification.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace automation {

AutomationSchedulerService::AutomationSchedulerService(Profile* profile)
    : profile_(profile),
      storage_(std::make_unique<AutomationStorage>()),
      scheduler_(std::make_unique<AutomationScheduler>()) {
  // Defer disk I/O off the UI thread (KeyedService construction runs in
  // PreMainMessageLoopRun where blocking is disallowed). After
  // EnsureDirectory completes on the thread pool we bounce back to the
  // UI sequence to start the scheduler (which uses base::OneShotTimer
  // and must live on the UI sequence).
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE,
      {base::TaskPriority::BEST_EFFORT, base::MayBlock(),
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(
          [](AutomationStorage* storage) { storage->EnsureDirectory(); },
          storage_.get()),
      base::BindOnce(&AutomationSchedulerService::FinishStartOnUI,
                      weak_factory_.GetWeakPtr()));
  LOG(INFO) << "[MoltAutomation] scheduler service init queued for "
            << profile_->GetPath().value();
}

void AutomationSchedulerService::FinishStartOnUI() {
  if (!scheduler_ || !storage_)
    return;
  scheduler_->SetStorage(storage_.get());
  scheduler_->SetFireCallback(base::BindRepeating(
      &AutomationSchedulerService::OnTriggerFired,
      weak_factory_.GetWeakPtr()));
  scheduler_->Start();
  LOG(INFO) << "[MoltAutomation] scheduler started";
}

AutomationSchedulerService::~AutomationSchedulerService() = default;

void AutomationSchedulerService::Shutdown() {
  if (scheduler_)
    scheduler_->Stop();
}

void AutomationSchedulerService::OnTriggerFired(const Script& script,
                                                base::Time fired_at) {
  LOG(INFO) << "[MoltAutomation] trigger fired for script id=" << script.id
            << " name=" << script.name;
  storage_->AppendAudit(script.id, "scheduled_fire",
                        "trust=" + std::to_string(static_cast<int>(
                                       script.security.trust)));

  // Show a desktop notification so the user knows something is happening.
  ShowAutomationNotification(profile_, script.id, script.name,
                             "Scheduled run starting…");

  // MoltBrowser: persist the fire time BEFORE the run so restart catch-up and
  // interval phase-tracking work across restarts (the scheduler reads
  // trigger.last_fired_unix). Written to disk here because the run reloads a
  // fresh copy; its later stats-Save preserves this field.
  if (storage_) {
    if (auto fresh = storage_->Load(script.id)) {
      fresh->trigger.last_fired_unix =
          static_cast<int64_t>(fired_at.InSecondsFSinceUnixEpoch());
      storage_->Save(*fresh);
    }
  }

  // Headless if TRUSTED+, otherwise foreground tab so user sees activity.
  bool headless = script.security.trust >= TrustLevel::TRUSTED;
  // Pass by const ref; the helper will reload a fresh copy from storage
  // (Script is move-only, so this avoids a copy).
  RunScriptInBackgroundBrowser(profile_.get(), script, headless);
}

}  // namespace automation
}  // namespace molt_ai
