#!/bin/bash
# MoltBrowser Full Release Pipeline
# Build → Patch → Sign → Package → Notarize → Upload
#
# Usage:
#   # Alpha (unsigned, no notarization)
#   ./scripts/release.sh --version 0.1.0-alpha --build 1
#
#   # Production (signed + notarized)
#   ./scripts/release.sh --version 0.1.0 --build 1 \
#     --identity "Developer ID Application: GenEye AI Labs Inc (TEAM_ID)" \
#     --team-id TEAM_ID \
#     --notarize
#
#   # Full release with GitHub upload
#   ./scripts/release.sh --version 0.1.0 --build 1 \
#     --identity "..." --team-id TEAM_ID --notarize --upload
#
# Copyright 2025 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# Defaults
VERSION=""
BUILD_NUM=""
SIGN_IDENTITY=""
TEAM_ID=""
DO_NOTARIZE=false
DO_UPLOAD=false
SKIP_BUILD=false

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --build) BUILD_NUM="$2"; shift 2 ;;
    --identity) SIGN_IDENTITY="$2"; shift 2 ;;
    --team-id) TEAM_ID="$2"; shift 2 ;;
    --notarize) DO_NOTARIZE=true; shift ;;
    --upload) DO_UPLOAD=true; shift ;;
    --skip-build) SKIP_BUILD=true; shift ;;
    --help|-h)
      echo "MoltBrowser Release Pipeline"
      echo ""
      echo "Usage: $0 --version VERSION --build BUILD_NUM [options]"
      echo ""
      echo "Options:"
      echo "  --version      Version string (e.g., 0.1.0-alpha)"
      echo "  --build        Build number (integer)"
      echo "  --identity     Code signing identity"
      echo "  --team-id      Apple Developer Team ID"
      echo "  --notarize     Submit for Apple notarization"
      echo "  --upload       Upload to GitHub Releases"
      echo "  --skip-build   Skip the Chromium build step"
      exit 0
      ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

if [ -z "$VERSION" ] || [ -z "$BUILD_NUM" ]; then
  echo "ERROR: --version and --build are required"
  echo "Run: $0 --help"
  exit 1
fi

DMG_NAME="MoltBrowser-${VERSION}-macOS-arm64"
DMG_PATH="$REPO_DIR/dist/${DMG_NAME}.dmg"
BUILD_DIR="$REPO_DIR/chromium/src/out/MoltBrowser"
APP_PATH="$BUILD_DIR/MoltBrowser.app"

echo "╔══════════════════════════════════════════╗"
echo "║    MoltBrowser Release Pipeline          ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Version:    $VERSION"
echo "║  Build:      $BUILD_NUM"
echo "║  Signed:     $([ -n "$SIGN_IDENTITY" ] && echo "Yes" || echo "No")"
echo "║  Notarize:   $DO_NOTARIZE"
echo "║  Upload:     $DO_UPLOAD"
echo "╚══════════════════════════════════════════╝"
echo ""

STEPS_TOTAL=6
STEP=0

