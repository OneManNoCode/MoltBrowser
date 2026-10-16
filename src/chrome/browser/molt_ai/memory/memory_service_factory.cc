// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_service_factory.h"

#include "chrome/browser/molt_ai/keys/molt_os_crypt_startup.h"
#include "chrome/browser/molt_ai/memory/memory_prefs.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"

namespace molt_ai {
namespace memory {

// static
MemoryService* MemoryServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<MemoryService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
MemoryServiceFactory* MemoryServiceFactory::GetInstance() {
  static base::NoDestructor<MemoryServiceFactory> instance;
  return instance.get();
}

MemoryServiceFactory::MemoryServiceFactory()
    : ProfileKeyedServiceFactory(
          "MoltMemoryService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .Build()) {
  // The memory DB is encrypted with molt_os_crypt's Encryptor. Depending on
  // the factory that starts molt_os_crypt also registers it: this
  // constructor runs from EnsureBrowserContextKeyedServiceFactoriesBuilt(),
  // so no upstream file needs to name it.
  DependsOn(MoltOSCryptStartupFactory::GetInstance());
}

MemoryServiceFactory::~MemoryServiceFactory() = default;

std::unique_ptr<KeyedService>
MemoryServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<MemoryService>(
      Profile::FromBrowserContext(context));
}

bool MemoryServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  // Auto-start with the profile so the index loads from disk and is
  // ready to serve queries by the time the user opens molt://memory/.
  return true;
}

bool MemoryServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

void MemoryServiceFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  prefs::RegisterProfilePrefs(registry);
}

}  // namespace memory
}  // namespace molt_ai
