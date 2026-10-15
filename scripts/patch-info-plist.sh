#!/bin/bash
# Patch MoltBrowser's Info.plist with version and branding info.
# (Updates are handled in-app by molt_ai/update with signed release
# manifests; no Sparkle keys are written. See docs/SIGNED_UPDATES.md.)
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

# MoltBrowser branding
/usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName MoltBrowser" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :CFBundleDisplayName string MoltBrowser" "$PLIST"

/usr/libexec/PlistBuddy -c "Set :CFBundleName MoltBrowser" "$PLIST" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :CFBundleName string MoltBrowser" "$PLIST"

echo "  CFBundleShortVersionString = $VERSION"
echo "  CFBundleVersion = $BUILD_NUM"
echo ""
echo "Done!"
