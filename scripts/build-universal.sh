#!/bin/bash
# MoltBrowser Universal Binary Build Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Builds a macOS Universal Binary (arm64 + x64) by:
#   1. Building arm64 (Apple Silicon)
#   2. Building x64 (Intel)
#   3. Merging with lipo into a universal .app bundle
#
# The result runs natively on both Apple Silicon and Intel Macs.
#
# Usage:
#   ./scripts/build-universal.sh
#   ./scripts/build-universal.sh --version 0.1.0 --sign "Developer ID Application: ..."
#   ./scripts/build-universal.sh --skip-x64   # arm64-only (faster)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
DIST_DIR="$ROOT_DIR/dist"
VERSION="0.1.0"
SIGNING_IDENTITY=""
SKIP_X64=false
SKIP_BUILD=false

export PATH="$ROOT_DIR/depot_tools:$PATH"

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --sign) SIGNING_IDENTITY="$2"; shift 2 ;;
    --skip-x64) SKIP_X64=true; shift ;;
    --skip-build) SKIP_BUILD=true; shift ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --version VERSION    Version string (default: 0.1.0)"
      echo "  --sign IDENTITY      Code signing identity"
      echo "  --skip-x64           Build arm64 only (no universal)"
      echo "  --skip-build         Skip building, just merge existing builds"
      echo ""
      echo "This script builds both arm64 and x64, then merges with lipo."
      echo "Requires ~2x build time and disk space."
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

echo "=== MoltBrowser Universal Binary Build ==="
echo "Version: $VERSION"
echo "Universal: $([ "$SKIP_X64" = true ] && echo "No (arm64 only)" || echo "Yes (arm64 + x64)")"
echo ""

BUILD_ARM64="out/MoltBrowser-arm64"
BUILD_X64="out/MoltBrowser-x64"
BUILD_UNIVERSAL="out/MoltBrowser-universal"

# Shared GN args (minus target_cpu)
SHARED_ARGS=$(cat "$ROOT_DIR/configs/macos-universal.gn")

cd "$CHROMIUM_SRC"

if [ "$SKIP_BUILD" = false ]; then
  # --- Build arm64 ---
  echo "=== Building arm64 (Apple Silicon) ==="
  mkdir -p "$BUILD_ARM64"
  GN_ARGS_ARM64="$SHARED_ARGS
target_cpu = \"arm64\"
target_os = \"mac\""
  gn gen "$BUILD_ARM64" --args="$GN_ARGS_ARM64"

  NCPU=$(sysctl -n hw.ncpu)
  autoninja -C "$BUILD_ARM64" chrome -j "$NCPU"
  echo "arm64 build complete."

  # --- Build x64 ---
  if [ "$SKIP_X64" = false ]; then
    echo ""
    echo "=== Building x64 (Intel) ==="
    mkdir -p "$BUILD_X64"
    GN_ARGS_X64="$SHARED_ARGS
target_cpu = \"x64\"
target_os = \"mac\""
    gn gen "$BUILD_X64" --args="$GN_ARGS_X64"
    autoninja -C "$BUILD_X64" chrome -j "$NCPU"
    echo "x64 build complete."
  fi
fi

# --- Merge into universal binary ---
echo ""
echo "=== Creating Universal Binary ==="

APP_ARM64="$BUILD_ARM64/Chromium.app"
APP_X64="$BUILD_X64/Chromium.app"
UNIVERSAL_APP="$BUILD_UNIVERSAL/MoltBrowser.app"

rm -rf "$BUILD_UNIVERSAL"
mkdir -p "$BUILD_UNIVERSAL"

if [ "$SKIP_X64" = true ]; then
  # Just copy arm64
  echo "Copying arm64 build (no universal merge)..."
  cp -R "$APP_ARM64" "$UNIVERSAL_APP"
