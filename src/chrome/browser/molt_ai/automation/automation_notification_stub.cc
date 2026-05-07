// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Non-mac stub for ShowAutomationNotification. Logs and returns.

#include "chrome/browser/molt_ai/automation/automation_notification.h"

#include "base/logging.h"

namespace molt_ai {
namespace automation {

void ShowAutomationNotification(Profile* /*profile*/,
                                const std::string& script_id,
                                const std::string& title,
                                const std::string& body) {
  LOG(INFO) << "[MoltAutomation] notify (stub) script=" << script_id
            << " title=" << title << " body=" << body;
}

}  // namespace automation
}  // namespace molt_ai
