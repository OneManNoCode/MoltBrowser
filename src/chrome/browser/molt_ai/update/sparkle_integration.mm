// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Sparkle Auto-Update Integration — Objective-C++ bridge.
// Links to Sparkle.framework at runtime to avoid hard dependency.

#include "chrome/browser/molt_ai/update/sparkle_integration.h"

#import <Cocoa/Cocoa.h>
#include <dlfcn.h>

#include "base/logging.h"
#include "base/apple/bundle_locations.h"

// Sparkle SPUUpdater / SPUStandardUpdaterController interfaces.
// We load dynamically to gracefully degrade when Sparkle isn't bundled.

@protocol SUUpdaterDelegate <NSObject>
@optional
@end

@interface SPUStandardUpdaterController : NSObject
- (instancetype)initWithStartingUpdater:(BOOL)start
                        updaterDelegate:(id)delegate
                     userDriverDelegate:(id)userDriver;
- (void)checkForUpdates:(id)sender;
@property (nonatomic, readonly) id updater;
@end

namespace {

// Weak reference to the Sparkle updater controller.
// Objective-C ARC manages lifetime.
SPUStandardUpdaterController* g_updater_controller = nil;
bool g_initialized = false;
bool g_available = false;

NSString* GetSparkleFrameworkPath() {
  NSBundle* main_bundle = [NSBundle mainBundle];
  return [main_bundle.privateFrameworksPath
      stringByAppendingPathComponent:@"Sparkle.framework"];
}

}  // namespace

namespace molt_ai {

// static
void SparkleIntegration::Initialize() {
  if (g_initialized) return;
  g_initialized = true;

  @autoreleasepool {
    NSString* framework_path = GetSparkleFrameworkPath();
    NSBundle* sparkle_bundle = [NSBundle bundleWithPath:framework_path];

    if (!sparkle_bundle || ![sparkle_bundle load]) {
      LOG(INFO) << "[MoltAI] Sparkle.framework not found at "
                << [framework_path UTF8String]
                << " — auto-update disabled";
      g_available = false;
      return;
    }

    LOG(INFO) << "[MoltAI] Sparkle.framework loaded successfully";

    // Create the standard updater controller.
    // startingUpdater:YES triggers an automatic check on launch.
    Class controller_class =
        NSClassFromString(@"SPUStandardUpdaterController");
    if (!controller_class) {
      LOG(ERROR) << "[MoltAI] SPUStandardUpdaterController class not found";
      g_available = false;
      return;
    }

    g_updater_controller =
        [[controller_class alloc] initWithStartingUpdater:YES
                                          updaterDelegate:nil
                                       userDriverDelegate:nil];
    g_available = (g_updater_controller != nil);
    LOG(INFO) << "[MoltAI] Sparkle auto-update initialized: "
              << (g_available ? "active" : "failed");
  }
}

// static
void SparkleIntegration::CheckForUpdates() {
  if (!g_available || !g_updater_controller) {
    LOG(WARNING) << "[MoltAI] CheckForUpdates called but Sparkle not available";
    return;
  }

  @autoreleasepool {
    [g_updater_controller checkForUpdates:nil];
    LOG(INFO) << "[MoltAI] Manual update check triggered";
  }
}

// static
bool SparkleIntegration::IsAvailable() {
  return g_available;
}

// static
std::string SparkleIntegration::GetCurrentVersion() {
  @autoreleasepool {
    NSBundle* bundle = [NSBundle mainBundle];
    NSString* version = [bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    NSString* build = [bundle objectForInfoDictionaryKey:@"CFBundleVersion"];
    if (version && build) {
      return std::string([version UTF8String]) + " (" +
             std::string([build UTF8String]) + ")";
    }
    if (version) return std::string([version UTF8String]);
    return "unknown";
  }
}

// static
void SparkleIntegration::SetFeedURL(const std::string& url) {
  if (!g_available || !g_updater_controller) return;

  @autoreleasepool {
    // Access the underlying SPUUpdater and set feedURL
    id updater = [g_updater_controller updater];
    if (updater && [updater respondsToSelector:@selector(setFeedURL:)]) {
      NSURL* feed_url = [NSURL URLWithString:
          [NSString stringWithUTF8String:url.c_str()]];
      [updater performSelector:@selector(setFeedURL:) withObject:feed_url];
      LOG(INFO) << "[MoltAI] Sparkle feed URL set to: " << url;
    }
  }
}

}  // namespace molt_ai
