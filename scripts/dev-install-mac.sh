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
#    it on demand and cache at build/cache/default.metallib.
#  - tor/ocr/whisper/molt_models are packaging-time payloads, not build
#    outputs; carried over from the previously installed app.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_APP="$REPO_DIR/chromium/src/out/MoltBrowser/MoltBrowser.app"
STAGE="$(mktemp -d)/MoltBrowser.app"
SIGN_IDENTITY="${SIGN_IDENTITY:-Developer ID Application: GenEye AI Labs Inc. (83DA5U8N7Z)}"
ENT_DIR="$REPO_DIR/chromium/src/chrome/app"
METALLIB_CACHE="$REPO_DIR/build/cache/default.metallib"

echo "[1/7] Stage out-dir app"
ditto "$OUT_APP" "$STAGE"

echo "[2/7] Carry bundled payloads from installed app"
for d in tor ocr whisper molt_models; do
  SRC="/Applications/MoltBrowser.app/Contents/Resources/$d"
  [ -d "$SRC" ] && ditto "$SRC" "$STAGE/Contents/Resources/$d" && echo "  + $d"
done

echo "[3/7] Ensure default.metallib (llama.cpp Metal)"
FWRES=$(dirname "$(find "$STAGE/Contents/Frameworks" -name "ggml-metal.metal" | head -1)")
if [ ! -f "$FWRES/default.metallib" ]; then
  if [ ! -f "$METALLIB_CACHE" ]; then
    echo "  compiling metallib from ggml sources..."
    W=$(mktemp -d)
    cp "$REPO_DIR/src/third_party/llama_cpp/ggml/src/ggml-metal/ggml-metal.metal" \
       "$REPO_DIR/src/third_party/llama_cpp/ggml/src/ggml-common.h" \
       "$REPO_DIR/src/third_party/llama_cpp/ggml/src/ggml-metal/ggml-metal-impl.h" "$W/"
    (cd "$W" && xcrun -sdk macosx metal -O2 -fno-fast-math -I . -c ggml-metal.metal -o ggml-metal.air \
      && xcrun -sdk macosx metallib ggml-metal.air -o default.metallib)
    mkdir -p "$(dirname "$METALLIB_CACHE")"
    cp "$W/default.metallib" "$METALLIB_CACHE"
    rm -rf "$W"
  fi
  cp "$METALLIB_CACHE" "$FWRES/default.metallib"
  echo "  + default.metallib"
fi

echo "[4/7] Sign (inside-out, per-helper entitlements)"
codesign --force --deep --sign "$SIGN_IDENTITY" --options runtime --timestamp "$STAGE" 2>/dev/null
for d in tor ocr whisper; do
  [ -d "$STAGE/Contents/Resources/$d" ] && find "$STAGE/Contents/Resources/$d" -type f | while read -r f; do
    file "$f" | grep -q "Mach-O" && codesign --force --sign "$SIGN_IDENTITY" --options runtime --timestamp "$f" 2>/dev/null || true
  done
done
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
echo "  signature valid, allow-jit intact"

echo "[5/7] Renderer gate (headless)"
PROF=$(mktemp -d)
( "$STAGE/Contents/MacOS/MoltBrowser" --headless=new --user-data-dir="$PROF" --disable-gpu \
    --no-first-run --use-mock-keychain --dump-dom "data:text/html,<h1>GATE_OK</h1>" > /tmp/devgate.out 2>/dev/null & )
sleep 22
grep -q "GATE_OK" /tmp/devgate.out || { echo "FATAL: renderer gate failed — NOT installing"; pkill -9 -f "$PROF" 2>/dev/null; exit 1; }
pkill -9 -f "$PROF" 2>/dev/null || true
echo "  gate passed"

echo "[6/7] Install to /Applications"
osascript -e 'tell application "MoltBrowser" to quit' 2>/dev/null || true
sleep 3; pkill -9 -f "/Applications/MoltBrowser.app" 2>/dev/null || true; sleep 1
rm -rf /Applications/MoltBrowser.app
ditto "$STAGE" /Applications/MoltBrowser.app
xattr -dr com.apple.quarantine /Applications/MoltBrowser.app 2>/dev/null || true

echo "[7/7] Launch"
open /Applications/MoltBrowser.app
echo "DONE — installed and launched."
