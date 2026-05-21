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

# Build whisper.cpp at a pinned tag (idempotent — skips if already done).
SRC_DIR="$CACHE_DIR/whisper.cpp-$WHISPER_TAG"
if [[ ! -d "$SRC_DIR" ]]; then
  echo "[bundle-whisper] Cloning whisper.cpp $WHISPER_TAG"
  git clone --depth 1 --branch "$WHISPER_TAG" \
    https://github.com/ggerganov/whisper.cpp "$SRC_DIR"
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
# for "press mic, speak a sentence" UX. Cached, idempotent.
MODEL_CACHE="$CACHE_DIR/ggml-tiny.en.bin"
if [[ ! -f "$MODEL_CACHE" ]]; then
  echo "[bundle-whisper] Downloading ggml-tiny.en.bin model"
  curl -fL -o "$MODEL_CACHE.tmp" \
    https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin
  mv "$MODEL_CACHE.tmp" "$MODEL_CACHE"
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
