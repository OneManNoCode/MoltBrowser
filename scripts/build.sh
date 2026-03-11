#!/bin/bash
# MoltBrowser Build Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Builds MoltBrowser from the configured Chromium source tree.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
BUILD_DIR="out/MoltBrowser"

export PATH="$ROOT_DIR/depot_tools:$PATH"

# Parse arguments
JOBS=""
TARGET="chrome"
VERBOSE=""

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
    --platform=*)
      # Platform override (used for cross-compilation)
      shift
      ;;
    --arch=*)
      # Architecture override
      shift
      ;;
    *)
      echo "Unknown argument: $1"
      exit 1
      ;;
  esac
done

echo "=== MoltBrowser Build ==="
echo "Source: $CHROMIUM_SRC"
echo "Build dir: $BUILD_DIR"
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
    NCPU=$(nproc)
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

# Report binary location
if [ "$(uname)" = "Darwin" ]; then
  BINARY="$CHROMIUM_SRC/$BUILD_DIR/Chromium.app"
  if [ -d "$BINARY" ]; then
    echo "Binary: $BINARY"
    echo "Size: $(du -sh "$BINARY" | cut -f1)"
  fi
else
  BINARY="$CHROMIUM_SRC/$BUILD_DIR/chrome"
  if [ -f "$BINARY" ]; then
    echo "Binary: $BINARY"
    echo "Size: $(du -sh "$BINARY" | cut -f1)"
  fi
fi

echo ""
echo "To run MoltBrowser:"
if [ "$(uname)" = "Darwin" ]; then
  echo "  open $BINARY"
else
  echo "  $BINARY"
fi
