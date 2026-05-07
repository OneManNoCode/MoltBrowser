// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_scheduler_factory.h"

#include "chrome/browser/molt_ai/automation/automation_scheduler_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"

namespace molt_ai {
namespace automation {

// static
AutomationSchedulerService* AutomationSchedulerServiceFactory::GetForProfile(
    Profile* profile) {
  return static_cast<AutomationSchedulerService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
AutomationSchedulerServiceFactory*
AutomationSchedulerServiceFactory::GetInstance() {
  static base::NoDestructor<AutomationSchedulerServiceFactory> instance;
  return instance.get();
}

AutomationSchedulerServiceFactory::AutomationSchedulerServiceFactory()
    : ProfileKeyedServiceFactory(
          "MoltAutomationSchedulerService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .Build()) {}

AutomationSchedulerServiceFactory::~AutomationSchedulerServiceFactory() =
    default;

std::unique_ptr<KeyedService>
AutomationSchedulerServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<AutomationSchedulerService>(
      Profile::FromBrowserContext(context));
}

bool AutomationSchedulerServiceFactory::ServiceIsCreatedWithBrowserContext()
    const {
  // Start the scheduler as soon as the profile loads so cron triggers can
  // catch up on missed fires.
  return true;
}

bool AutomationSchedulerServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace automation
}  // namespace molt_ai
