// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/keys/molt_os_crypt_startup.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"

namespace molt_ai {

// static
MoltOSCryptStartupFactory* MoltOSCryptStartupFactory::GetInstance() {
  static base::NoDestructor<MoltOSCryptStartupFactory> instance;
  return instance.get();
}

MoltOSCryptStartupFactory::MoltOSCryptStartupFactory()
    : ProfileKeyedServiceFactory(
          "MoltOSCryptStartup",
          // Whichever profile comes first: API keys, the vault and the
          // form-filler profile are process-wide, and a session may open
          // only the profile picker or a guest window.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kOriginalOnly)
              .WithSystem(ProfileSelection::kOriginalOnly)
              .Build()) {}

MoltOSCryptStartupFactory::~MoltOSCryptStartupFactory() = default;

std::unique_ptr<KeyedService>
MoltOSCryptStartupFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  // Idempotent: only the first profile's call does anything.
  if (g_browser_process) {
    molt_os_crypt::Init(g_browser_process->os_crypt_async());
  }
  return nullptr;
}

bool MoltOSCryptStartupFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

bool MoltOSCryptStartupFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace molt_ai
