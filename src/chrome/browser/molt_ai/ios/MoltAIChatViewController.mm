// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// iOS AI Chat View Controller for MoltBrowser.
// Hosts a WKWebView pointed at the AI chat WebUI, providing the same
// interface as the desktop side panel adapted for iOS interaction patterns.
//
// On iOS, Chromium must use WKWebView (Apple mandate). The AI chat
// WebUI (HTML/CSS/JS) is loaded into a WKWebView with a JavaScript
// bridge to the native BrowserAIRuntime for on-device LLM inference.

#import "chrome/browser/molt_ai/ios/MoltAIChatViewController.h"

#import <WebKit/WebKit.h>

@interface MoltAIChatViewController () <WKNavigationDelegate, WKScriptMessageHandler>
@property(nonatomic, strong) WKWebView* webView;
@property(nonatomic, copy) NSString* pageURL;
@property(nonatomic, copy) NSString* selectedText;
@end

@implementation MoltAIChatViewController

- (instancetype)initWithPageURL:(NSString*)pageURL
                   selectedText:(NSString*)selectedText {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    _pageURL = [pageURL copy];
    _selectedText = [selectedText copy];
  }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];

  self.title = @"MoltBrowser AI";
  self.view.backgroundColor = [UIColor systemBackgroundColor];

  // Configure WKWebView with message handler for native bridge
  WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
  WKUserContentController* contentController =
      [[WKUserContentController alloc] init];

  // Register JavaScript bridge handlers
  [contentController addScriptMessageHandler:self name:@"moltAI"];
  [contentController addScriptMessageHandler:self name:@"moltDownload"];
  [contentController addScriptMessageHandler:self name:@"moltSettings"];
  config.userContentController = contentController;

  // Allow inline media playback (for potential audio responses)
  config.allowsInlineMediaPlayback = YES;
  config.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;

  // Create WebView
  self.webView = [[WKWebView alloc] initWithFrame:CGRectZero
                                    configuration:config];
  self.webView.navigationDelegate = self;
  self.webView.translatesAutoresizingMaskIntoConstraints = NO;
  self.webView.scrollView.contentInsetAdjustmentBehavior =
      UIScrollViewContentInsetAdjustmentAutomatic;

  [self.view addSubview:self.webView];

  // Full-screen constraints with safe area
  [NSLayoutConstraint activateConstraints:@[
    [self.webView.topAnchor
        constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
    [self.webView.leadingAnchor
        constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
    [self.webView.trailingAnchor
        constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
    [self.webView.bottomAnchor
        constraintEqualToAnchor:self.view.bottomAnchor],
  ]];

  // Navigation bar with close button
  self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                           target:self
                           action:@selector(dismissChat)];

  // Load the AI chat WebUI
  [self loadChatUI];
}

- (void)loadChatUI {
  // Build URL with context parameters
  NSURLComponents* components =
      [[NSURLComponents alloc] initWithString:@"chrome://molt-ai-chat"];
  NSMutableArray<NSURLQueryItem*>* queryItems = [NSMutableArray array];

  if (self.pageURL.length > 0) {
    [queryItems addObject:[NSURLQueryItem queryItemWithName:@"pageUrl"
                                                     value:self.pageURL]];
  }
  if (self.selectedText.length > 0) {
    [queryItems
        addObject:[NSURLQueryItem queryItemWithName:@"selectedText"
                                              value:self.selectedText]];
  }

  if (queryItems.count > 0) {
    components.queryItems = queryItems;
  }

  NSURL* url = components.URL;
  if (url) {
    [self.webView loadRequest:[NSURLRequest requestWithURL:url]];
  }
}

#pragma mark - WKScriptMessageHandler

- (void)userContentController:(WKUserContentController*)userContentController
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if ([message.name isEqualToString:@"moltAI"]) {
    [self handleAIMessage:message.body];
  } else if ([message.name isEqualToString:@"moltDownload"]) {
    [self handleDownloadMessage:message.body];
  } else if ([message.name isEqualToString:@"moltSettings"]) {
    [self handleSettingsMessage:message.body];
  }
}

- (void)handleAIMessage:(id)body {
  // Bridge to BrowserAIRuntime for LLM inference
  // TODO: Connect to native llama.cpp runtime via C++ bridge
  NSDictionary* message = (NSDictionary*)body;
  NSString* action = message[@"action"];

  if ([action isEqualToString:@"send"]) {
    NSString* prompt = message[@"prompt"];
    NSString* model = message[@"model"];
    NSLog(@"[MoltAI] Inference request: model=%@, prompt=%@",
          model, [prompt substringToIndex:MIN(50, prompt.length)]);
    // TODO: Forward to C++ BrowserAIRuntime
  } else if ([action isEqualToString:@"cancel"]) {
    NSLog(@"[MoltAI] Cancel inference");
    // TODO: Cancel ongoing inference
  }
}

- (void)handleDownloadMessage:(id)body {
  NSDictionary* message = (NSDictionary*)body;
  NSString* action = message[@"action"];

  if ([action isEqualToString:@"start"]) {
    NSString* modelId = message[@"modelId"];
    NSLog(@"[MoltAI] Download model: %@", modelId);
    // TODO: Start model download via NSURLSession
  } else if ([action isEqualToString:@"cancel"]) {
    NSLog(@"[MoltAI] Cancel download");
  }
}

- (void)handleSettingsMessage:(id)body {
  NSDictionary* message = (NSDictionary*)body;
  NSLog(@"[MoltAI] Settings update: %@", message);
  // TODO: Persist to UserDefaults or settings.json
}

#pragma mark - WKNavigationDelegate

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)navigationAction
                    decisionHandler:
                        (void (^)(WKNavigationActionPolicy))decisionHandler {
  NSURL* url = navigationAction.request.URL;

  // Allow chrome:// URLs (our WebUI)
  if ([url.scheme isEqualToString:@"chrome"]) {
    decisionHandler(WKNavigationActionPolicyAllow);
    return;
  }

  // Open external URLs in Safari / default browser
  if ([url.scheme isEqualToString:@"https"] ||
      [url.scheme isEqualToString:@"http"]) {
    [[UIApplication sharedApplication] openURL:url
                                       options:@{}
                             completionHandler:nil];
    decisionHandler(WKNavigationActionPolicyCancel);
    return;
  }

  decisionHandler(WKNavigationActionPolicyAllow);
}

#pragma mark - Actions

- (void)dismissChat {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)dealloc {
  // Remove script message handlers to break retain cycle
  WKUserContentController* controller =
      self.webView.configuration.userContentController;
  [controller removeScriptMessageHandlerForName:@"moltAI"];
  [controller removeScriptMessageHandlerForName:@"moltDownload"];
  [controller removeScriptMessageHandlerForName:@"moltSettings"];
}

@end
