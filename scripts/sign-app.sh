#!/bin/bash
# Sign MoltBrowser.app for distribution. Without proper signing, downloaded
# copies trigger "MoltBrowser is damaged" via Gatekeeper. With proper signing
# AND notarization, the app opens with zero warnings.
#
# Usage:
#   ./scripts/sign-app.sh                                                       # ad-hoc sign (free)
#   ./scripts/sign-app.sh --identity "Developer ID Application: ..."            # signed (still warns)
#   ./scripts/sign-app.sh --identity "Developer ID Application: ..." --notarize # signed + notarized
#
# Strategy:
#   1. Find EVERY Mach-O binary anywhere under the .app and sign each one
#      with timestamps + hardened runtime. Catches bare helpers like
#      chrome_crashpad_handler that aren't inside a .app bundle.
#   2. Then sign every nested bundle container (.app, .framework, .xpc)
#      bottom-up via `find -depth`. The bundle signing seals the now-signed
#      inner binaries.
#   3. Finally sign the outer .app bundle.

set -euo pipefail

APP="${APP_PATH:-chromium/src/out/MoltBrowser/MoltBrowser.app}"
IDENTITY="-"          # default to ad-hoc
NOTARIZE=0
ENTITLEMENTS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --identity)     IDENTITY="$2"; shift 2 ;;
    --notarize)     NOTARIZE=1; shift ;;
    --app)          APP="$2"; shift 2 ;;
    --entitlements) ENTITLEMENTS="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

if [[ ! -d "$APP" ]]; then
  echo "ERROR: app not found at $APP"
  exit 1
fi

# Notarization-grade signing requires Apple-issued timestamps + Hardened Runtime
SIGN_FLAGS=(--force --sign "$IDENTITY" --options runtime --timestamp)
if [[ "$IDENTITY" == "-" ]]; then
  # Ad-hoc: no need for timestamps or hardened runtime
  SIGN_FLAGS=(--force --sign "$IDENTITY")
fi
if [[ -n "$ENTITLEMENTS" ]]; then
  SIGN_FLAGS+=(--entitlements "$ENTITLEMENTS")
fi

echo "Signing $APP"
echo "Identity: $IDENTITY"
echo ""

# Step 1: Find ALL Mach-O binaries anywhere under the bundle (recursive,
# regardless of file extension) and sign each one. Catches bare helpers
# like chrome_crashpad_handler, Autoupdate, web_app_shortcut_copier that
# aren't inside .app bundles.
echo "→ Step 1: signing all Mach-O binaries..."
count=0
while IFS= read -r -d '' f; do
  if file "$f" 2>/dev/null | grep -qE "Mach-O|universal binary"; then
    codesign "${SIGN_FLAGS[@]}" "$f" 2>/dev/null || \
      codesign "${SIGN_FLAGS[@]}" "$f"
    count=$((count + 1))
  fi
done < <(find "$APP" -type f -not -path '*.dSYM/*' -print0)
echo "  signed $count standalone Mach-O binaries"

# Step 2: Find every nested bundle container — .app, .framework, .xpc —
# and sign them bottom-up via -depth. This seals the inner binaries we
# just signed into the bundle signature.
echo "→ Step 2: signing nested bundles (.app, .framework, .xpc) bottom-up..."
count=0
while IFS= read -r -d '' b; do
  # Skip the main .app — that's signed last as the outer bundle
  if [[ "$b" == "$APP" ]]; then continue; fi
  codesign "${SIGN_FLAGS[@]}" "$b"
  count=$((count + 1))
done < <(find "$APP" \( -name "*.app" -o -name "*.framework" -o -name "*.xpc" \) -depth -print0)
echo "  signed $count nested bundles"

# Step 3: Sign the outer .app
echo "→ Step 3: signing main bundle..."
codesign "${SIGN_FLAGS[@]}" "$APP"

echo ""
echo "=== Verification ==="
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | tail -3
codesign -dv "$APP" 2>&1 | grep -E "Signature|TeamIdentifier|Identifier|Authority|Timestamp" | head -8

if [[ "$NOTARIZE" == "1" ]]; then
  if [[ "$IDENTITY" == "-" ]]; then
    echo "ERROR: --notarize requires --identity"
    exit 1
  fi
  echo ""
  echo "→ Submitting to Apple notary..."
  ZIP="$(dirname "$APP")/$(basename "$APP" .app)-for-notary.zip"
  rm -f "$ZIP"
  ditto -c -k --keepParent "$APP" "$ZIP"
  xcrun notarytool submit "$ZIP" --keychain-profile AC_PASSWORD --wait
  echo ""
  echo "→ Stapling notarization ticket..."
  xcrun stapler staple "$APP"
  rm -f "$ZIP"
  echo ""
  echo "=== Final verification ==="
  spctl --assess --type execute --verbose "$APP" 2>&1 | tail -3
fi

echo ""
echo "Done. To package the DMG:"
echo "  hdiutil create -volname MoltBrowser -srcfolder $APP -ov -format UDZO dist/MoltBrowser-macOS-arm64.dmg"
