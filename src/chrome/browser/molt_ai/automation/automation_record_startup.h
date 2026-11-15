// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Configures the Molt Workflow Record (workflow_record.h) at browser
// startup: the record of the shared workflow directory
// (AutomationStorage::SharedDirectory()) is <user data dir>/Molt Workflow
// Record, and it may seed only when this user data directory isn't new
// (not Chrome's first run, and Local State existed before this launch: a
// fresh --user-data-dir started with --no-first-run is new too). A
// keyed-service factory whose only job is that, when the first profile
// (regular, guest or system) is created; it builds no service. Path math
// and cached state only, no I/O: the record is loaded on the storage's
// sequence, at its first use (AutomationStorage::PrepareRecord()).
//
// AutomationSchedulerServiceFactory DependsOn() it, which registers it and
// orders it before the scheduler's service.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORD_STARTUP_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORD_STARTUP_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

namespace molt_ai {
namespace automation {

// Configures the record once per process (later calls do nothing). UI
// thread.
void ConfigureWorkflowRecordOnce();

class AutomationRecordStartupFactory : public ProfileKeyedServiceFactory {
 public:
  static AutomationRecordStartupFactory* GetInstance();

  AutomationRecordStartupFactory(const AutomationRecordStartupFactory&) =
      delete;
  AutomationRecordStartupFactory& operator=(
      const AutomationRecordStartupFactory&) = delete;

 private:
  friend class base::NoDestructor<AutomationRecordStartupFactory>;

  AutomationRecordStartupFactory();
  ~AutomationRecordStartupFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  // Unit tests with a TestingProfile configure their own record
  // (ScopedWorkflowRecordForTesting).
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_RECORD_STARTUP_H_
