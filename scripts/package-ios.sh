#!/bin/bash
# MoltBrowser iOS Packaging Script
# Copyright 2025 GenEye AI Labs Inc.
#
# Creates iOS distribution packages:
#   1. .app for simulator testing
#   2. .ipa for device / TestFlight / App Store
#
# Prerequisites:
#   - macOS with Xcode 15+
#   - Built MoltBrowser with --platform ios
#   - Apple Developer account (for IPA signing)
#
# Usage:
#   ./scripts/package-ios.sh [--version VERSION] [--export-method METHOD]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
BUILD_DIR="$CHROMIUM_SRC/out/MoltBrowser"
DIST_DIR="$ROOT_DIR/dist"
VERSION="0.1.0-alpha"
EXPORT_METHOD="development"  # development, ad-hoc, app-store, enterprise
TEAM_ID=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --export-method) EXPORT_METHOD="$2"; shift 2 ;;
    --team-id) TEAM_ID="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --version VERSION        Version string (default: 0.1.0-alpha)"
      echo "  --export-method METHOD   development|ad-hoc|app-store|enterprise"
      echo "  --team-id TEAM_ID        Apple Developer Team ID"
      echo ""
      echo "Export methods:"
      echo "  development   For running on development devices"
      echo "  ad-hoc        For distribution to specific devices"
      echo "  app-store     For App Store / TestFlight submission"
      echo "  enterprise    For enterprise in-house distribution"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

# Verify macOS
if [ "$(uname -s)" != "Darwin" ]; then
  echo "ERROR: iOS packaging requires macOS with Xcode."
  exit 1
fi

echo "=== MoltBrowser iOS Packaging ==="
echo "Version: $VERSION"
echo "Export method: $EXPORT_METHOD"

# Verify build
APP_PATH="$BUILD_DIR/Chromium.app"
if [ ! -d "$APP_PATH" ]; then
  echo "ERROR: iOS build not found at $APP_PATH"
  echo "Build with: ./scripts/build.sh --platform ios"
  exit 1
fi

mkdir -p "$DIST_DIR"

# --- Stage 1: Simulator bundle ---
echo ""
echo "--- Creating simulator bundle ---"
SIM_DIR="$DIST_DIR/MoltBrowser-$VERSION-ios-sim"
rm -rf "$SIM_DIR"
cp -R "$APP_PATH" "$SIM_DIR.app"
echo "Simulator app: $SIM_DIR.app"
echo "Install: xcrun simctl install booted $SIM_DIR.app"

# --- Stage 2: Export IPA ---
echo ""
echo "--- Creating IPA ---"

# Create ExportOptions.plist
EXPORT_PLIST="$DIST_DIR/.exportOptions.plist"
cat > "$EXPORT_PLIST" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>method</key>
	<string>$EXPORT_METHOD</string>
	<key>compileBitcode</key>
	<false/>
	<key>stripSwiftSymbols</key>
	<true/>
	<key>thinning</key>
	<string>&lt;none&gt;</string>
EOF

if [ -n "$TEAM_ID" ]; then
  cat >> "$EXPORT_PLIST" << EOF
	<key>teamID</key>
	<string>$TEAM_ID</string>
EOF
fi

cat >> "$EXPORT_PLIST" << EOF
</dict>
</plist>
EOF

# Create IPA manually (xcrun alternative)
IPA_NAME="MoltBrowser-$VERSION-ios-arm64.ipa"
IPA_PATH="$DIST_DIR/$IPA_NAME"

# Standard IPA structure: Payload/App.app
PAYLOAD_DIR="$DIST_DIR/.ipa-payload"
rm -rf "$PAYLOAD_DIR"
mkdir -p "$PAYLOAD_DIR/Payload"
cp -R "$APP_PATH" "$PAYLOAD_DIR/Payload/MoltBrowser.app"

# Sign if we have an identity
if [ -n "$TEAM_ID" ]; then
  echo "Signing with team: $TEAM_ID"
  SIGNING_IDENTITY=$(security find-identity -v -p codesigning | grep "$TEAM_ID" | head -1 | awk -F'"' '{print $2}')
  if [ -n "$SIGNING_IDENTITY" ]; then
    codesign --force --sign "$SIGNING_IDENTITY" \
      --entitlements "$ROOT_DIR/src/chrome/browser/molt_ai/ios/Entitlements.plist" \
      "$PAYLOAD_DIR/Payload/MoltBrowser.app" 2>/dev/null || true
  fi
fi

# Create IPA zip
cd "$PAYLOAD_DIR"
zip -r -9 "$IPA_PATH" Payload/
cd "$ROOT_DIR"

# Clean up
rm -rf "$PAYLOAD_DIR" "$EXPORT_PLIST"

echo "IPA: $IPA_PATH ($(du -sh "$IPA_PATH" | cut -f1))"

# --- Stage 3: TestFlight upload (if app-store method) ---
if [ "$EXPORT_METHOD" = "app-store" ]; then
  echo ""
  echo "--- TestFlight Upload ---"
  echo "To upload to TestFlight:"
  echo "  xcrun altool --upload-app --type ios --file $IPA_PATH"
  echo "  # Or use Transporter.app"
fi

echo ""
echo "=== iOS Packaging Complete ==="
echo ""
echo "Artifacts in: $DIST_DIR/"
ls -la "$DIST_DIR/"*ios* 2>/dev/null || true
echo ""
echo "Testing:"
echo "  Simulator: xcrun simctl install booted $SIM_DIR.app"
echo "  Device:    ios-deploy --bundle $IPA_PATH"
echo "  TestFlight: xcrun altool --upload-app --type ios --file $IPA_PATH"
