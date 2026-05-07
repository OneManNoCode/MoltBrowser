// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationSchedulerService — profile-scoped owner of an
// AutomationStorage + AutomationScheduler. Auto-starts on creation so
// scheduled cron / interval triggers fire even if the user never opens
// the manager UI. Fires open a hidden background browser window when
// trust >= TRUSTED, otherwise a foreground tab so the user can watch.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_SERVICE_H_

#include <memory>

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

  AutomationStorage* storage() { return storage_.get(); }
  AutomationScheduler* scheduler() { return scheduler_.get(); }

  // KeyedService:
  void Shutdown() override;

 private:
  // Default fire callback: route to background-or-foreground runner.
  void OnTriggerFired(const Script& script, base::Time fired_at);

  raw_ptr<Profile> profile_;
  std::unique_ptr<AutomationStorage> storage_;
  std::unique_ptr<AutomationScheduler> scheduler_;
  base::WeakPtrFactory<AutomationSchedulerService> weak_factory_{this};
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_SERVICE_H_
