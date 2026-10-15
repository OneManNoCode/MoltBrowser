#!/bin/bash
# MoltBrowser Tor Bundler
# Copyright 2026 GenEye AI Labs Inc.
#
# Downloads the official Tor Expert Bundle from torproject.org and
# places the tor binary, GeoIP data and the pluggable transports inside
# MoltBrowser.app. MoltBrowser only ever runs this bundled tor (a
# different binary needs the explicit --moltnet-tor-binary=<path> switch;
# there is no Homebrew/PATH fallback), so this script is the supply chain
# for MoltNet and refuses to bundle an unverified download.
#
# Bundle layout inside the .app:
#   MoltBrowser.app/Contents/Resources/tor/tor       (binary)
#   MoltBrowser.app/Contents/Resources/tor/*.dylib   (libevent etc.)
#   MoltBrowser.app/Contents/Resources/tor/geoip     (Tor's GeoIP DB)
#   MoltBrowser.app/Contents/Resources/tor/geoip6    (IPv6 variant)
#   MoltBrowser.app/Contents/Resources/tor/pluggable_transports/
#       lyrebird           (obfs4 bridges)
#       snowflake-client   (snowflake bridges)
#       pt_config.json     (built-in bridge lines)
#   TorManager exec's the transports relative to Resources/tor/.
#
# Cache: ~/.molt-tor-cache/ to avoid re-downloading on every build.
#
# Usage:
#   ./scripts/bundle-tor.sh                       # bundle into built app
#   ./scripts/bundle-tor.sh --app /path/to/MoltBrowser.app
#   ./scripts/bundle-tor.sh --tor-version 15.0.15
#   ./scripts/bundle-tor.sh --arch aarch64        # or x86_64
#
# Integrity: the tarball MUST match the SHA256 pinned below. With no pin
# (or a --tor-version other than the pinned one) the script FAILS, unless a
# developer explicitly sets MOLT_ALLOW_UNPINNED_TOR=1 (or the repo-wide
# MOLT_ALLOW_UNPINNED_DOWNLOADS=1) for a local build. Both are ignored when
# MOLT_RELEASE_BUILD=1 (set by the release scripts/workflows), so a release
# can never ship an unverified Tor.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_SRC="$ROOT_DIR/chromium/src"
CACHE_DIR="$ROOT_DIR/.molt-tor-cache"

# Pull in shared SHA verification helpers.
# shellcheck disable=SC1091
source "$SCRIPT_DIR/lib-bundle-verify.sh"

# Pinned SHA256 of the Tor Expert Bundle tarball, per (version, arch).
# Tor Project publishes these via their key-signed .sha256sum file
# alongside each release. We DO NOT trust the published .sha256sum
# at download time (a coordinated origin compromise serves bad .tar.gz
# AND bad .sha256sum together). Instead, paste the hash here once,
# verified out-of-band against a key-signed Tor Project release.
#
# To populate (needs network + gpg; do it on a trusted machine):
#   V=15.0.15
#   B=https://archive.torproject.org/tor-package-archive/torbrowser/$V
#   curl -fLO $B/sha256sums-signed-build.txt
#   curl -fLO $B/sha256sums-signed-build.txt.asc
#   gpg --auto-key-locate nodefault,wkd --locate-keys torbrowser@torproject.org
#   gpg --verify sha256sums-signed-build.txt.asc sha256sums-signed-build.txt
#   grep "tor-expert-bundle-macos-aarch64-$V.tar.gz" sha256sums-signed-build.txt
#   grep "tor-expert-bundle-macos-x86_64-$V.tar.gz"  sha256sums-signed-build.txt
# then paste the two hex digests below (and bump PINNED_TOR_VERSION with
# TOR_VERSION). Cross-check one digest against an independent download.
#
# TODO(release): EMPTY ON PURPOSE — these could not be computed in the
# environment that bumped the version (dist/archive.torproject.org were
# unreachable). Until they are filled in, release builds fail here.
PINNED_TOR_VERSION="15.0.15"
EXPECTED_SHA256_TOR_AARCH64=""
EXPECTED_SHA256_TOR_X86_64=""

# The Tor Expert Bundle ships with each Tor Browser release. Kept in step
# with .github/workflows/release-windows-selfhosted.yml ($torVer).
TOR_VERSION="15.0.15"
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

