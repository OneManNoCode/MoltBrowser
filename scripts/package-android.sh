#!/bin/bash
# MoltBrowser Android Packaging Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Creates Android distribution packages:
#   1. APK (for direct install / sideloading)
#   2. AAB (Android App Bundle for Play Store)
#
# Prerequisites:
#   - Built MoltBrowser with --platform android
#   - Android SDK build-tools installed
#   - Signing keystore (for release builds)
#
# Usage:
#   ./scripts/package-android.sh [--version VERSION] [--sign] [--aab]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
BUILD_DIR="$CHROMIUM_SRC/out/MoltBrowser"
DIST_DIR="$ROOT_DIR/dist"
VERSION="0.1.0-alpha"
SIGN=false
BUILD_AAB=false
KEYSTORE=""
KEY_ALIAS="moltbrowser"

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --sign) SIGN=true; shift ;;
    --aab) BUILD_AAB=true; shift ;;
    --keystore) KEYSTORE="$2"; shift 2 ;;
    --key-alias) KEY_ALIAS="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --version VERSION   Version string (default: 0.1.0-alpha)"
      echo "  --sign              Sign the APK with release keystore"
      echo "  --aab               Also build Android App Bundle"
      echo "  --keystore PATH     Path to .jks keystore file"
      echo "  --key-alias NAME    Key alias in keystore (default: moltbrowser)"
      echo ""
      echo "To create a signing keystore:"
      echo "  keytool -genkey -v -keystore moltbrowser.jks -alias moltbrowser \\"
      echo "    -keyalg RSA -keysize 2048 -validity 10000"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

echo "=== MoltBrowser Android Packaging ==="
echo "Version: $VERSION"

# Locate APK from build output
APK_SRC="$BUILD_DIR/apks/ChromePublic.apk"
if [ ! -f "$APK_SRC" ]; then
  echo "ERROR: APK not found at $APK_SRC"
  echo "Build with: ./scripts/build.sh --platform android"
  exit 1
fi

mkdir -p "$DIST_DIR"

# Detect Android SDK build-tools
if [ -n "$ANDROID_HOME" ]; then
  BUILD_TOOLS=$(ls -d "$ANDROID_HOME/build-tools/"* 2>/dev/null | sort -V | tail -1)
elif [ -n "$ANDROID_SDK_ROOT" ]; then
  BUILD_TOOLS=$(ls -d "$ANDROID_SDK_ROOT/build-tools/"* 2>/dev/null | sort -V | tail -1)
fi

ZIPALIGN="${BUILD_TOOLS:+$BUILD_TOOLS/}zipalign"
APKSIGNER="${BUILD_TOOLS:+$BUILD_TOOLS/}apksigner"

# --- Stage 1: Align APK ---
echo ""
echo "--- Aligning APK ---"
ALIGNED_APK="$DIST_DIR/MoltBrowser-$VERSION-android-arm64-aligned.apk"
if command -v "$ZIPALIGN" &>/dev/null; then
  "$ZIPALIGN" -v -p 4 "$APK_SRC" "$ALIGNED_APK"
else
  echo "WARNING: zipalign not found — copying APK as-is"
  cp "$APK_SRC" "$ALIGNED_APK"
fi

# --- Stage 2: Sign APK ---
FINAL_APK="$DIST_DIR/MoltBrowser-$VERSION-android-arm64.apk"
if [ "$SIGN" = true ]; then
  echo ""
  echo "--- Signing APK ---"

  if [ -z "$KEYSTORE" ]; then
    KEYSTORE="$ROOT_DIR/signing/moltbrowser.jks"
  fi

  if [ ! -f "$KEYSTORE" ]; then
    echo "ERROR: Keystore not found at $KEYSTORE"
    echo "Create one with: keytool -genkey -v -keystore $KEYSTORE -alias $KEY_ALIAS -keyalg RSA -keysize 2048 -validity 10000"
    exit 1
  fi

  if command -v "$APKSIGNER" &>/dev/null; then
    "$APKSIGNER" sign --ks "$KEYSTORE" --ks-key-alias "$KEY_ALIAS" \
      --out "$FINAL_APK" "$ALIGNED_APK"
    echo "Signed APK: $FINAL_APK"

    # Verify signature
    "$APKSIGNER" verify --verbose "$FINAL_APK"
  else
    echo "WARNING: apksigner not found — using jarsigner fallback"
    cp "$ALIGNED_APK" "$FINAL_APK"
    jarsigner -verbose -sigalg SHA256withRSA -digestalg SHA-256 \
      -keystore "$KEYSTORE" "$FINAL_APK" "$KEY_ALIAS"
  fi
else
  cp "$ALIGNED_APK" "$FINAL_APK"
  echo "Unsigned APK: $FINAL_APK (use --sign for release)"
fi

# Clean up aligned intermediate
rm -f "$ALIGNED_APK"

echo "APK: $FINAL_APK ($(du -sh "$FINAL_APK" | cut -f1))"

# --- Stage 3: AAB (optional) ---
if [ "$BUILD_AAB" = true ]; then
  echo ""
  echo "--- Building AAB ---"
  echo "NOTE: AAB requires building with 'chrome_public_bundle' target."
  echo "  Run: ./scripts/build.sh --target chrome_public_bundle --platform android"

  AAB_SRC="$BUILD_DIR/apks/ChromePublic.aab"
  if [ -f "$AAB_SRC" ]; then
    AAB_DEST="$DIST_DIR/MoltBrowser-$VERSION-android-arm64.aab"
    cp "$AAB_SRC" "$AAB_DEST"
    echo "AAB: $AAB_DEST ($(du -sh "$AAB_DEST" | cut -f1))"
  else
    echo "AAB not found. Build with --target chrome_public_bundle first."
  fi
fi

echo ""
echo "=== Android Packaging Complete ==="
echo ""
echo "To install on device:"
echo "  adb install -r $FINAL_APK"
echo ""
echo "To upload to Play Store:"
echo "  1. Build AAB: ./scripts/build.sh --target chrome_public_bundle --platform android"
echo "  2. Package: ./scripts/package-android.sh --sign --aab"
echo "  3. Upload AAB to Google Play Console"
