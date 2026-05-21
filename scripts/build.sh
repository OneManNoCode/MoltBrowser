#!/bin/bash
# MoltBrowser Build Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Builds MoltBrowser from the configured Chromium source tree.
#
# Usage:
#   ./scripts/build.sh                        # Build default target
#   ./scripts/build.sh --target chrome_public_apk  # Android APK
#   ./scripts/build.sh --platform android     # Auto-detect Android target

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
BUILD_DIR="out/MoltBrowser"

export PATH="$ROOT_DIR/depot_tools:$PATH"

# Parse arguments
JOBS=""
TARGET=""
VERBOSE=""
PLATFORM=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --jobs|-j)
      JOBS="-j $2"
      shift 2
      ;;
    --target)
      TARGET="$2"
      shift 2
      ;;
    --verbose|-v)
      VERBOSE="-v"
      shift
      ;;
    --platform|-p)
      PLATFORM="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --jobs, -j N       Parallel jobs"
      echo "  --target TARGET    Build target (default: auto-detect)"
      echo "  --platform, -p P   Platform hint: macos, linux, windows, android, ios"
      echo "  --build-dir DIR    Build output directory"
      echo "  --verbose, -v      Verbose build output"
      echo ""
      echo "Default targets by platform:"
      echo "  macos/linux/windows: chrome"
      echo "  android:            chrome_public_apk"
      echo "  ios:                //ios/chrome/app:chrome"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1"
      exit 1
      ;;
  esac
done

# Auto-detect platform from args.gn if not specified
if [ -z "$PLATFORM" ] && [ -f "$CHROMIUM_SRC/$BUILD_DIR/args.gn" ]; then
  if grep -q 'target_os = "android"' "$CHROMIUM_SRC/$BUILD_DIR/args.gn" 2>/dev/null; then
    PLATFORM="android"
  elif grep -q 'target_os = "ios"' "$CHROMIUM_SRC/$BUILD_DIR/args.gn" 2>/dev/null; then
    PLATFORM="ios"
  elif grep -q 'target_os = "win"' "$CHROMIUM_SRC/$BUILD_DIR/args.gn" 2>/dev/null; then
    PLATFORM="windows"
  elif grep -q 'target_os = "linux"' "$CHROMIUM_SRC/$BUILD_DIR/args.gn" 2>/dev/null; then
    PLATFORM="linux"
  else
    PLATFORM="macos"
  fi
fi

# Set default target based on platform
if [ -z "$TARGET" ]; then
  case "$PLATFORM" in
    android)  TARGET="chrome_public_apk" ;;
    ios)      TARGET="//ios/chrome/app:chrome" ;;
    *)        TARGET="chrome" ;;
  esac
fi

echo "=== MoltBrowser Build ==="
echo "Source: $CHROMIUM_SRC"
echo "Build dir: $BUILD_DIR"
echo "Platform: ${PLATFORM:-auto}"
echo "Target: $TARGET"
echo ""

cd "$CHROMIUM_SRC"

# Check if configured
if [ ! -d "$BUILD_DIR" ]; then
  echo "ERROR: Build not configured. Run ./scripts/configure.sh first."
  exit 1
fi

# Detect CPU count for parallel build
if [ -z "$JOBS" ]; then
  if [ "$(uname)" = "Darwin" ]; then
    NCPU=$(sysctl -n hw.ncpu)
  else
    NCPU=$(nproc 2>/dev/null || echo 4)
  fi
  JOBS="-j $NCPU"
fi

echo "Building with $JOBS..."
echo "Started at: $(date)"
echo ""

# Build using autoninja (Chromium's ninja wrapper)
autoninja -C "$BUILD_DIR" $TARGET $JOBS $VERBOSE

echo ""
echo "=== Build Complete ==="
echo "Finished at: $(date)"
echo ""

# Phase B.3: bundle the tor binary into MoltBrowser.app so users
# never need to `brew install tor`. Only runs on macOS builds — we
# bundle Linux tor through the .deb in package-linux.sh and Windows
# tor through the NSIS script in package-win.sh (those are wired in
# separately to keep this build script platform-clean).
if [ "${PLATFORM:-auto}" = "macos" ] || \
   { [ "${PLATFORM:-auto}" = "auto" ] && [ "$(uname)" = "Darwin" ]; }; then
  APP="$CHROMIUM_SRC/$BUILD_DIR/MoltBrowser.app"
  if [ -d "$APP" ]; then
    echo "=== Bundling Tor ==="
    "$SCRIPT_DIR/bundle-tor.sh" --app "$APP" || {
      echo "[build] WARNING: tor bundling failed. The app builds fine"
      echo "[build] but /tor launch will fall back to system tor if any."
    }
    echo ""
    # Phase Tier-5: bundle whisper.cpp for the voice-mode mic button.
    echo "=== Bundling Whisper ==="
    "$SCRIPT_DIR/bundle-whisper.sh" --app "$APP" || {
      echo "[build] WARNING: whisper bundling failed. /transcribeAudio"
      echo "[build] will fall back to a system-installed whisper-cli."
    }
    echo ""
  fi
fi


# Report binary location based on platform
case "${PLATFORM:-auto}" in
  macos)
    BINARY="$CHROMIUM_SRC/$BUILD_DIR/Chromium.app"
    if [ -d "$BINARY" ]; then
      echo "Binary: $BINARY"
      echo "Size: $(du -sh "$BINARY" | cut -f1)"
      echo ""
      echo "To run: open $BINARY"
    fi
    ;;
  windows)
    BINARY="$CHROMIUM_SRC/$BUILD_DIR/chrome.exe"
    if [ -f "$BINARY" ]; then
      echo "Binary: $BINARY"
      echo "Size: $(du -sh "$BINARY" | cut -f1)"
    fi
    ;;
  linux)
    BINARY="$CHROMIUM_SRC/$BUILD_DIR/chrome"
    if [ -f "$BINARY" ]; then
      echo "Binary: $BINARY"
      echo "Size: $(du -sh "$BINARY" | cut -f1)"
      echo ""
      echo "To run: $BINARY"
    fi
    ;;
  android)
    APK="$CHROMIUM_SRC/$BUILD_DIR/apks/ChromePublic.apk"
    if [ -f "$APK" ]; then
      echo "APK: $APK"
      echo "Size: $(du -sh "$APK" | cut -f1)"
      echo ""
      echo "To install: adb install -r $APK"
    fi
    ;;
  ios)
    APP="$CHROMIUM_SRC/$BUILD_DIR/Chromium.app"
    if [ -d "$APP" ]; then
      echo "App: $APP"
      echo "Size: $(du -sh "$APP" | cut -f1)"
      echo ""
      echo "To run on simulator: xcrun simctl install booted $APP"
    fi
    ;;
  *)
    # Fallback: detect from host OS
    if [ "$(uname)" = "Darwin" ]; then
      BINARY="$CHROMIUM_SRC/$BUILD_DIR/Chromium.app"
      [ -d "$BINARY" ] && echo "Binary: $BINARY" && echo "To run: open $BINARY"
    else
      BINARY="$CHROMIUM_SRC/$BUILD_DIR/chrome"
      [ -f "$BINARY" ] && echo "Binary: $BINARY" && echo "To run: $BINARY"
    fi
    ;;
esac
