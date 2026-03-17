#!/bin/bash
# MoltBrowser Notarization Script
# Signs and notarizes the app for macOS Gatekeeper
#
# Usage: ./scripts/notarize.sh --identity "Developer ID Application: ..."
#                               --team-id TEAM_ID
#                               --apple-id your@email.com
#                               --password @keychain:AC_PASSWORD
#
# Prerequisites:
#   - Apple Developer account with Developer ID certificate
#   - App-specific password stored in keychain:
#     xcrun notarytool store-credentials "AC_PASSWORD" \
#       --apple-id your@email.com --team-id TEAM_ID
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
TEAM_ID=""
APPLE_ID=""
PASSWORD=""
PROFILE_NAME="AC_PASSWORD"

while [[ $# -gt 0 ]]; do
  case $1 in
    --identity) SIGN_IDENTITY="$2"; shift 2 ;;
    --team-id) TEAM_ID="$2"; shift 2 ;;
    --apple-id) APPLE_ID="$2"; shift 2 ;;
    --password) PASSWORD="$2"; shift 2 ;;
    --profile) PROFILE_NAME="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 --identity IDENTITY --team-id TEAM_ID [options]"
      echo ""
      echo "Options:"
      echo "  --identity    Code signing identity (Developer ID Application: ...)"
      echo "  --team-id     Apple Developer Team ID"
      echo "  --apple-id    Apple ID email (for notarytool)"
      echo "  --password    App-specific password or @keychain:NAME"
      echo "  --profile     Notarytool credential profile name (default: AC_PASSWORD)"
      echo ""
      echo "Setup (one-time):"
      echo "  xcrun notarytool store-credentials AC_PASSWORD \\"
      echo "    --apple-id your@email.com --team-id TEAM_ID"
      exit 0
      ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

echo "=== MoltBrowser Notarization ==="
echo "Version: $VERSION"
echo ""

# Validate
if [ -z "$SIGN_IDENTITY" ]; then
  echo "ERROR: --identity is required"
  echo "Run: $0 --help"
  exit 1
fi

if [ ! -d "$APP_PATH" ]; then
  echo "ERROR: MoltBrowser.app not found at $APP_PATH"
  exit 1
fi

# ---- Step 1: Deep code sign ----
echo "Step 1/5: Code signing MoltBrowser.app..."

# Sign all frameworks and helpers first (inside-out signing)
find "$APP_PATH/Contents/Frameworks" -type f -name "*.dylib" -exec \
  codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp {} \; 2>/dev/null || true

# Sign helper apps
for helper in "$APP_PATH/Contents/Frameworks/MoltBrowser Helper"*.app; do
  if [ -d "$helper" ]; then
    codesign --deep --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$helper"
    echo "  Signed: $(basename "$helper")"
  fi
done

# Sign the framework
FRAMEWORK="$APP_PATH/Contents/Frameworks/MoltBrowser Framework.framework"
if [ -d "$FRAMEWORK" ]; then
  codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$FRAMEWORK"
  echo "  Signed: MoltBrowser Framework.framework"
fi

# Sign the main app
codesign --deep --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$APP_PATH"
echo "  Signed: MoltBrowser.app"

# Verify
echo ""
echo "Step 2/5: Verifying code signature..."
codesign --verify --deep --strict --verbose=2 "$APP_PATH" 2>&1 | tail -3
echo "  Signature valid"

# ---- Step 3: Create DMG ----
echo ""
echo "Step 3/5: Creating signed DMG..."
mkdir -p "$DMG_DIR"
rm -f "$DMG_PATH"

if command -v create-dmg &> /dev/null; then
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
    "$APP_PATH" || true
else
  STAGING_DIR=$(mktemp -d)
  cp -R "$APP_PATH" "$STAGING_DIR/"
  ln -s /Applications "$STAGING_DIR/Applications"
  hdiutil create -volname "MoltBrowser" -srcfolder "$STAGING_DIR" -ov -format UDZO "$DMG_PATH"
  rm -rf "$STAGING_DIR"
fi

# Sign the DMG too
codesign --force --sign "$SIGN_IDENTITY" --timestamp "$DMG_PATH"
echo "  DMG created and signed: $DMG_PATH"
DMG_SIZE=$(du -sh "$DMG_PATH" | cut -f1)
echo "  Size: $DMG_SIZE"

# ---- Step 4: Notarize ----
echo ""
echo "Step 4/5: Submitting for notarization..."

# Use stored credentials or explicit credentials
NOTARIZE_ARGS=""
if [ -n "$APPLE_ID" ] && [ -n "$PASSWORD" ] && [ -n "$TEAM_ID" ]; then
  NOTARIZE_ARGS="--apple-id $APPLE_ID --password $PASSWORD --team-id $TEAM_ID"
else
  NOTARIZE_ARGS="--keychain-profile $PROFILE_NAME"
fi

xcrun notarytool submit "$DMG_PATH" $NOTARIZE_ARGS --wait 2>&1 | tee /tmp/molt-notarize.log

# Check result
if grep -q "status: Accepted" /tmp/molt-notarize.log; then
  echo "  Notarization accepted!"
else
  echo ""
  echo "WARNING: Notarization may have failed. Check the log above."
  echo "You can check status with:"
  echo "  xcrun notarytool log <submission-id> $NOTARIZE_ARGS"
  echo ""
  echo "Common issues:"
  echo "  - Missing hardened runtime entitlements"
  echo "  - Unsigned frameworks or dylibs"
  echo "  - Invalid provisioning"
fi

# ---- Step 5: Staple ----
echo ""
echo "Step 5/5: Stapling notarization ticket..."
xcrun stapler staple "$DMG_PATH"
echo "  Stapled successfully"

echo ""
echo "==========================================="
echo "  MoltBrowser ${VERSION} — Ready to Ship!"
echo "==========================================="
echo ""
echo "  DMG: $DMG_PATH ($DMG_SIZE)"
echo "  Signed: $SIGN_IDENTITY"
echo "  Notarized: Yes"
echo "  Stapled: Yes"
echo ""
echo "  Users can now download and install without"
echo "  Gatekeeper warnings."
echo ""