# ---- Step 1: Build ----
STEP=$((STEP + 1))
if [ "$SKIP_BUILD" = false ]; then
  echo "[$STEP/$STEPS_TOTAL] Building MoltBrowser..."
  export PATH="$REPO_DIR/depot_tools:/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:$PATH"

  # Copy source files to Chromium tree
  if [ -d "$REPO_DIR/src/chrome/browser/ui/webui/molt_ai" ]; then
    cp "$REPO_DIR"/src/chrome/browser/ui/webui/molt_ai/*.cc \
       "$REPO_DIR/chromium/src/chrome/browser/ui/webui/molt_ai/" 2>/dev/null || true
    cp "$REPO_DIR"/src/chrome/browser/ui/webui/molt_ai/*.h \
       "$REPO_DIR/chromium/src/chrome/browser/ui/webui/molt_ai/" 2>/dev/null || true
  fi

  # Apply icon
  if [ -f "$REPO_DIR/branding/MoltBrowser.icns" ]; then
    cp "$REPO_DIR/branding/MoltBrowser.icns" \
       "$REPO_DIR/chromium/src/chrome/app/theme/chromium/mac/app.icns"
  fi

  autoninja -C "$BUILD_DIR" chrome
  echo "  Build complete"
else
  echo "[$STEP/$STEPS_TOTAL] Skipping build (--skip-build)"
fi

# ---- Step 2: Patch Info.plist ----
STEP=$((STEP + 1))
echo ""
echo "[$STEP/$STEPS_TOTAL] Patching Info.plist..."
bash "$SCRIPT_DIR/patch-info-plist.sh" "$VERSION" "$BUILD_NUM"

# ---- Step 3: Code Sign ----
STEP=$((STEP + 1))
echo ""
if [ -n "$SIGN_IDENTITY" ]; then
  echo "[$STEP/$STEPS_TOTAL] Code signing..."

  # Sign helpers and frameworks (inside-out)
  find "$APP_PATH/Contents/Frameworks" -type f -name "*.dylib" -exec \
    codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp {} \; 2>/dev/null || true

  for helper in "$APP_PATH/Contents/Frameworks/MoltBrowser Helper"*.app; do
    [ -d "$helper" ] && codesign --deep --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$helper"
  done

  FRAMEWORK="$APP_PATH/Contents/Frameworks/MoltBrowser Framework.framework"
  [ -d "$FRAMEWORK" ] && codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$FRAMEWORK"

  codesign --deep --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$APP_PATH"
  codesign --verify --deep --strict "$APP_PATH"
  echo "  Code signing complete and verified"
else
  echo "[$STEP/$STEPS_TOTAL] Skipping code signing (no --identity)"
fi

# ---- Step 4: Package DMG ----
STEP=$((STEP + 1))
echo ""
echo "[$STEP/$STEPS_TOTAL] Creating DMG..."
mkdir -p "$REPO_DIR/dist"
rm -f "$DMG_PATH"

if command -v create-dmg &> /dev/null; then
  create-dmg \
    --volname "MoltBrowser" \
    --volicon "$APP_PATH/Contents/Resources/app.icns" \
    --window-pos 200 120 --window-size 600 400 \
    --icon-size 100 --icon "MoltBrowser.app" 175 190 \
    --app-drop-link 425 190 \
    --hide-extension "MoltBrowser.app" \
    --no-internet-enable \
    "$DMG_PATH" "$APP_PATH" || true
else
  STAGING=$(mktemp -d)
  cp -R "$APP_PATH" "$STAGING/"
  ln -s /Applications "$STAGING/Applications"
  hdiutil create -volname "MoltBrowser" -srcfolder "$STAGING" -ov -format UDZO "$DMG_PATH"
  rm -rf "$STAGING"
fi

# Sign DMG if identity provided
[ -n "$SIGN_IDENTITY" ] && codesign --force --sign "$SIGN_IDENTITY" --timestamp "$DMG_PATH"

DMG_SIZE=$(du -sh "$DMG_PATH" | cut -f1)
echo "  DMG created: $DMG_PATH ($DMG_SIZE)"

# ---- Step 5: Notarize ----
STEP=$((STEP + 1))
echo ""
if [ "$DO_NOTARIZE" = true ] && [ -n "$SIGN_IDENTITY" ]; then
  echo "[$STEP/$STEPS_TOTAL] Notarizing..."
  if [ -n "$TEAM_ID" ]; then
    xcrun notarytool submit "$DMG_PATH" --keychain-profile "AC_PASSWORD" --wait
    xcrun stapler staple "$DMG_PATH"
    echo "  Notarized and stapled"
  else
    echo "  WARNING: --team-id required for notarization, skipping"
  fi
else
  echo "[$STEP/$STEPS_TOTAL] Skipping notarization"
fi

# ---- Step 6: Upload ----
STEP=$((STEP + 1))
echo ""
if [ "$DO_UPLOAD" = true ]; then
  echo "[$STEP/$STEPS_TOTAL] Uploading to GitHub Releases..."
  if command -v gh &> /dev/null; then
    # Create release tag
    git tag -f "v${VERSION}" HEAD

    # Create GitHub release
    gh release create "v${VERSION}" \
      --title "MoltBrowser v${VERSION}" \
      --notes "MoltBrowser v${VERSION} — AI-native browser with local LLM inference.

## Features
- Local AI chat powered by llama.cpp + Metal GPU
- Page-aware context (summarize, explain, extract)
- Multi-model support with one-click download
- First-run guided setup
- Keyboard shortcuts for AI actions
- Chat export/import
- Conversation search

## Install
Download the DMG, open it, and drag MoltBrowser to Applications." \
      "$DMG_PATH"

    echo "  Release created: https://github.com/OneManNoCode/MoltBrowser/releases/tag/v${VERSION}"

    # Update appcast
    bash "$SCRIPT_DIR/generate-appcast.sh" --version "$VERSION" --build "$BUILD_NUM" --dmg "$DMG_PATH"
  else
    echo "  ERROR: gh CLI not found. Install with: brew install gh"
  fi
else
  echo "[$STEP/$STEPS_TOTAL] Skipping upload"
fi

# ---- Summary ----
echo ""
echo "╔══════════════════════════════════════════╗"
echo "║    Release Complete!                     ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Version:    $VERSION (build $BUILD_NUM)"
echo "║  DMG:        $DMG_PATH"
echo "║  Size:       $DMG_SIZE"
echo "║  Signed:     $([ -n "$SIGN_IDENTITY" ] && echo "Yes" || echo "No")"
echo "║  Notarized:  $([ "$DO_NOTARIZE" = true ] && echo "Yes" || echo "No")"
echo "║  Uploaded:   $([ "$DO_UPLOAD" = true ] && echo "Yes" || echo "No")"
echo "╚══════════════════════════════════════════╝"
