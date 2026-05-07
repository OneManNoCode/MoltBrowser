// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_notification.h"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include "base/logging.h"
#include "base/strings/sys_string_conversions.h"

namespace molt_ai {
namespace automation {

void ShowAutomationNotification(Profile* /*profile*/,
                                const std::string& script_id,
                                const std::string& title,
                                const std::string& body) {
  @autoreleasepool {
    if (@available(macOS 10.14, *)) {
      UNUserNotificationCenter* center =
          [UNUserNotificationCenter currentNotificationCenter];
      [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                                UNAuthorizationOptionSound)
                            completionHandler:^(BOOL granted, NSError* err) {
        if (!granted) {
          LOG(WARNING) << "[MoltAutomation] notification auth not granted";
          return;
        }
      }];

      UNMutableNotificationContent* content =
          [[UNMutableNotificationContent alloc] init];
      content.title = base::SysUTF8ToNSString("Molt Automation");
      content.subtitle = base::SysUTF8ToNSString(title);
      content.body = base::SysUTF8ToNSString(body);
      content.sound = [UNNotificationSound defaultSound];
      content.userInfo = @{
        @"script_id" : base::SysUTF8ToNSString(script_id),
      };

      // Deliver immediately.
      UNNotificationRequest* req = [UNNotificationRequest
          requestWithIdentifier:base::SysUTF8ToNSString("molt-automation-" +
                                                         script_id)
                        content:content
                        trigger:nil];
      [center addNotificationRequest:req
               withCompletionHandler:^(NSError* err) {
        if (err) {
          LOG(WARNING) << "[MoltAutomation] notification deliver failed: "
                       << base::SysNSStringToUTF8(err.localizedDescription);
        }
      }];
    } else {
      LOG(INFO) << "[MoltAutomation] notify (no UN support): " << title
                << " — " << body;
    }
  }
}

}  // namespace automation
}  // namespace molt_ai