# Pick the right pinned hash for this arch. A version other than the
# pinned one has no pin at all.
EXPECTED_SHA=""
if [[ "$TOR_VERSION" == "$PINNED_TOR_VERSION" ]]; then
  if [[ "$ARCH" == "aarch64" ]]; then
    EXPECTED_SHA="$EXPECTED_SHA256_TOR_AARCH64"
  else
    EXPECTED_SHA="$EXPECTED_SHA256_TOR_X86_64"
  fi
fi

# Refuse to bundle an unverified Tor. Developers can opt out for a local
# build only, and the override is loud.
ALLOW_UNPINNED=0
if [[ "${MOLT_RELEASE_BUILD:-0}" != "1" ]] && \
   { [[ "${MOLT_ALLOW_UNPINNED_TOR:-0}" == "1" ]] || unpinned_allowed; }; then
  ALLOW_UNPINNED=1
fi
if [[ -z "$EXPECTED_SHA" ]]; then
  if [[ "$ALLOW_UNPINNED" == "1" ]]; then
    echo "[bundle-tor] WARNING: unpinned-download override set — bundling an"
    echo "[bundle-tor]          UNVERIFIED tor ${TOR_VERSION} (${ARCH})."
    echo "[bundle-tor]          Local development only. Never ship this."
  else
    echo "[bundle-tor] FATAL: no pinned SHA256 for ${ARCHIVE_NAME}."
    echo "[bundle-tor]        Pin it in scripts/bundle-tor.sh (instructions"
    echo "[bundle-tor]        above EXPECTED_SHA256_TOR_*)."
    if [[ "${MOLT_RELEASE_BUILD:-0}" == "1" ]]; then
      echo "[bundle-tor]        (MOLT_RELEASE_BUILD=1: no override possible.)"
    else
      echo "[bundle-tor]        Dev-only override: MOLT_ALLOW_UNPINNED_TOR=1"
    fi
    exit 1
  fi
fi

# Download with integrity check. verify_or_redownload re-checks the
# cached file on every run, redownloads if tampered locally, and
# fails hard if the fresh download mismatches the pinned SHA. The shared
# lib applies the same unpinned policy; hand it our decision.
if ! MOLT_ALLOW_UNPINNED_DOWNLOADS="$ALLOW_UNPINNED" \
     verify_or_redownload "$ARCHIVE_PATH" "$EXPECTED_SHA" "$ARCHIVE_URL"; then
  echo "[bundle-tor] FATAL: tor bundle verification failed."
  exit 1
fi

# Extract to a scratch dir we can throw away.
EXTRACT_DIR="$CACHE_DIR/extract-${TOR_VERSION}-${ARCH}"
rm -rf "$EXTRACT_DIR"
mkdir -p "$EXTRACT_DIR"
tar -xzf "$ARCHIVE_PATH" -C "$EXTRACT_DIR"

# The expert bundle layout is:
#   tor/tor                    (the binary)
#   tor/libevent-2.1.7.dylib   (REQUIRED — tor 14.x is NOT statically linked;
#                               `otool -L tor` shows @executable_path/
#                               libevent-2.1.7.dylib. Omitting it makes the
#                               bundled tor fail at launch with
#                               "dyld: Library not loaded" and MoltNet never
#                               starts. This was a real shipped bug.)
#   data/geoip
#   data/geoip6
#   tor/pluggable_transports/{lyrebird,snowflake-client,pt_config.json}
#     (location searched below: the layout has moved between releases)
SRC_BIN="$EXTRACT_DIR/tor/tor"
SRC_GEOIP="$EXTRACT_DIR/data/geoip"
SRC_GEOIP6="$EXTRACT_DIR/data/geoip6"

if [[ ! -x "$SRC_BIN" ]]; then
  echo "[bundle-tor] Expected $SRC_BIN to be an executable. Bundle layout changed?"
  ls -lR "$EXTRACT_DIR" | head -50
  exit 1
fi

# Locate the pluggable transports. Bridges (Settings -> MoltNet) need them,
# so a missing transport is fatal for anything but a dev override.
SRC_PT=""
for cand in \
  "$EXTRACT_DIR/tor/pluggable_transports" \
  "$EXTRACT_DIR/pluggable_transports" \
  "$EXTRACT_DIR/tor/pt" \
  "$EXTRACT_DIR/pt"; do
  if [[ -x "$cand/lyrebird" || -x "$cand/snowflake-client" ]]; then
    SRC_PT="$cand"; break
  fi
