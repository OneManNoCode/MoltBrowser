// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Global MoltShield service — provides a singleton ad blocker instance.
// Initialized once at browser startup with default filter rules.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SERVICE_H_

#include "chrome/browser/molt_ai/shield/molt_shield.h"

namespace molt_ai {

// Returns the global MoltShield instance. Creates and initializes it
// with default built-in rules on first call. Thread-safe for reads
// after initialization.
MoltShield* GetMoltShieldInstance();

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_SERVICE_H_
