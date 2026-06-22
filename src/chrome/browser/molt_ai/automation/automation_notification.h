// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Cross-platform thin wrapper around the OS notification center. Posts a
// real desktop notification on macOS, Windows, and Linux via Chromium's
// browser-process notification stack (NotificationDisplayService +
// message_center::Notification). Implemented once in
// automation_notification.cc.

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
