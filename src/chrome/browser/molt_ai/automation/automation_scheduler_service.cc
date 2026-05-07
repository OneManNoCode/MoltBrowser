// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/molt_ai/automation/automation_background_browser.h"
#include "chrome/browser/molt_ai/automation/automation_notification.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace automation {

AutomationSchedulerService::AutomationSchedulerService(Profile* profile)
    : profile_(profile),
      storage_(std::make_unique<AutomationStorage>()),
      scheduler_(std::make_unique<AutomationScheduler>()) {
  storage_->EnsureDirectory();
  scheduler_->SetStorage(storage_.get());
  scheduler_->SetFireCallback(base::BindRepeating(
      &AutomationSchedulerService::OnTriggerFired,
      weak_factory_.GetWeakPtr()));
  scheduler_->Start();
  LOG(INFO) << "[MoltAutomation] scheduler service started for profile "
            << profile_->GetPath().value();
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

  // Headless if TRUSTED+, otherwise foreground tab so user sees activity.
  bool headless = script.security.trust >= TrustLevel::TRUSTED;
  // Pass by const ref; the helper will reload a fresh copy from storage
  // (Script is move-only, so this avoids a copy).
  RunScriptInBackgroundBrowser(profile_.get(), script, headless);
}

}  // namespace automation
}  // namespace molt_ai
