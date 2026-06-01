#!/bin/bash
# Sign MoltBrowser.app for distribution. With proper signing + correct
# entitlements + notarization, the app opens with zero warnings AND
# pages actually render (V8 JIT works, GPU process runs, etc).
#
# Usage:
#   ./scripts/sign-app.sh                                                       # ad-hoc sign (free)
#   ./scripts/sign-app.sh --identity "Developer ID Application: ..."            # signed
#   ./scripts/sign-app.sh --identity "Developer ID Application: ..." --notarize # signed + notarized
#
# Why entitlements matter:
#   Hardened Runtime (--options runtime, required for notarization) blocks
#   JIT compilation and unsigned executable memory by default. Chromium's
#   renderer needs JIT for V8, the GPU process needs JIT for shader
#   compilation, and the plugin process needs unsigned-memory + library-
#   validation-disable for legacy plugins. Without the right entitlements
#   per-helper, pages won't load (renderer can't start), GPU is broken
#   (white window), etc.
#
# Entitlement mapping (matches Chromium's official build):
#   MoltBrowser Helper (Renderer).app → helper-renderer-entitlements.plist
#   MoltBrowser Helper (GPU).app      → helper-gpu-entitlements.plist
#   MoltBrowser Helper (Plugin).app   → helper-plugin-entitlements.plist
#   MoltBrowser Helper.app            → (none, base hardened runtime)
#   MoltBrowser Helper (Alerts).app   → (none)
#   MoltBrowser.app (main bundle)     → app-entitlements.plist

set -euo pipefail

APP="${APP_PATH:-chromium/src/out/MoltBrowser/MoltBrowser.app}"
IDENTITY="-"
NOTARIZE=0
ENT_DIR="chromium/src/chrome/app"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --identity) IDENTITY="$2"; shift 2 ;;
    --notarize) NOTARIZE=1; shift ;;
    --app)      APP="$2"; shift 2 ;;
    --ent-dir)  ENT_DIR="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

if [[ ! -d "$APP" ]]; then
  echo "ERROR: app not found at $APP"
  exit 1
fi

# Base codesign flags. Notarization requires Hardened Runtime + Apple TSA.
BASE_FLAGS=(--force --sign "$IDENTITY" --options runtime --timestamp)
if [[ "$IDENTITY" == "-" ]]; then
  BASE_FLAGS=(--force --sign "$IDENTITY")
fi

# Helper to invoke codesign with optional entitlements
sign_with_ent() {
  local target="$1"; shift
  local ent="${1:-}"
  if [[ -n "$ent" && -f "$ent" ]]; then
    codesign "${BASE_FLAGS[@]}" --entitlements "$ent" "$target"
  else
    codesign "${BASE_FLAGS[@]}" "$target"
  fi
}

# Decide which entitlements file to use based on helper name
entitlements_for() {
  local path="$1"
  case "$path" in
    *"Helper (Renderer)"*) echo "$ENT_DIR/helper-renderer-entitlements.plist" ;;
    *"Helper (GPU)"*)       echo "$ENT_DIR/helper-gpu-entitlements.plist" ;;
    *"Helper (Plugin)"*)    echo "$ENT_DIR/helper-plugin-entitlements.plist" ;;
    *) echo "" ;;
  esac
}

echo "Signing $APP"
echo "Identity: $IDENTITY"
echo "Entitlements dir: $ENT_DIR"
echo ""

# Step 1: Sign all standalone Mach-O binaries that are NOT inside a helper
# .app or .xpc (those get signed via their bundle in step 2). This catches
# bare helpers like chrome_crashpad_handler.
echo "→ Step 1: standalone Mach-O binaries..."
count=0
while IFS= read -r -d '' f; do
  if file "$f" 2>/dev/null | grep -qE "Mach-O|universal binary"; then
    # Skip binaries inside a bundle — those are signed via the bundle
    case "$f" in
      *".app/Contents/MacOS/"*) continue ;;
      *".xpc/Contents/MacOS/"*) continue ;;
      *".framework/Versions/"*"/"*) continue ;;
    esac
    codesign "${BASE_FLAGS[@]}" "$f"
    count=$((count + 1))
  fi
done < <(find "$APP" -type f -not -path '*.dSYM/*' -print0)
echo "  signed $count standalone Mach-O binaries"

# Step 2: Sign nested helper bundles (.app, .xpc) with the right
# entitlements per type. Bottom-up via -depth so inner ones go first.
echo "→ Step 2: helper bundles with per-type entitlements..."
count=0
while IFS= read -r -d '' b; do
  if [[ "$b" == "$APP" ]]; then continue; fi
  ent=$(entitlements_for "$b")
  if [[ -n "$ent" ]]; then
    echo "  $b → $(basename "$ent")"
  fi
  sign_with_ent "$b" "$ent"
  count=$((count + 1))
done < <(find "$APP" \( -name "*.app" -o -name "*.xpc" \) -depth -print0)
echo "  signed $count helper bundles"

# Step 3: Sign framework binaries and frameworks themselves
echo "→ Step 3: frameworks..."
count=0
while IFS= read -r -d '' fw; do
  fwname="$(basename "$fw" .framework)"
  # Sign each version's main binary, then the framework
  for version in "$fw"/Versions/*; do
    [[ -d "$version" ]] || continue
    bin="$version/$fwname"
    if [[ -f "$bin" ]]; then
      codesign "${BASE_FLAGS[@]}" "$bin"
    fi
  done
  codesign "${BASE_FLAGS[@]}" "$fw"
  count=$((count + 1))
done < <(find "$APP" -name "*.framework" -maxdepth 4 -print0)
echo "  signed $count frameworks"

# Step 4: Sign main app bundle with app-entitlements
echo "→ Step 4: main bundle with app-entitlements.plist..."
sign_with_ent "$APP" "$ENT_DIR/app-entitlements.plist"

echo ""
echo "=== Verification ==="
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | tail -3
codesign -dv "$APP" 2>&1 | grep -E "Signature|TeamIdentifier|Identifier|Authority|Timestamp" | head -8
echo ""
echo "Entitlements check on renderer (should include allow-jit):"
codesign -d --entitlements - "$APP/Contents/Frameworks/MoltBrowser Framework.framework/Versions/Current/Helpers/MoltBrowser Helper (Renderer).app" 2>&1 | grep -E "allow-jit|cs.allow" | head -3

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
