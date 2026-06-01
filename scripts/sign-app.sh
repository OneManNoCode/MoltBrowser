#!/bin/bash
# Sign MoltBrowser.app so macOS Gatekeeper does not show "damaged" on
# downloaded copies. Runs the codesign cycle bottom-up: nested helper
# apps → frameworks → main bundle.
#
# Usage:
#   ./scripts/sign-app.sh                                    # ad-hoc sign (free, shows "unidentified developer")
#   ./scripts/sign-app.sh --identity "Developer ID Application: GenEye AI Labs Inc."  # signed
#   ./scripts/sign-app.sh --identity "..." --notarize        # signed + notarized (no warnings)
#
# Without --identity, ad-hoc signing is used. Ad-hoc kills the
# "MoltBrowser is damaged" message but downloaded users still see an
# "unidentified developer" warning and must right-click → Open the first
# time. To skip that warning entirely you need a Developer ID Application
# certificate from developer.apple.com → Certificates IDs & Profiles.

set -euo pipefail

APP="${APP_PATH:-chromium/src/out/MoltBrowser/MoltBrowser.app}"
IDENTITY="-"          # default to ad-hoc
NOTARIZE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --identity) IDENTITY="$2"; shift 2 ;;
    --notarize) NOTARIZE=1; shift ;;
    --app)      APP="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

if [[ ! -d "$APP" ]]; then
  echo "ERROR: app not found at $APP"
  exit 1
fi

echo "Signing $APP with identity: $IDENTITY"
echo ""

# 1. Nested helper apps first
echo "→ Helpers..."
find "$APP/Contents/Frameworks" -name "*.app" -depth | while read -r helper; do
  codesign --force --sign "$IDENTITY" --options runtime --timestamp=none "$helper"
done

# 2. Frameworks
echo "→ Frameworks..."
find "$APP/Contents/Frameworks" -name "*.framework" -maxdepth 2 | while read -r fw; do
  codesign --force --deep --sign "$IDENTITY" --options runtime --timestamp=none "$fw"
done

# 3. Main bundle
echo "→ Main bundle..."
codesign --force --deep --sign "$IDENTITY" --options runtime --timestamp=none "$APP"

echo ""
echo "=== Verification ==="
codesign --verify --deep --strict "$APP"
codesign -dv "$APP" 2>&1 | grep -E "Signature|TeamIdentifier|Identifier|Authority" | head -6

if [[ "$NOTARIZE" == "1" ]]; then
  if [[ "$IDENTITY" == "-" ]]; then
    echo "ERROR: --notarize requires --identity (cannot notarize ad-hoc-signed apps)"
    exit 1
  fi
  echo ""
  echo "→ Notarizing (requires keychain profile 'AC_PASSWORD'; create via:"
  echo "  xcrun notarytool store-credentials AC_PASSWORD --apple-id ... --team-id ... --password ...)"
  ZIP="$(dirname "$APP")/MoltBrowser-for-notarization.zip"
  ditto -c -k --keepParent "$APP" "$ZIP"
  xcrun notarytool submit "$ZIP" --keychain-profile AC_PASSWORD --wait
  xcrun stapler staple "$APP"
  rm -f "$ZIP"
fi

echo ""
echo "Done. Now repackage the DMG:"
echo "  hdiutil create -volname MoltBrowser -srcfolder $APP -ov -format UDZO dist/MoltBrowser-macOS-arm64.dmg"
