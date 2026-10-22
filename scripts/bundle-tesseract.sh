#!/bin/bash
# MoltBrowser Tesseract Bundler
# Copyright 2026 GenEye AI Labs Inc.
#
# Bundles a tesseract binary + English traineddata into
# MoltBrowser.app/Contents/Resources/ocr/. The /pdf command's OCR
# fallback path uses this when a downloaded PDF has no text layer.
#
# Strategy: tesseract is a thicket of native deps (leptonica, libpng,
# libtiff, libarchive, ...). Building from source inside the build pipeline
# is over-engineering — we take the build machine's Homebrew (or MacPorts)
# tesseract and copy the binary + every non-system dylib it loads into
# Resources/ocr/, rewritten to load from there (macho_bundle_deps.py). The
# app never runs a tesseract from outside its bundle on macOS, and the
# helper sandbox only lets it map code from Resources/ocr/ and the OS.
# Without a tesseract on the build machine, only eng.traineddata is bundled
# and OCR is unavailable.
#
# Bundle layout:
#   MoltBrowser.app/Contents/Resources/ocr/tesseract
#   MoltBrowser.app/Contents/Resources/ocr/lib/*.dylib
#   MoltBrowser.app/Contents/Resources/ocr/tessdata/eng.traineddata
#   MoltBrowser.app/Contents/Resources/ocr/.version   (tesseract's version)
#   MoltBrowser.app/Contents/Resources/ocr/.bundle-provenance.json
#     (where the binary and each library were copied from; the release SBOM,
#     tools/sbom/molt_sbom.py, reads both)
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

if [[ "$(uname)" != "Darwin" ]]; then
  echo "[bundle-tesseract] macOS only (Linux and Windows use their own packaging)."
  exit 1
fi

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

# Never leave a half-bundled tesseract behind: the app would find it and
# fail at launch (or load a library from outside the bundle).
remove_bundled_binary() {
  rm -rf "${DEST:?}/tesseract" "${DEST:?}/lib" "${DEST:?}/.version" \
    "${DEST:?}/.bundle-provenance.json"
}

if [[ -z "$SYS_BIN" ]]; then
  remove_bundled_binary
  echo "[bundle-tesseract] No tesseract on this machine; bundled traineddata"
  echo "[bundle-tesseract] is in place but the binary is not, so OCR is"
  echo "[bundle-tesseract] unavailable in this build. Install with"
  echo "[bundle-tesseract]   brew install tesseract"
  echo "[bundle-tesseract] and re-run this script to bundle it."
  exit 0
fi

# Copy the binary and the dylibs it loads (recursively, OS libraries
# aside) into lib/, rewrite every load command to point there, drop the
# LC_RPATHs and re-sign ad hoc. Fails if anything still loads from outside
# the bundle. Release signing later signs each file with the Developer ID
# (release.sh / sign-app.sh sign every Mach-O under Resources/ocr/).
remove_bundled_binary
install -m 755 "$SYS_BIN" "$DEST/tesseract"
if ! python3 "$SCRIPT_DIR/macho_bundle_deps.py" "$DEST/tesseract" \
        --lib-dir "$DEST/lib" --origin "$SYS_BIN" \
        --provenance "$DEST/.bundle-provenance.json"; then
  remove_bundled_binary
  echo "[bundle-tesseract] FATAL: could not make the bundled tesseract"
  echo "[bundle-tesseract] self-contained (see above); not bundling it."
  exit 1
fi
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true

# Sanity check: it runs from the bundle.
if ! "$DEST/tesseract" --version >/dev/null 2>&1; then
  remove_bundled_binary
  echo "[bundle-tesseract] FATAL: the bundled tesseract does not run; not bundling it."
  exit 1
fi
TESS_VERSION_LINE="$("$DEST/tesseract" --version 2>&1 | head -1)"
# "tesseract 5.5.0" -> 5.5.0, recorded for the release SBOM.
echo "${TESS_VERSION_LINE#tesseract }" > "$DEST/.version"
echo "[bundle-tesseract] Bundled tesseract: $TESS_VERSION_LINE"

echo "[bundle-tesseract] Bundled into $DEST"
echo "[bundle-tesseract]   lib/: $(find "$DEST/lib" -type f 2>/dev/null | wc -l | tr -d ' ') dylibs, $(du -sh "$DEST/lib" 2>/dev/null | cut -f1)"
echo "[bundle-tesseract]   tessdata/eng.traineddata: $(du -h "$DEST/tessdata/eng.traineddata" | cut -f1)"
