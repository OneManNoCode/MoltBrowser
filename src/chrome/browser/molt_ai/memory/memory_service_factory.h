// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_FACTORY_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_FACTORY_H_

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace molt_ai {
namespace memory {

class MemoryService;

class MemoryServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static MemoryService* GetForProfile(Profile* profile);
  static MemoryServiceFactory* GetInstance();

  MemoryServiceFactory(const MemoryServiceFactory&) = delete;
  MemoryServiceFactory& operator=(const MemoryServiceFactory&) = delete;

 private:
  friend class base::NoDestructor<MemoryServiceFactory>;

  MemoryServiceFactory();
  ~MemoryServiceFactory() override;

  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_SERVICE_FACTORY_H_
