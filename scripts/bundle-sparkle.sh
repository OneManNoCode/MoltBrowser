#!/bin/bash
# MoltBrowser Sparkle Framework Bundler
# Copyright 2025 GenEye AI Labs Inc.
#
# Downloads and bundles Sparkle.framework into the MoltBrowser.app bundle
# so auto-update works at runtime. The sparkle_integration.mm code
# dynamically loads it from Contents/Frameworks/.
#
# Usage:
#   ./scripts/bundle-sparkle.sh                    # Download + bundle into built app
#   ./scripts/bundle-sparkle.sh --app /path/to/MoltBrowser.app
#   ./scripts/bundle-sparkle.sh --sparkle-version 2.6.4

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
SPARKLE_VERSION="2.6.4"
SPARKLE_CACHE="$ROOT_DIR/.sparkle-cache"
APP_PATH=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --app) APP_PATH="$2"; shift 2 ;;
    --sparkle-version) SPARKLE_VERSION="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --app PATH              Path to MoltBrowser.app bundle"
      echo "  --sparkle-version VER   Sparkle version (default: $SPARKLE_VERSION)"
      echo ""
      echo "If --app is not specified, searches for the app in common build locations."
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

echo "=== Sparkle Framework Bundler ==="
echo "Sparkle version: $SPARKLE_VERSION"

# Find the app bundle
if [ -z "$APP_PATH" ]; then
  for candidate in \
    "$CHROMIUM_SRC/out/MoltBrowser-universal/MoltBrowser.app" \
    "$CHROMIUM_SRC/out/MoltBrowser/Chromium.app" \
    "$ROOT_DIR/dist/"*.app; do
    if [ -d "$candidate" ]; then
      APP_PATH="$candidate"
      break
    fi
  done
fi

if [ -z "$APP_PATH" ] || [ ! -d "$APP_PATH" ]; then
  echo "ERROR: No .app bundle found. Specify with --app /path/to/MoltBrowser.app"
  exit 1
fi

echo "App bundle: $APP_PATH"

# --- Download Sparkle if not cached ---
SPARKLE_ZIP="$SPARKLE_CACHE/Sparkle-$SPARKLE_VERSION.tar.xz"
SPARKLE_DIR="$SPARKLE_CACHE/Sparkle-$SPARKLE_VERSION"

if [ ! -d "$SPARKLE_DIR/Sparkle.framework" ]; then
  echo ""
  echo "Downloading Sparkle $SPARKLE_VERSION..."
  mkdir -p "$SPARKLE_CACHE"

  SPARKLE_URL="https://github.com/nicklama/sparkle-project-Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz"
  # Fallback to official Sparkle
  SPARKLE_URL="https://github.com/nicklama/sparkle-project-Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz"
  SPARKLE_URL_OFFICIAL="https://github.com/nicklama/sparkle-project-Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz"

  # Try downloading from official Sparkle releases
  if ! curl -L -o "$SPARKLE_ZIP" \
    "https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz" 2>/dev/null; then
    echo "ERROR: Failed to download Sparkle $SPARKLE_VERSION"
    echo "Download manually from: https://sparkle-project.org/"
    echo "Extract to: $SPARKLE_DIR/"
    exit 1
  fi

  echo "Extracting..."
  mkdir -p "$SPARKLE_DIR"
  tar xf "$SPARKLE_ZIP" -C "$SPARKLE_DIR"
  echo "Sparkle downloaded and extracted."
fi

# Verify Sparkle.framework exists
if [ ! -d "$SPARKLE_DIR/Sparkle.framework" ]; then
  echo "ERROR: Sparkle.framework not found in $SPARKLE_DIR"
  echo "Contents:"
  ls -la "$SPARKLE_DIR/"
  exit 1
fi

# --- Bundle Sparkle.framework into the app ---
echo ""
echo "Bundling Sparkle.framework into app..."

FRAMEWORKS_DIR="$APP_PATH/Contents/Frameworks"
mkdir -p "$FRAMEWORKS_DIR"

# Copy Sparkle.framework
if [ -d "$FRAMEWORKS_DIR/Sparkle.framework" ]; then
  echo "Removing existing Sparkle.framework..."
  rm -rf "$FRAMEWORKS_DIR/Sparkle.framework"
fi

cp -R "$SPARKLE_DIR/Sparkle.framework" "$FRAMEWORKS_DIR/"
echo "Sparkle.framework bundled at: $FRAMEWORKS_DIR/Sparkle.framework"

# Also copy the XPC services that Sparkle needs
if [ -d "$SPARKLE_DIR/Sparkle.framework/XPCServices" ]; then
  echo "XPC services included."
fi

# --- Verify the framework loads ---
echo ""
echo "Verification:"
SPARKLE_BINARY="$FRAMEWORKS_DIR/Sparkle.framework/Sparkle"
if [ -f "$SPARKLE_BINARY" ]; then
  echo "  Binary: $(file "$SPARKLE_BINARY" | sed 's/.*: //')"
  echo "  Size: $(du -sh "$FRAMEWORKS_DIR/Sparkle.framework" | cut -f1)"

  # Check architectures
  ARCHS=$(lipo -archs "$SPARKLE_BINARY" 2>/dev/null || echo "unknown")
  echo "  Architectures: $ARCHS"
else
  echo "  WARNING: Sparkle binary not found at expected path"
fi

# --- Generate EdDSA signing key if not exists ---
KEYS_DIR="$ROOT_DIR/.sparkle-keys"
if [ ! -f "$KEYS_DIR/eddsa_key" ]; then
  echo ""
  echo "--- Generating EdDSA signing key ---"
  mkdir -p "$KEYS_DIR"

  # Check if Sparkle's generate_keys tool is available
  GENERATE_KEYS="$SPARKLE_DIR/bin/generate_keys"
  if [ -x "$GENERATE_KEYS" ]; then
    echo "Using Sparkle's generate_keys tool..."
    "$GENERATE_KEYS" -p "$KEYS_DIR"
    echo "EdDSA key generated at: $KEYS_DIR/"
    echo ""
    echo "IMPORTANT: Keep the private key SAFE and SECRET."
    echo "  Private key: $KEYS_DIR/eddsa_key"
    echo "  Add the PUBLIC key to Info.plist as SUPublicEDKey"
  else
    echo "NOTE: generate_keys tool not found in Sparkle distribution."
    echo "Generate manually with: $SPARKLE_DIR/bin/generate_keys -p $KEYS_DIR"
  fi
fi

# Check if public key needs to be added to Info.plist
if [ -f "$KEYS_DIR/eddsa_key.pub" ]; then
  PUB_KEY=$(cat "$KEYS_DIR/eddsa_key.pub")
  echo ""
  echo "Setting SUPublicEDKey in Info.plist..."
  /usr/libexec/PlistBuddy -c "Delete :SUPublicEDKey" "$APP_PATH/Contents/Info.plist" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c "Add :SUPublicEDKey string $PUB_KEY" "$APP_PATH/Contents/Info.plist"
  echo "Public key set in Info.plist."
fi

echo ""
echo "=== Sparkle Bundling Complete ==="
echo ""
echo "The app now includes Sparkle.framework for auto-updates."
echo "Next steps:"
echo "  1. Code sign the app: codesign --deep --force --sign 'Developer ID Application: ...' $APP_PATH"
echo "  2. Build DMG: ./scripts/package-dmg.sh --app $APP_PATH"
echo "  3. Sign the DMG for appcast: ./scripts/generate-appcast.sh"
