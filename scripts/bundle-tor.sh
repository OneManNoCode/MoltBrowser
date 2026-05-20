#!/bin/bash
# MoltBrowser Tor Bundler
# Copyright 2026 GenEye AI Labs Inc.
#
# Downloads the official Tor Expert Bundle from torproject.org and
# places the tor binary + GeoIP data files inside MoltBrowser.app so
# users never have to `brew install tor`. After this runs, the app is
# self-contained: launch it, run /tor launch in the side panel, and
# the bundled binary spins up.
#
# Bundle layout inside the .app:
#   MoltBrowser.app/Contents/Resources/tor/tor       (binary)
#   MoltBrowser.app/Contents/Resources/tor/geoip     (Tor's GeoIP DB)
#   MoltBrowser.app/Contents/Resources/tor/geoip6    (IPv6 variant)
#
# Cache: ~/.molt-tor-cache/ to avoid re-downloading on every build.
#
# Usage:
#   ./scripts/bundle-tor.sh                       # bundle into built app
#   ./scripts/bundle-tor.sh --app /path/to/MoltBrowser.app
#   ./scripts/bundle-tor.sh --tor-version 14.0.3
#   ./scripts/bundle-tor.sh --arch aarch64        # or x86_64

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
CACHE_DIR="$ROOT_DIR/.molt-tor-cache"

# The Tor Expert Bundle ships with each Tor Browser release. Bumping
# this to a newer minor version is safe; we only consume the `tor`
# binary + geoip data, neither of which has changed shape in years.
TOR_VERSION="14.0.3"
ARCH=""
APP_PATH=""

while [[ $# -gt 0 ]]; do
  case $1 in
    --app) APP_PATH="$2"; shift 2 ;;
    --tor-version) TOR_VERSION="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [OPTIONS]"
      echo ""
      echo "Options:"
      echo "  --app PATH           Path to MoltBrowser.app bundle"
      echo "  --tor-version VER    Tor Expert Bundle version (default: $TOR_VERSION)"
      echo "  --arch ARCH          aarch64 or x86_64 (default: detect host)"
      echo ""
      echo "If --app is not specified, searches common build locations."
      exit 0
      ;;
    *) echo "Unknown arg: $1"; exit 1 ;;
  esac
done

# Auto-detect host arch if not provided. The Tor Expert Bundle uses
# "aarch64" / "x86_64" in URLs.
if [[ -z "$ARCH" ]]; then
  case "$(uname -m)" in
    arm64|aarch64) ARCH="aarch64" ;;
    x86_64) ARCH="x86_64" ;;
    *) echo "Unsupported arch $(uname -m)"; exit 1 ;;
  esac
fi

# Resolve the .app path. Prefer the standard Chromium build output.
if [[ -z "$APP_PATH" ]]; then
  for cand in \
    "$CHROMIUM_SRC/out/MoltBrowser/MoltBrowser.app" \
    "$CHROMIUM_SRC/out/Release/MoltBrowser.app" \
    "$ROOT_DIR/dist/MoltBrowser.app"; do
    if [[ -d "$cand" ]]; then APP_PATH="$cand"; break; fi
  done
fi
if [[ -z "$APP_PATH" || ! -d "$APP_PATH" ]]; then
  echo "[bundle-tor] No MoltBrowser.app found. Build first or pass --app PATH."
  exit 1
fi

TOR_DEST="$APP_PATH/Contents/Resources/tor"
mkdir -p "$TOR_DEST"
mkdir -p "$CACHE_DIR"

ARCHIVE_NAME="tor-expert-bundle-macos-${ARCH}-${TOR_VERSION}.tar.gz"
ARCHIVE_PATH="$CACHE_DIR/$ARCHIVE_NAME"
ARCHIVE_URL="https://archive.torproject.org/tor-package-archive/torbrowser/${TOR_VERSION}/${ARCHIVE_NAME}"

# Download (idempotent).
if [[ ! -f "$ARCHIVE_PATH" ]]; then
  echo "[bundle-tor] Downloading $ARCHIVE_URL"
  if ! curl -fL -o "$ARCHIVE_PATH.tmp" "$ARCHIVE_URL"; then
    echo "[bundle-tor] Download failed. Check Tor version in $0."
    rm -f "$ARCHIVE_PATH.tmp"
    exit 1
  fi
  mv "$ARCHIVE_PATH.tmp" "$ARCHIVE_PATH"
fi

# Extract to a scratch dir we can throw away.
EXTRACT_DIR="$CACHE_DIR/extract-${TOR_VERSION}-${ARCH}"
rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"
tar -xzf "$ARCHIVE_PATH" -C "$EXTRACT_DIR"

# The expert bundle layout is:
#   tor/tor         (the binary)
#   tor/libevent-2.1.7.dylib  (sometimes; statically linked since 14.x)
#   data/geoip
#   data/geoip6
SRC_BIN="$EXTRACT_DIR/tor/tor"
SRC_GEOIP="$EXTRACT_DIR/data/geoip"
SRC_GEOIP6="$EXTRACT_DIR/data/geoip6"

if [[ ! -x "$SRC_BIN" ]]; then
  echo "[bundle-tor] Expected $SRC_BIN to be an executable. Bundle layout changed?"
  ls -lR "$EXTRACT_DIR" | head -50
  exit 1
fi

# Install. We remove the existing copy first so we never end up with
# a stale binary when versions change.
rm -f "$TOR_DEST/tor" "$TOR_DEST/geoip" "$TOR_DEST/geoip6"
install -m 755 "$SRC_BIN" "$TOR_DEST/tor"
[[ -f "$SRC_GEOIP" ]]  && install -m 644 "$SRC_GEOIP"  "$TOR_DEST/geoip"
[[ -f "$SRC_GEOIP6" ]] && install -m 644 "$SRC_GEOIP6" "$TOR_DEST/geoip6"

# Drop the macOS quarantine bit so the bundled binary launches without
# the "downloaded from the internet" prompt the first time. This is
# safe for binaries we just placed ourselves.
xattr -d com.apple.quarantine "$TOR_DEST/tor" 2>/dev/null || true

# Record the version we bundled, useful for support / debug.
echo "$TOR_VERSION" > "$TOR_DEST/.version"

echo "[bundle-tor] Bundled tor ${TOR_VERSION} (${ARCH}) into $TOR_DEST"
echo "[bundle-tor]   binary: $(file "$TOR_DEST/tor" | head -1)"
echo "[bundle-tor]   geoip:  $([ -f "$TOR_DEST/geoip" ]  && wc -l <"$TOR_DEST/geoip"  || echo missing) lines"
echo "[bundle-tor]   geoip6: $([ -f "$TOR_DEST/geoip6" ] && wc -l <"$TOR_DEST/geoip6" || echo missing) lines"
