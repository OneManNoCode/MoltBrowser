// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_FACTORY_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_FACTORY_H_

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace molt_ai {
namespace automation {

class AutomationSchedulerService;

class AutomationSchedulerServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static AutomationSchedulerService* GetForProfile(Profile* profile);
  static AutomationSchedulerServiceFactory* GetInstance();

  AutomationSchedulerServiceFactory(const AutomationSchedulerServiceFactory&) =
      delete;
  AutomationSchedulerServiceFactory& operator=(
      const AutomationSchedulerServiceFactory&) = delete;

 private:
  friend class base::NoDestructor<AutomationSchedulerServiceFactory>;

  AutomationSchedulerServiceFactory();
  ~AutomationSchedulerServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_SCHEDULER_FACTORY_H_
