#!/bin/bash
# dev-install-mac.sh — stage the freshly built out-dir app, add the bundled
# payloads + Metal library, sign with per-helper entitlements, gate on a
# headless renderer test, and install into /Applications for local testing.
#
# This is the LOCAL fast path (no notarization, no dmg). Releases still go
# through scripts/release.sh.
#
# Hard-won gotchas encoded here:
#  - Renderer/GPU helpers MUST get allow-jit entitlements or every renderer
#    dies with "V8 process OOM" (Error code 5 on all pages).
#  - Never `codesign --deep` AFTER the per-helper pass (strips entitlements);
#    the --deep base pass goes FIRST to catch bare Mach-O helpers.
#  - default.metallib is wiped from the out-dir framework on EVERY rebuild;
#    without it llama.cpp Metal init fails and NO model can load. We compile
#    it on demand (scripts/build-ggml-metallib.sh) and cache it at
#    build/cache/ggml-metal-<hash of the Metal sources>.metallib, so a
#    llama.cpp update never reuses a metallib built from other kernels.
#  - tor/ocr/whisper/molt_models are packaging-time payloads, not build
#    outputs; carried over from the previously installed app.
#  - A carried-over tesseract can predate its bundled libraries (v0.2.5's
#    loads leptonica etc. from /opt/homebrew, which the helper sandbox
#    refuses: "tesseract exit=...: dyld[...]" in the chat). A helper that
#    loads code from outside the app is re-bundled, every Mach-O under
#    Resources/{tor,ocr,whisper} (ocr/lib/*.dylib included) must sign with the
#    Developer ID, and tesseract has to read a test image under the real
#    helper sandbox before anything is installed.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck disable=SC1091
source "$REPO_DIR/scripts/lib-bundled-helpers.sh"
OUT_APP="$REPO_DIR/chromium/src/out/MoltBrowser/MoltBrowser.app"
STAGE="$(mktemp -d)/MoltBrowser.app"
SIGN_IDENTITY="${SIGN_IDENTITY:-Developer ID Application: GenEye AI Labs Inc. (83DA5U8N7Z)}"
ENT_DIR="$REPO_DIR/chromium/src/chrome/app"
METALLIB_CACHE="$REPO_DIR/build/cache/ggml-metal-$("$REPO_DIR/scripts/build-ggml-metallib.sh" --print-hash).metallib"

echo "[1/7] Stage out-dir app"
ditto "$OUT_APP" "$STAGE"

echo "[2/7] Carry bundled payloads from installed app"
for d in tor ocr whisper molt_models; do
  SRC="/Applications/MoltBrowser.app/Contents/Resources/$d"
  [ -d "$SRC" ] && ditto "$SRC" "$STAGE/Contents/Resources/$d" && echo "  + $d"
done
# ditto merges, and the installed copy may be older than the libraries the
# helper needs: re-bundle one that doesn't load only from the app and the OS.
# bundle-tesseract.sh just copies the Homebrew tesseract (without one, OCR is
# left out); bundle-whisper.sh rebuilds from its cache.
OCR_BIN="$STAGE/Contents/Resources/ocr/tesseract"
if [ ! -f "$OCR_BIN" ] ||
   ! python3 "$REPO_DIR/scripts/macho_bundle_deps.py" --check "$OCR_BIN" \
       --lib-dir "$(dirname "$OCR_BIN")/lib"; then
  echo "  ocr: re-bundling tesseract"
  "$REPO_DIR/scripts/bundle-tesseract.sh" --app "$STAGE"
fi
WHISPER_BIN="$STAGE/Contents/Resources/whisper/whisper-cli"
if [ -f "$WHISPER_BIN" ] &&
   ! python3 "$REPO_DIR/scripts/macho_bundle_deps.py" --check "$WHISPER_BIN" \
       --lib-dir "$(dirname "$WHISPER_BIN")/lib"; then
  echo "  whisper: re-bundling whisper-cli"
  "$REPO_DIR/scripts/bundle-whisper.sh" --app "$STAGE"
fi