done
PT_MISSING=""
for _pt in lyrebird snowflake-client; do
  [[ -n "$SRC_PT" && -x "$SRC_PT/$_pt" ]] || PT_MISSING="$PT_MISSING $_pt"
done
[[ -n "$SRC_PT" && -f "$SRC_PT/pt_config.json" ]] || PT_MISSING="$PT_MISSING pt_config.json"
if [[ -n "$PT_MISSING" ]]; then
  if [[ "$ALLOW_UNPINNED" == "1" ]]; then
    echo "[bundle-tor] WARNING: missing pluggable transports:$PT_MISSING"
    echo "[bundle-tor]          (bridges will not work in this dev build)"
  else
    echo "[bundle-tor] FATAL: missing pluggable transports:$PT_MISSING"
    echo "[bundle-tor]        Bundle layout changed? Extracted tree:"
    ls -lR "$EXTRACT_DIR" | head -80
    exit 1
  fi
fi

# Install. We remove the existing copy first so we never end up with
# a stale binary (or stale dylib / transport) when versions change.
rm -f "$TOR_DEST/tor" "$TOR_DEST/geoip" "$TOR_DEST/geoip6" "$TOR_DEST"/*.dylib
rm -rf "$TOR_DEST/pluggable_transports"
install -m 755 "$SRC_BIN" "$TOR_DEST/tor"
# Copy EVERY dylib that ships next to the tor binary (libevent, and whatever
# else a future bundle links against) so @executable_path/ resolves. Verify
# tor's declared dylib deps are all present afterwards or fail loudly.
for _dylib in "$EXTRACT_DIR/tor/"*.dylib; do
  [[ -f "$_dylib" ]] && install -m 755 "$_dylib" "$TOR_DEST/$(basename "$_dylib")"
done
for _need in $(otool -L "$TOR_DEST/tor" 2>/dev/null \
                 | grep -o '@executable_path/[^ ]*\.dylib' \
                 | sed 's|@executable_path/||'); do
  if [[ ! -f "$TOR_DEST/$_need" ]]; then
    echo "[bundle-tor] FATAL: tor needs $_need but it is not in the bundle."
    echo "             The tor binary will fail to launch. Bundle layout changed?"
    exit 1
  fi
done
[[ -f "$SRC_GEOIP" ]]  && install -m 644 "$SRC_GEOIP"  "$TOR_DEST/geoip"
[[ -f "$SRC_GEOIP6" ]] && install -m 644 "$SRC_GEOIP6" "$TOR_DEST/geoip6"
if [[ -n "$SRC_PT" ]]; then
  mkdir -p "$TOR_DEST/pluggable_transports"
  for _pt in lyrebird snowflake-client; do
    [[ -x "$SRC_PT/$_pt" ]] && install -m 755 "$SRC_PT/$_pt" "$TOR_DEST/pluggable_transports/$_pt"
  done
  [[ -f "$SRC_PT/pt_config.json" ]] && install -m 644 "$SRC_PT/pt_config.json" "$TOR_DEST/pluggable_transports/pt_config.json"
fi

# Drop the macOS quarantine bit so the bundled binaries launch without
# the "downloaded from the internet" prompt the first time. This is
# safe for binaries we just placed ourselves.
xattr -dr com.apple.quarantine "$TOR_DEST" 2>/dev/null || true

# Record the version we bundled, useful for support / debug.
echo "$TOR_VERSION" > "$TOR_DEST/.version"

echo "[bundle-tor] Bundled tor ${TOR_VERSION} (${ARCH}) into $TOR_DEST"
echo "[bundle-tor]   binary: $(file "$TOR_DEST/tor" | head -1)"
echo "[bundle-tor]   geoip:  $([ -f "$TOR_DEST/geoip" ]  && wc -l <"$TOR_DEST/geoip"  || echo missing) lines"
echo "[bundle-tor]   geoip6: $([ -f "$TOR_DEST/geoip6" ] && wc -l <"$TOR_DEST/geoip6" || echo missing) lines"
echo "[bundle-tor]   pluggable transports: $(ls "$TOR_DEST/pluggable_transports" 2>/dev/null | tr '\n' ' ')"
