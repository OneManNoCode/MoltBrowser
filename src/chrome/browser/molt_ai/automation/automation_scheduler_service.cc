// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"

#include <string>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/molt_ai/automation/automation_background_browser.h"
#include "chrome/browser/molt_ai/automation/automation_notification.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_shared_tasks.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/profiles/profile.h"

namespace molt_ai {
namespace automation {

namespace {

// The audit log's reason for a fire refused.
std::string RefusalReason(const Script& script) {
  if (script.security.changed_outside) {
    return "changed_outside";
  }
  if (script.security.sites_unconfirmed) {
    return "draft_unconfirmed";
  }
  if (HasVoiceSteps(script)) {
    return "voice_routine";
  }
  return "disabled";
}

}  // namespace

AutomationSchedulerService::AutomationSchedulerService(Profile* profile)
    : profile_(profile),
      storage_dir_(AutomationStorage::SharedDirectory()),
      scheduler_(std::make_unique<AutomationScheduler>()) {
  // Disk I/O off the UI thread (KeyedService construction runs in
  // PreMainMessageLoopRun where blocking is disallowed): the directory is
  // made on the storage's sequence, then the scheduler (which uses
  // base::OneShotTimer and must live on the UI sequence) starts here.
  // The workflow record is loaded first (on the first start of this
  // version, seeded once: automation_storage.h), before the scheduler's
  // first listing or any page's read.
  PostSharedChange(storage_dir_, base::BindOnce([](AutomationStorage& storage) {
                     storage.PrepareRecord();
                     return storage.EnsureDirectory();
                   }),
                   base::BindOnce(&AutomationSchedulerService::FinishStartOnUI,
                                  weak_factory_.GetWeakPtr()));
  LOG(INFO) << "[MoltAutomation] scheduler service init queued for "
            << profile_->GetPath().value();
}

void AutomationSchedulerService::FinishStartOnUI(bool directory_ready) {
  if (!scheduler_)
    return;
  scheduler_->SetStorageDir(storage_dir_);
  scheduler_->SetFireCallback(base::BindRepeating(
      &AutomationSchedulerService::OnTriggerFired,
      weak_factory_.GetWeakPtr()));
  scheduler_->Start();
  LOG(INFO) << "[MoltAutomation] scheduler started"
            << (directory_ready ? "" : " (no automations directory yet)");
}

AutomationSchedulerService::~AutomationSchedulerService() = default;

void AutomationSchedulerService::Shutdown() {
  if (scheduler_)
    scheduler_->Stop();
}

void AutomationSchedulerService::OnTriggerFired(const Script& script,
                                                base::Time /*fired_at*/) {
  // Whoever fires it: only what may run unattended (never a workflow whose
  // file was changed outside the browser, nor a draft or a routine taught
  // by voice), whatever the scheduler read.
  if (!AutomationSecurity::MayFireUnattended(script)) {
    PostSharedAudit(storage_dir_, script.id, "scheduled_fire_refused",
                    RefusalReason(script));
    return;
  }
  LOG(INFO) << "[MoltAutomation] trigger fired for script id=" << script.id
            << " name=" << script.name;
  PostSharedAudit(
      storage_dir_, script.id, "scheduled_fire",
      "trust=" + std::to_string(static_cast<int>(script.security.trust)));

  // Show a desktop notification so the user knows something is happening.
  ShowAutomationNotification(profile_, script.id, script.name,
                             "Scheduled run starting…");

  // The fire time is already persisted, BEFORE the run: the scheduler
  // claimed this fire (AutomationStorage::ClaimFire(): the browser's record
  // of the fire first, then the file's trigger.last_fired_unix, never its
  // security content), so restart catch-up and interval phase-tracking
  // work across restarts, and no other profile's scheduler runs it too.
  // The run reloads a fresh copy (posted after that change, so it sees
  // it); its later stats write preserves this field.

  // Headless if TRUSTED+, otherwise foreground tab so user sees activity.
  bool headless = script.security.trust >= TrustLevel::TRUSTED;
  // Pass by const ref; the helper will reload a fresh copy from storage
  // (Script is move-only, so this avoids a copy).
  RunScriptInBackgroundBrowser(profile_.get(), script, headless);
}

}  // namespace automation
}  // namespace molt_ai