echo "[3/7] Ensure default.metallib (llama.cpp Metal)"
# Resources/ of the framework: the dir //third_party/llama_cpp bundles
# ggml-metal-impl.h into (next to kernels/).
FWRES=$(dirname "$(find "$STAGE/Contents/Frameworks" -name "ggml-metal-impl.h" | head -1)")
if [ ! -f "$FWRES/default.metallib" ]; then
  if [ ! -f "$METALLIB_CACHE" ]; then
    echo "  compiling metallib from ggml sources..."
    "$REPO_DIR/scripts/build-ggml-metallib.sh" "$METALLIB_CACHE"
  fi
  cp "$METALLIB_CACHE" "$FWRES/default.metallib"
  echo "  + default.metallib"
fi

echo "[4/7] Sign (inside-out, per-helper entitlements)"
codesign --force --deep --sign "$SIGN_IDENTITY" --options runtime --timestamp "$STAGE" 2>/dev/null
# Resources/{tor,ocr,whisper}: every Mach-O, ocr/lib/*.dylib included.
sign_bundled_helpers "$STAGE" "$SIGN_IDENTITY"
REN=$(find "$STAGE/Contents/Frameworks" -type d -name "*Helper (Renderer).app" | head -1)
for helper in "$(dirname "$REN")/"*.app; do
  case "$(basename "$helper")" in
    *"(Renderer)"*) ENT="$ENT_DIR/helper-renderer-entitlements.plist" ;;
    *"(GPU)"*)      ENT="$ENT_DIR/helper-gpu-entitlements.plist" ;;
    *)              ENT="$ENT_DIR/helper-plugin-entitlements.plist" ;;
  esac
  codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp --entitlements "$ENT" "$helper" 2>/dev/null
done
codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$STAGE/Contents/Frameworks/MoltBrowser Framework.framework/Versions/Current" 2>/dev/null
codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp --entitlements "$ENT_DIR/app-entitlements.plist" "$STAGE" 2>/dev/null
codesign --verify --deep --strict "$STAGE"
codesign -d --entitlements - "$REN" 2>&1 | grep -q allow-jit || { echo "FATAL: renderer lost allow-jit"; exit 1; }
verify_bundled_helper_signatures "$STAGE" || { echo "FATAL: bundled helpers not signed for library validation"; exit 1; }
echo "  signature valid, allow-jit intact, bundled helpers signed"

echo "[5/7] Gates (headless renderer, sandboxed OCR)"
PROF=$(mktemp -d)
( "$STAGE/Contents/MacOS/MoltBrowser" --headless=new --user-data-dir="$PROF" --disable-gpu \
    --no-first-run --use-mock-keychain --dump-dom "data:text/html,<h1>GATE_OK</h1>" > /tmp/devgate.out 2>/dev/null & )
sleep 22
grep -q "GATE_OK" /tmp/devgate.out || { echo "FATAL: renderer gate failed — NOT installing"; pkill -9 -f "$PROF" 2>/dev/null; exit 1; }
pkill -9 -f "$PROF" 2>/dev/null || true
echo "  renderer gate passed"
if [ -f "$OCR_BIN" ]; then
  "$REPO_DIR/tools/ocr_smoke/ocr_smoke_mac.sh" --app "$STAGE" || { echo "FATAL: sandboxed OCR gate failed — NOT installing"; exit 1; }
else
  echo "  no bundled tesseract (brew install tesseract to bundle it): OCR unavailable in this build"
fi

echo "[6/7] Install to /Applications"
osascript -e 'tell application "MoltBrowser" to quit' 2>/dev/null || true
sleep 3; pkill -9 -f "/Applications/MoltBrowser.app" 2>/dev/null || true; sleep 1
rm -rf /Applications/MoltBrowser.app
ditto "$STAGE" /Applications/MoltBrowser.app
xattr -dr com.apple.quarantine /Applications/MoltBrowser.app 2>/dev/null || true

echo "[7/7] Launch"
open /Applications/MoltBrowser.app
echo "DONE — installed and launched."
