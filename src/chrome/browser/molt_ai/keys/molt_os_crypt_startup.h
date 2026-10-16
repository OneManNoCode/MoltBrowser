// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Starts molt_os_crypt at browser startup. This is a keyed-service factory
// whose only job is to call molt_os_crypt::Init() with
// g_browser_process->os_crypt_async() when the first profile (regular, guest
// or system) is created. That happens after BrowserProcessImpl has created
// OSCryptAsync in PreMainMessageLoopRun(). It builds no service.
//
// It is registered through MemoryServiceFactory's DependsOn(), which
// EnsureBrowserContextKeyedServiceFactoriesBuilt() already reaches, so no
// upstream file has to name it.

#ifndef CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_STARTUP_H_
#define CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_STARTUP_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

namespace molt_ai {

class MoltOSCryptStartupFactory : public ProfileKeyedServiceFactory {
 public:
  static MoltOSCryptStartupFactory* GetInstance();

  MoltOSCryptStartupFactory(const MoltOSCryptStartupFactory&) = delete;
  MoltOSCryptStartupFactory& operator=(const MoltOSCryptStartupFactory&) =
      delete;

 private:
  friend class base::NoDestructor<MoltOSCryptStartupFactory>;

  MoltOSCryptStartupFactory();
  ~MoltOSCryptStartupFactory() override;

  // ProfileKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  // Unit tests with a TestingProfile install their own Encryptor through
  // molt_os_crypt::ScopedEncryptorForTesting instead.
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_KEYS_MOLT_OS_CRYPT_STARTUP_H_
