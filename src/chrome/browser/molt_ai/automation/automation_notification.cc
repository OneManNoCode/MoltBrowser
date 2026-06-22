// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Single cross-platform implementation of ShowAutomationNotification().
// Posts a real desktop notification on macOS, Windows, and Linux via
// Chromium's browser-process notification stack
// (NotificationDisplayService -> message_center::Notification). This
// replaces the former macOS-only UNUserNotificationCenter impl plus the
// no-op stub used on other desktop platforms.

#include "chrome/browser/molt_ai/automation/automation_notification.h"

#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/notifications/notification_display_service_factory.h"
#include "chrome/browser/notifications/notification_handler.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/models/image_model.h"
#include "ui/message_center/public/cpp/notification.h"
#include "ui/message_center/public/cpp/notification_delegate.h"
#include "ui/message_center/public/cpp/notification_types.h"
#include "ui/message_center/public/cpp/notifier_id.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {

void ShowAutomationNotification(Profile* profile,
                                const std::string& script_id,
                                const std::string& title,
                                const std::string& body) {
  // Must run on the UI thread with a valid Profile: the caller
  // (AutomationSchedulerService::OnTriggerFired) already invokes this on the
  // UI sequence. Guard defensively so a null profile or wrong thread is a
  // safe no-op rather than a crash.
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile) {
    LOG(WARNING) << "[MoltAutomation] notify skipped (null profile) script="
                 << script_id;
    return;
  }

  // Stable id so repeated fires for the same script coalesce instead of
  // stacking up. Matches the id scheme used by the former macOS impl.
  const std::string notification_id = "molt-automation-" + script_id;

  // NotifierType::APPLICATION with a stable app id groups all MoltBrowser
  // automation notifications under one source.
  message_center::NotifierId notifier_id(
      message_center::NotifierType::APPLICATION, "moltbrowser.automation");

  message_center::Notification notification(
      message_center::NOTIFICATION_TYPE_SIMPLE, notification_id,
      base::UTF8ToUTF16(title), base::UTF8ToUTF16(body),
      ui::ImageModel(), /*display_source=*/u"MoltBrowser Automation",
      /*origin_url=*/GURL(), notifier_id,
      message_center::RichNotificationData(),
      base::MakeRefCounted<message_center::NotificationDelegate>());

  NotificationDisplayServiceFactory::GetForProfile(profile)->Display(
      NotificationHandler::Type::TRANSIENT, notification, /*metadata=*/nullptr);

  LOG(INFO) << "[MoltAutomation] notify shown script=" << script_id
            << " title=" << title;
}

}  // namespace automation
}  // namespace molt_ai