else
  # Start with arm64 as base
  echo "Starting with arm64 as base..."
  cp -R "$APP_ARM64" "$UNIVERSAL_APP"

  # Find all Mach-O binaries and merge with lipo
  echo "Merging binaries with lipo..."
  MERGE_COUNT=0

  find "$UNIVERSAL_APP" -type f | while read -r file; do
    # Check if it's a Mach-O binary
    if file "$file" | grep -q "Mach-O"; then
      # Get the relative path within the .app
      REL_PATH="${file#$UNIVERSAL_APP/}"
      X64_FILE="$APP_X64/$REL_PATH"

      if [ -f "$X64_FILE" ]; then
        # Create universal binary with lipo
        TEMP_FILE="${file}.universal"
        if lipo -create "$file" "$X64_FILE" -output "$TEMP_FILE" 2>/dev/null; then
          mv "$TEMP_FILE" "$file"
          MERGE_COUNT=$((MERGE_COUNT + 1))
        else
          rm -f "$TEMP_FILE"
        fi
      fi
    fi
  done

  echo "Merged Mach-O binaries into universal format."
fi

# Rename the binary
if [ -f "$UNIVERSAL_APP/Contents/MacOS/Chromium" ]; then
  mv "$UNIVERSAL_APP/Contents/MacOS/Chromium" "$UNIVERSAL_APP/Contents/MacOS/MoltBrowser"
  # Update Info.plist executable name
  /usr/libexec/PlistBuddy -c "Set :CFBundleExecutable MoltBrowser" \
    "$UNIVERSAL_APP/Contents/Info.plist" 2>/dev/null || true
fi

# Patch Info.plist with MoltBrowser branding
echo "Patching Info.plist..."
"$ROOT_DIR/scripts/patch-info-plist.sh" "$UNIVERSAL_APP" "$VERSION"

# Apply app icon
if [ -f "$ROOT_DIR/branding/MoltBrowser.icns" ]; then
  cp "$ROOT_DIR/branding/MoltBrowser.icns" "$UNIVERSAL_APP/Contents/Resources/app.icns"
  /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile app.icns" \
    "$UNIVERSAL_APP/Contents/Info.plist" 2>/dev/null || true
  echo "App icon applied."
fi

# Verify architecture
echo ""
echo "Architecture verification:"
MAIN_BINARY="$UNIVERSAL_APP/Contents/MacOS/MoltBrowser"
if [ -f "$MAIN_BINARY" ]; then
  lipo -info "$MAIN_BINARY"
else
  # Fallback to Chromium name
  MAIN_BINARY="$UNIVERSAL_APP/Contents/MacOS/Chromium"
  [ -f "$MAIN_BINARY" ] && lipo -info "$MAIN_BINARY"
fi

APP_SIZE=$(du -sh "$UNIVERSAL_APP" | cut -f1)
echo "App size: $APP_SIZE"

# --- Code sign if identity provided ---
if [ -n "$SIGNING_IDENTITY" ]; then
  echo ""
  echo "=== Code Signing ==="
  "$ROOT_DIR/scripts/notarize.sh" \
    --identity "$SIGNING_IDENTITY" \
    --app "$UNIVERSAL_APP" \
    --skip-notarize
  echo "Signed with: $SIGNING_IDENTITY"
fi

# --- Package DMG ---
echo ""
echo "=== Packaging DMG ==="
mkdir -p "$DIST_DIR"

DMG_NAME="MoltBrowser-$VERSION-macOS-universal.dmg"
if [ "$SKIP_X64" = true ]; then
  DMG_NAME="MoltBrowser-$VERSION-macOS-arm64.dmg"
fi

"$ROOT_DIR/scripts/package-dmg.sh" \
  --app "$UNIVERSAL_APP" \
  --output "$DIST_DIR/$DMG_NAME" \
  --version "$VERSION" \
  ${SIGNING_IDENTITY:+--sign "$SIGNING_IDENTITY"}

echo ""
echo "=== Universal Build Complete ==="
echo "App: $UNIVERSAL_APP ($APP_SIZE)"
echo "DMG: $DIST_DIR/$DMG_NAME"
echo ""

if [ -z "$SIGNING_IDENTITY" ]; then
  echo "NOTE: This build is UNSIGNED. For GA release, run with:"
  echo "  $0 --version $VERSION --sign \"Developer ID Application: GenEye AI Labs Inc (TEAM_ID)\""
  echo ""
  echo "Or use the full release pipeline:"
  echo "  ./scripts/release.sh --version $VERSION --identity \"Developer ID Application: ...\" --notarize"
fi
