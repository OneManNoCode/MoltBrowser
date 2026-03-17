// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// iOS AI Chat View Controller header.

#ifndef CHROME_BROWSER_MOLT_AI_IOS_MOLT_AI_CHAT_VIEW_CONTROLLER_H_
#define CHROME_BROWSER_MOLT_AI_IOS_MOLT_AI_CHAT_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

// View controller that hosts the MoltBrowser AI chat interface.
// Wraps a WKWebView loading chrome://molt-ai-chat with native bridge
// for on-device LLM inference.
@interface MoltAIChatViewController : UIViewController

// Initialize with optional page context from the current tab.
// @param pageURL   URL of the page the user was viewing when opening chat.
// @param selectedText  Any text selected on the page.
- (instancetype)initWithPageURL:(NSString*)pageURL
                   selectedText:(NSString*)selectedText;

@end

#endif  // CHROME_BROWSER_MOLT_AI_IOS_MOLT_AI_CHAT_VIEW_CONTROLLER_H_
