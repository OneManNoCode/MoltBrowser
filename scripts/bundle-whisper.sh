#!/bin/bash
# MoltBrowser Whisper Bundler
# Copyright 2026 GenEye AI Labs Inc.
#
# Builds (or downloads, depending on path availability) whisper.cpp
# and places the `whisper-cli` binary + ggml-tiny.en.bin model file
# inside MoltBrowser.app/Contents/Resources/whisper/. After this
# runs, /tor and /transcribeAudio both work fully offline.
#
# Strategy:
#   1. Check if a system whisper.cpp is already installed and just
#      copy its binary + a tiny model from a cache.
#   2. Otherwise clone whisper.cpp at a pinned tag, build, copy.
#
# Cache: ~/.molt-whisper-cache/
# Bundle layout:
#   MoltBrowser.app/Contents/Resources/whisper/whisper-cli
#   MoltBrowser.app/Contents/Resources/whisper/ggml-tiny.en.bin
#
# Usage:
#   ./scripts/bundle-whisper.sh
#   ./scripts/bundle-whisper.sh --app /path/to/MoltBrowser.app
#   ./scripts/bundle-whisper.sh --whisper-tag v1.7.5

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
CACHE_DIR="$ROOT_DIR/.molt-whisper-cache"
WHISPER_TAG="v1.7.5"
APP_PATH=""

# shellcheck disable=SC1091
source "$SCRIPT_DIR/lib-bundle-verify.sh"

# Pinned commit SHA that the whisper.cpp tag should resolve to. Git
# tags are mutable on the server side — a compromised github.com could
# repoint v1.7.5 to a malicious commit. The commit SHA is content-
# addressed (changing any byte changes the SHA), so verifying we got
# the expected commit forecloses that attack.
#
# To populate: `git rev-parse refs/tags/v1.7.5^{commit}` in a trusted
# checkout, paste the 40-char hex below. Empty = WARNING in output.
EXPECTED_COMMIT_WHISPER_V1_7_5=""

# Pinned SHA256 of ggml-tiny.en.bin from the official HuggingFace
# mirror. Get from the HuggingFace model card or `shasum -a 256` on
# a trusted local copy.
EXPECTED_SHA256_WHISPER_TINY_EN=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --app) APP_PATH="$2"; shift 2 ;;
    --whisper-tag) WHISPER_TAG="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --app PATH            Path to MoltBrowser.app bundle"
      echo "  --whisper-tag TAG     whisper.cpp git tag (default: $WHISPER_TAG)"
      exit 0
      ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

# Resolve .app path.
if [[ -z "$APP_PATH" ]]; then
  for cand in \
    "$CHROMIUM_SRC/out/MoltBrowser/MoltBrowser.app" \
    "$CHROMIUM_SRC/out/Release/MoltBrowser.app" \
    "$ROOT_DIR/dist/MoltBrowser.app"; do
    if [[ -d "$cand" ]]; then APP_PATH="$cand"; break; fi
  done
fi
if [[ -z "$APP_PATH" || ! -d "$APP_PATH" ]]; then
  echo "[bundle-whisper] No MoltBrowser.app found. Build first or pass --app."
  exit 1
fi

DEST="$APP_PATH/Contents/Resources/whisper"
mkdir -p "$DEST"
mkdir -p "$CACHE_DIR"

# Pre-flight: we need git + cmake + curl on PATH. build.sh calls into
# this script silently from the macOS post-build step; a missing dep
# used to show up as a confusing cmake-not-found halfway through the
# clone. Catch it up-front with a clear message. Code-review LOW #17.
_missing=""
for tool in git cmake curl; do
  command -v "$tool" >/dev/null 2>&1 || _missing="$_missing $tool"
done
if [[ -n "$_missing" ]]; then
  echo "[bundle-whisper] FATAL: missing required tools:$_missing"
  echo "[bundle-whisper] On macOS:  brew install$_missing"
  echo "[bundle-whisper] On Linux:  apt-get install$_missing"
  echo "[bundle-whisper] (Build will continue without bundled whisper;"
  echo "[bundle-whisper] /transcribeAudio will fall back to system whisper-cli"
  echo "[bundle-whisper] if any. Re-run scripts/bundle-whisper.sh after install.)"
  exit 1
