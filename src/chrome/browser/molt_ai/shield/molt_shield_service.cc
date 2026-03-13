// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_service.h"

#include "base/logging.h"
#include "base/no_destructor.h"

namespace molt_ai {

MoltShield* GetMoltShieldInstance() {
  static base::NoDestructor<MoltShield> instance;

  // Initialize with default rules on first access.
  // MoltShield::Initialize() is safe to call multiple times —
  // it only loads rules once.
  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    instance->Initialize();
    LOG(INFO) << "[MoltShield] Ad blocker initialized with built-in rules";
  }

  return instance.get();
}

}  // namespace molt_ai
