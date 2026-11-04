// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationSchedulerService — profile-scoped owner of an
// AutomationScheduler over the shared storage. Auto-starts on creation so
// scheduled cron / interval triggers fire even if the user never opens
// the manager UI. Fires open a hidden background browser window when
// trust >= TRUSTED, otherwise a foreground tab so the user can watch.
// The storage is read and changed on its sequences
// (automation_shared_tasks.h), never on the UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_SERVICE_H_

#include <memory>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/molt_ai/automation/automation_scheduler.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace molt_ai {
namespace automation {

class AutomationSchedulerService : public KeyedService {
 public:
  explicit AutomationSchedulerService(Profile* profile);
  ~AutomationSchedulerService() override;

  AutomationSchedulerService(const AutomationSchedulerService&) = delete;
  AutomationSchedulerService& operator=(const AutomationSchedulerService&) =
      delete;

  // The shared storage's directory (no I/O), which the runner, the run
  // window and the Agent studio post their work for.
  const base::FilePath& storage_dir() const { return storage_dir_; }
  AutomationScheduler* scheduler() { return scheduler_.get(); }

  // KeyedService:
  void Shutdown() override;

 private:
  // Continues construction on the UI sequence after EnsureDirectory()
  // finishes on the storage's sequence. Wires the scheduler and starts it.
  void FinishStartOnUI(bool directory_ready);

  // Default fire callback: route to background-or-foreground runner.
  void OnTriggerFired(const Script& script, base::Time fired_at);

  raw_ptr<Profile> profile_;
  const base::FilePath storage_dir_;
  std::unique_ptr<AutomationScheduler> scheduler_;
  base::WeakPtrFactory<AutomationSchedulerService> weak_factory_{this};
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_SERVICE_H_