fi

# Build whisper.cpp at a pinned tag (idempotent — skips if already done).
SRC_DIR="$CACHE_DIR/whisper.cpp-$WHISPER_TAG"
if [[ ! -d "$SRC_DIR" ]]; then
  echo "[bundle-whisper] Cloning whisper.cpp $WHISPER_TAG"
  git clone --depth 1 --branch "$WHISPER_TAG" \
    https://github.com/ggerganov/whisper.cpp "$SRC_DIR"
fi
# Verify the cloned commit matches the pinned SHA. Git tags are
# mutable; commit SHAs are not.
ACTUAL_COMMIT="$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
if [[ -n "$EXPECTED_COMMIT_WHISPER_V1_7_5" ]]; then
  if [[ "$ACTUAL_COMMIT" != "$EXPECTED_COMMIT_WHISPER_V1_7_5" ]]; then
    echo "[bundle-whisper] FATAL: whisper.cpp commit mismatch"
    echo "[bundle-whisper]   expected $EXPECTED_COMMIT_WHISPER_V1_7_5"
    echo "[bundle-whisper]   actual   $ACTUAL_COMMIT"
    echo "[bundle-whisper] either the upstream tag was repointed or the"
    echo "[bundle-whisper] pinned SHA needs updating (verify in a trusted"
    echo "[bundle-whisper] checkout, then paste the new value at the top"
    echo "[bundle-whisper] of $0)."
    exit 1
  fi
  echo "[bundle-whisper] OK: whisper.cpp commit $ACTUAL_COMMIT"
else
  echo "[bundle-whisper] WARNING: no pinned commit SHA for $WHISPER_TAG"
  echo "[bundle-whisper]          actual = $ACTUAL_COMMIT"
  echo "[bundle-whisper]          paste this into EXPECTED_COMMIT_WHISPER_V1_7_5"
  echo "[bundle-whisper]          after verifying in a trusted checkout."
fi

# Use whisper.cpp's CMake build. -DGGML_METAL=1 lets it use Apple
# Silicon's GPU; falls back to CPU on Intel macs.
BUILD_DIR="$SRC_DIR/build"
BIN="$BUILD_DIR/bin/whisper-cli"
if [[ ! -x "$BIN" ]]; then
  echo "[bundle-whisper] Building whisper.cpp (this takes ~2 min)"
  cmake -B "$BUILD_DIR" -S "$SRC_DIR" -DBUILD_SHARED_LIBS=OFF \
        -DGGML_METAL=1 -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR" --target whisper-cli -j 8 >/dev/null
fi
if [[ ! -x "$BIN" ]]; then
  echo "[bundle-whisper] Build did not produce whisper-cli at $BIN"
  exit 1
fi

# Download the tiny.en model (~40 MB) — best size/quality tradeoff
# for "press mic, speak a sentence" UX. Cached, verified, idempotent.
MODEL_CACHE="$CACHE_DIR/ggml-tiny.en.bin"
MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"
if ! verify_or_redownload "$MODEL_CACHE" \
        "$EXPECTED_SHA256_WHISPER_TINY_EN" "$MODEL_URL"; then
  echo "[bundle-whisper] FATAL: model verification failed"
  exit 1
fi

# Install. Strip extended attrs so the bundled binary launches
# without the macOS "downloaded from internet" prompt the first time.
install -m 755 "$BIN" "$DEST/whisper-cli"
install -m 644 "$MODEL_CACHE" "$DEST/ggml-tiny.en.bin"
xattr -d com.apple.quarantine "$DEST/whisper-cli" 2>/dev/null || true

# Record the version we bundled.
echo "$WHISPER_TAG" > "$DEST/.version"

echo "[bundle-whisper] Bundled whisper.cpp $WHISPER_TAG into $DEST"
echo "[bundle-whisper]   binary: $(file "$DEST/whisper-cli" | head -1)"
echo "[bundle-whisper]   model:  $(du -h "$DEST/ggml-tiny.en.bin" | cut -f1)"
