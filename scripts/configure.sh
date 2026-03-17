#!/bin/bash
# MoltBrowser Build Configuration Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Configures the Chromium build with MoltBrowser-specific settings.
# Uses GN (Generate Ninja) build system.
#
# Usage:
#   ./scripts/configure.sh                           # Auto-detect host platform
#   ./scripts/configure.sh --platform linux --arch x64
#   ./scripts/configure.sh --platform windows --arch x64
#   ./scripts/configure.sh --platform android --arch arm64
#   ./scripts/configure.sh --platform ios --arch arm64
#   ./scripts/configure.sh --config configs/linux-x64.gn

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"

export PATH="$ROOT_DIR/depot_tools:$PATH"

# Defaults — auto-detect host
HOST_PLATFORM="$(uname -s)"
HOST_ARCH="$(uname -m)"
TARGET_PLATFORM=""
TARGET_ARCH=""
CONFIG_FILE=""
BUILD_DIR=""
DEBUG_BUILD=false

# Parse arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --platform|-p)
      TARGET_PLATFORM="$2"
      shift 2
      ;;
    --arch|-a)
      TARGET_ARCH="$2"
      shift 2
      ;;
    --config|-c)
      CONFIG_FILE="$2"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --debug)
      DEBUG_BUILD=true
      shift
      ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --platform, -p   Target platform: macos, linux, windows, android, ios"
      echo "  --arch, -a       Target CPU: arm64, x64, arm"
      echo "  --config, -c     Use a specific .gn config file"
      echo "  --build-dir      Custom output directory (default: out/MoltBrowser)"
      echo "  --debug          Enable debug build"
      echo "  --help, -h       Show this help"
      echo ""
      echo "Platforms & architectures:"
      echo "  macos     arm64, x64"
      echo "  linux     x64, arm64"
      echo "  windows   x64"
      echo "  android   arm64, arm"
      echo "  ios       arm64, sim-arm64"
      echo ""
      echo "Examples:"
      echo "  $0                                    # Auto-detect host"
      echo "  $0 --platform linux --arch x64"
      echo "  $0 --config configs/android-arm64.gn"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1 (use --help for usage)"
      exit 1
      ;;
  esac
done

echo "=== MoltBrowser Build Configuration ==="

# Resolve target platform
if [ -n "$CONFIG_FILE" ]; then
  echo "Using config file: $CONFIG_FILE"
  # Extract platform/arch from filename for display
  BASENAME=$(basename "$CONFIG_FILE" .gn)
  echo "Config: $BASENAME"
elif [ -z "$TARGET_PLATFORM" ]; then
  # Auto-detect from host
  case "$HOST_PLATFORM" in
    Darwin) TARGET_PLATFORM="macos" ;;
    Linux)  TARGET_PLATFORM="linux" ;;
    MINGW*|MSYS*|CYGWIN*) TARGET_PLATFORM="windows" ;;
    *)
      echo "ERROR: Unknown host platform: $HOST_PLATFORM"
      exit 1
      ;;
  esac
  if [ -z "$TARGET_ARCH" ]; then
    case "$HOST_ARCH" in
      arm64|aarch64) TARGET_ARCH="arm64" ;;
      x86_64|amd64)  TARGET_ARCH="x64" ;;
      *)
        echo "ERROR: Unknown host architecture: $HOST_ARCH"
        exit 1
        ;;
    esac
  fi
fi

# Resolve config file from platform+arch if not specified
if [ -z "$CONFIG_FILE" ]; then
  CONFIG_FILE="$ROOT_DIR/configs/${TARGET_PLATFORM}-${TARGET_ARCH}.gn"
  if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: No config found at $CONFIG_FILE"
    echo "Available configs:"
    ls -1 "$ROOT_DIR/configs/"*.gn 2>/dev/null || echo "  (none)"
    exit 1
  fi
fi

echo "Platform: ${TARGET_PLATFORM:-from-config}"
echo "Architecture: ${TARGET_ARCH:-from-config}"

# Determine build output directory
if [ -z "$BUILD_DIR" ]; then
  BUILD_DIR="out/MoltBrowser"
fi

cd "$CHROMIUM_SRC"

# Read the config file
GN_ARGS=$(cat "$CONFIG_FILE")

# Override debug if requested
if [ "$DEBUG_BUILD" = true ]; then
  GN_ARGS=$(echo "$GN_ARGS" | sed 's/is_debug = false/is_debug = true/')
  GN_ARGS=$(echo "$GN_ARGS" | sed 's/is_official_build = true/is_official_build = false/')
  echo "Mode: DEBUG"
else
  echo "Mode: RELEASE"
fi

echo ""
echo "GN Arguments:"
echo "$GN_ARGS"
echo ""

# Validate cross-compilation requirements
if [ -n "$TARGET_PLATFORM" ]; then
  case "$TARGET_PLATFORM" in
    android)
      if [ -z "$ANDROID_NDK_HOME" ] && [ -z "$ANDROID_NDK_ROOT" ]; then
        echo "WARNING: ANDROID_NDK_HOME not set. Android builds require the NDK."
        echo "  Install via: sdkmanager --install 'ndk;26.1.10909125'"
        echo "  Then: export ANDROID_NDK_HOME=\$HOME/Android/Sdk/ndk/26.1.10909125"
      fi
      ;;
    ios)
      if [ "$HOST_PLATFORM" != "Darwin" ]; then
        echo "ERROR: iOS builds require macOS with Xcode."
        exit 1
      fi
      ;;
    windows)
      if [ "$HOST_PLATFORM" != "MINGW64_NT" ] && [ "$HOST_PLATFORM" != "Linux" ] && [ "$HOST_PLATFORM" != "Darwin" ]; then
        echo "NOTE: Windows cross-compilation requires the Windows SDK sysroot."
        echo "  See: https://chromium.googlesource.com/chromium/src/+/main/docs/win_cross.md"
      fi
      ;;
  esac
fi

# Generate build files
echo "Generating build files in $BUILD_DIR..."
gn gen "$BUILD_DIR" --args="$GN_ARGS"

echo ""
echo "=== Configuration Complete ==="
echo ""
echo "Build directory: $CHROMIUM_SRC/$BUILD_DIR"
echo ""
echo "To build MoltBrowser:"
echo "  ./scripts/build.sh"
echo ""
