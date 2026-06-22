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

# --- Ensure EdDSA signing key + set SUPublicEDKey (Sparkle 2.6.4) ---
# Sparkle 2.6.x is keychain-based: generate_keys stores the private key in the
# login keychain and prints the public key. The pre-2.x file-output form
# (`generate_keys -p <dir>`) was removed — `-p` now takes NO argument and just
# prints the existing public key. That CLI mismatch is what previously aborted
# this script (and forced the broken `release.sh --no-sparkle` packaging).
#   generate_keys        -> create keypair in keychain, print public key
#   generate_keys -p     -> print existing public key (non-zero exit if none)
#   generate_keys -x F   -> export private key (base64 seed) to file F
KEYS_DIR="$ROOT_DIR/.sparkle-keys"
mkdir -p "$KEYS_DIR"
chmod 700 "$KEYS_DIR" 2>/dev/null || true

GENERATE_KEYS="$SPARKLE_DIR/bin/generate_keys"
if [ ! -x "$GENERATE_KEYS" ]; then
  echo "ERROR: generate_keys not found at $GENERATE_KEYS"
  exit 1
fi

echo ""
echo "--- Sparkle EdDSA signing key ---"

# Look up the existing public key from the keychain (automation form).
# NOTE: `generate_keys -p` exits non-zero AND prints "ERROR: No existing
# signing key found!" to *stdout* when no key exists — so gate on the exit
# status and only capture stdout on success, otherwise the error text would be
# mistaken for a key.
PUB_KEY=""
if out="$("$GENERATE_KEYS" -p 2>/dev/null)"; then PUB_KEY="$out"; fi

if [ -z "$PUB_KEY" ]; then
  echo "No existing key in keychain — generating a new EdDSA keypair..."
  # Creates the keypair and stores the private key in the login keychain.
  "$GENERATE_KEYS" >/dev/null
  if out="$("$GENERATE_KEYS" -p 2>/dev/null)"; then PUB_KEY="$out"; fi
fi

if [ -z "$PUB_KEY" ] || [ "${PUB_KEY:0:5}" = "ERROR" ]; then
  echo "ERROR: Could not obtain the Sparkle EdDSA public key from the keychain."
  echo "       Run '$GENERATE_KEYS' manually (approve the keychain prompt and"
  echo "       unlock the login keychain), then re-run this script."
  exit 1
fi
echo "Public key: $PUB_KEY"

# Export a private-key backup for headless appcast signing
# (sign_update --ed-key-file) and disaster recovery. KEEP SECRET — the
# .sparkle-keys/ directory is gitignored. Done once; reused thereafter.
# INTERACTIVE ONLY: `generate_keys -x` can pop a keychain approval dialog,
# which would HANG a headless/CI build. Skip when there's no TTY; SUPublicEDKey
# is already set from `-p` above, and appcast signing can use the keychain
# key directly, so the dmg/auto-update verification is unaffected.
if [ -t 0 ] && [ ! -f "$KEYS_DIR/eddsa_key" ]; then
  if "$GENERATE_KEYS" -x "$KEYS_DIR/eddsa_key" >/dev/null 2>&1; then
    chmod 600 "$KEYS_DIR/eddsa_key"
    echo "Private key backup exported: $KEYS_DIR/eddsa_key"
  else
    echo "NOTE: private-key backup export skipped (keychain prompt declined);"
    echo "      appcast signing will fall back to the keychain key directly."
  fi
fi
printf '%s\n' "$PUB_KEY" > "$KEYS_DIR/eddsa_key.pub"

# Set SUPublicEDKey in the app's Info.plist so the app can verify updates.
PLIST="$APP_PATH/Contents/Info.plist"
echo "Setting SUPublicEDKey in Info.plist..."
/usr/libexec/PlistBuddy -c "Delete :SUPublicEDKey" "$PLIST" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Add :SUPublicEDKey string $PUB_KEY" "$PLIST"
echo "SUPublicEDKey set."

echo ""
echo "=== Sparkle Bundling Complete ==="
echo ""
echo "The app now includes Sparkle.framework for auto-updates."
echo "Next steps:"
echo "  1. Code sign the app: codesign --deep --force --sign 'Developer ID Application: ...' $APP_PATH"
echo "  2. Build DMG: ./scripts/package-dmg.sh --app $APP_PATH"
echo "  3. Sign the DMG for appcast: ./scripts/generate-appcast.sh"
