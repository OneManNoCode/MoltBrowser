#!/bin/bash
# MoltBrowser DMG Packaging Script
# Creates a distributable macOS DMG from the built .app
#
# Usage: ./scripts/package-dmg.sh [--sign IDENTITY]
#
# Prerequisites:
#   - MoltBrowser.app must be built in chromium/src/out/MoltBrowser/
#   - create-dmg (brew install create-dmg) OR hdiutil (built-in)
#
# Copyright 2025 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$REPO_DIR/chromium/src/out/MoltBrowser"
APP_PATH="$BUILD_DIR/MoltBrowser.app"
DMG_DIR="$REPO_DIR/dist"
VERSION="0.1.0-alpha"
DMG_NAME="MoltBrowser-${VERSION}-macOS-arm64"
DMG_PATH="$DMG_DIR/${DMG_NAME}.dmg"

# Parse arguments
SIGN_IDENTITY=""
while [[ $# -gt 0 ]]; do
  case $1 in
    --sign)
      SIGN_IDENTITY="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

echo "=== MoltBrowser DMG Packager ==="
echo "Version: $VERSION"
echo ""

# Check that the app exists
if [ ! -d "$APP_PATH" ]; then
  echo "ERROR: MoltBrowser.app not found at $APP_PATH"
  echo "Please build first: autoninja -C chromium/src/out/MoltBrowser chrome"
  exit 1
fi

# Create dist directory
mkdir -p "$DMG_DIR"

# Get app size
APP_SIZE=$(du -sh "$APP_PATH" | cut -f1)
echo "App size: $APP_SIZE"

# Optional: code sign
if [ -n "$SIGN_IDENTITY" ]; then
  echo "Code signing with: $SIGN_IDENTITY"
  codesign --deep --force --sign "$SIGN_IDENTITY" "$APP_PATH"
  echo "Code signing complete"
fi

# Check for create-dmg (prettier DMGs)
if command -v create-dmg &> /dev/null; then
  echo "Using create-dmg for styled DMG..."

  # Remove existing DMG if present
  rm -f "$DMG_PATH"

  create-dmg \
    --volname "MoltBrowser" \
    --volicon "$APP_PATH/Contents/Resources/app.icns" \
    --window-pos 200 120 \
    --window-size 600 400 \
    --icon-size 100 \
    --icon "MoltBrowser.app" 175 190 \
    --app-drop-link 425 190 \
    --hide-extension "MoltBrowser.app" \
    --no-internet-enable \
    "$DMG_PATH" \
    "$APP_PATH" \
    || true  # create-dmg returns non-zero if icon not found, but DMG is still created

else
  echo "Using hdiutil for basic DMG..."

  # Create temporary directory for DMG contents
  STAGING_DIR=$(mktemp -d)
  trap "rm -rf $STAGING_DIR" EXIT

  # Copy app to staging
  echo "Copying MoltBrowser.app to staging..."
  cp -R "$APP_PATH" "$STAGING_DIR/"

  # Create Applications symlink for drag-install
  ln -s /Applications "$STAGING_DIR/Applications"

  # Create DMG
  echo "Creating DMG..."
  rm -f "$DMG_PATH"
  hdiutil create \
    -volname "MoltBrowser" \
    -srcfolder "$STAGING_DIR" \
    -ov \
    -format UDZO \
    "$DMG_PATH"
fi

# Verify
if [ -f "$DMG_PATH" ]; then
  DMG_SIZE=$(du -sh "$DMG_PATH" | cut -f1)
  echo ""
  echo "=== DMG Created Successfully ==="
  echo "Path: $DMG_PATH"
  echo "Size: $DMG_SIZE"
  echo ""
  echo "To install: Open the DMG and drag MoltBrowser to Applications"
else
  echo "ERROR: DMG creation failed"
  exit 1
fi
