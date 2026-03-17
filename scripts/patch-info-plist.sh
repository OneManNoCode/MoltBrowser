#!/bin/bash
# Patch MoltBrowser's Info.plist with Sparkle auto-update keys
# and MoltBrowser version info.
#
# Run after build, before signing.
#
# Usage: ./scripts/patch-info-plist.sh [--version 0.1.0-alpha] [--build 1]
#
# Copyright 2025 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$REPO_DIR/chromium/src/out/MoltBrowser"
PLIST="$BUILD_DIR/MoltBrowser.app/Contents/Info.plist"

VERSION="${1:-0.1.0-alpha}"
BUILD_NUM="${2:-1}"

if [ ! -f "$PLIST" ]; then
  echo "ERROR: Info.plist not found at $PLIST"
  exit 1
fi

echo "=== Patching Info.plist ==="
echo "Version: $VERSION (build $BUILD_NUM)"

# Set version strings
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :CFBundleShortVersionString string $VERSION" "$PLIST"

/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $BUILD_NUM" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string $BUILD_NUM" "$PLIST"

# Sparkle feed URL
FEED_URL="https://moltbrowser.com/updates/appcast.xml"
/usr/libexec/PlistBuddy -c "Set :SUFeedURL $FEED_URL" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :SUFeedURL string $FEED_URL" "$PLIST"

# Enable Sparkle automatic checks
/usr/libexec/PlistBuddy -c "Set :SUEnableAutomaticChecks true" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :SUEnableAutomaticChecks bool true" "$PLIST"

# Check interval (daily = 86400 seconds)
/usr/libexec/PlistBuddy -c "Set :SUScheduledCheckInterval 86400" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :SUScheduledCheckInterval integer 86400" "$PLIST"

# Allow automatic downloads
/usr/libexec/PlistBuddy -c "Set :SUAutomaticallyUpdate false" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :SUAutomaticallyUpdate bool false" "$PLIST"

# MoltBrowser branding
/usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName MoltBrowser" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :CFBundleDisplayName string MoltBrowser" "$PLIST"

/usr/libexec/PlistBuddy -c "Set :CFBundleName MoltBrowser" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :CFBundleName string MoltBrowser" "$PLIST"

echo "  CFBundleShortVersionString = $VERSION"
echo "  CFBundleVersion = $BUILD_NUM"
echo "  SUFeedURL = $FEED_URL"
echo "  SUEnableAutomaticChecks = true"
echo "  SUScheduledCheckInterval = 86400"
echo ""
echo "Done!"
