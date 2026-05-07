// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Cross-platform thin wrapper around the OS notification center. On
// macOS, posts a top-right banner via UNUserNotificationCenter. On
// other platforms, currently logs to LOG(INFO) and returns.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_NOTIFICATION_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_NOTIFICATION_H_

#include <string>

class Profile;

namespace molt_ai {
namespace automation {

// Posts a "Molt Automation: <title>\n<body>" notification. |script_id|
// becomes the notification id so duplicate fires coalesce. Click opens
// chrome://molt-ai-automation/#<script_id>.
void ShowAutomationNotification(Profile* profile,
                                const std::string& script_id,
                                const std::string& title,
                                const std::string& body);

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_NOTIFICATION_H_
