#!/bin/bash
# MoltBrowser Tesseract Bundler
# Copyright 2026 GenEye AI Labs Inc.
#
# Bundles a tesseract binary + English traineddata into
# MoltBrowser.app/Contents/Resources/ocr/. The /pdf command's OCR
# fallback path uses this when a downloaded PDF has no text layer.
#
# Strategy: tesseract is a thicket of native deps (leptonica, libpng,
# libtiff, ICU, pango). Building from source inside the build pipeline
# is over-engineering — we lean on the user's Homebrew-installed
# tesseract and copy the binary + dylibs into Resources/ocr/.
# Falls back to downloading just the eng.traineddata if no system
# tesseract is present (the C++ side will keep using "system tesseract"
# in that case).
#
# Bundle layout:
#   MoltBrowser.app/Contents/Resources/ocr/tesseract
#   MoltBrowser.app/Contents/Resources/ocr/tessdata/eng.traineddata
#
# Usage:
#   ./scripts/bundle-tesseract.sh
#   ./scripts/bundle-tesseract.sh --app /path/to/MoltBrowser.app

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
CACHE_DIR="$ROOT_DIR/.molt-tesseract-cache"
APP_PATH=""

# shellcheck disable=SC1091
source "$SCRIPT_DIR/lib-bundle-verify.sh"

# tessdata_fast/eng.traineddata, fetched from a pinned commit of
# tesseract-ocr/tessdata_fast (not the moving main branch) and checked
# against its SHA-256; lib-bundle-verify.sh fails closed on a mismatch.
# To update: pick a new commit, download eng.traineddata from it, and
# replace both values.
TESSDATA_COMMIT="87416418657359cb625c412a48b6e1d6d41c29bd"
EXPECTED_SHA256_TESSDATA_ENG="7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2"

while [[ $# -gt 0 ]]; do
  case $1 in
    --app) APP_PATH="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo "  --app PATH   Path to MoltBrowser.app bundle"
      exit 0
      ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

if [[ -z "$APP_PATH" ]]; then
  for cand in \
    "$CHROMIUM_SRC/out/MoltBrowser/MoltBrowser.app" \
    "$CHROMIUM_SRC/out/Release/MoltBrowser.app" \
    "$ROOT_DIR/dist/MoltBrowser.app"; do
    if [[ -d "$cand" ]]; then APP_PATH="$cand"; break; fi
  done
fi
if [[ -z "$APP_PATH" || ! -d "$APP_PATH" ]]; then
  echo "[bundle-tesseract] No MoltBrowser.app found. Build first or pass --app."
  exit 1
fi

DEST="$APP_PATH/Contents/Resources/ocr"
mkdir -p "$DEST/tessdata"
mkdir -p "$CACHE_DIR"

# Locate a system tesseract (Homebrew first, then standard paths).
SYS_BIN=""
for cand in /opt/homebrew/bin/tesseract /usr/local/bin/tesseract \
            /usr/bin/tesseract /opt/local/bin/tesseract; do
  if [[ -x "$cand" ]]; then SYS_BIN="$cand"; break; fi
done

# Always make sure we have eng.traineddata in the bundle — the user
# might run with the C++ side's "system tesseract" fallback, and we
# still want our bundled traineddata to take precedence so OCR
# quality is consistent across machines.
TRAINEDDATA_CACHE="$CACHE_DIR/eng.traineddata"
TRAINEDDATA_URL="https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/$TESSDATA_COMMIT/eng.traineddata"
# tessdata_fast = best balance of model size (~10MB) vs accuracy
# for general English text. tessdata_best is ~15MB and slower.
if ! verify_or_redownload "$TRAINEDDATA_CACHE" \
        "$EXPECTED_SHA256_TESSDATA_ENG" "$TRAINEDDATA_URL"; then
  echo "[bundle-tesseract] FATAL: traineddata verification failed"
  exit 1
fi
install -m 644 "$TRAINEDDATA_CACHE" "$DEST/tessdata/eng.traineddata"

if [[ -z "$SYS_BIN" ]]; then
  echo "[bundle-tesseract] No system tesseract found; bundled traineddata"
  echo "[bundle-tesseract] is in place but the binary is not. Install with"
  echo "[bundle-tesseract]   brew install tesseract"
  echo "[bundle-tesseract] and re-run this script to bundle it."
  exit 0
fi

# Copy the tesseract binary. We do NOT rewrite its dylib links here —
# that's a notarization-time concern and beyond the scope of dev
# bundling. The bundled binary will still work as long as the user has
# the standard Homebrew dependencies installed (leptonica, libtiff,
# libpng, libjpeg). For DMG distribution we'd run otool + install_name_tool
# to rewrite each LC_LOAD_DYLIB to @executable_path/... — separate batch.
install -m 755 "$SYS_BIN" "$DEST/tesseract"
xattr -d com.apple.quarantine "$DEST/tesseract" 2>/dev/null || true

# Quick sanity check.
"$DEST/tesseract" --version >/dev/null 2>&1 && {
  VER="$("$DEST/tesseract" --version 2>&1 | head -1)"
  echo "[bundle-tesseract] Bundled tesseract: $VER"
} || {
  echo "[bundle-tesseract] Bundled binary fails to run (missing dylibs?)."
  echo "[bundle-tesseract] C++ side will fall through to system tesseract."
}

echo "[bundle-tesseract] Bundled into $DEST"
echo "[bundle-tesseract]   tessdata/eng.traineddata: $(du -h "$DEST/tessdata/eng.traineddata" | cut -f1)"
