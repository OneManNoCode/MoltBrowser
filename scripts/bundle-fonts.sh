#!/bin/bash
# MoltBrowser: bundle MoltNet's font set next to the browser.
# Copyright 2026 GenEye AI Labs Inc. Licensed under GPLv3.
#
# MoltNet windows draw text only with the fonts MoltBrowser ships, the same
# files on every OS (docs/FINGERPRINTING.md, "Fonts"). The set is
# tools/fonts/moltnet_fonts.json: 47 OFL font files from pinned commits,
# each pinned by size and SHA-256 (like the Tor and model pins), with their
# licences. This script fetches them (cached in build/cache/fonts, or
# $MOLT_FONTS_CACHE), checks every pin, and installs the folder the browser
# looks for (chrome/browser/molt_ai/fingerprinting/molt_net_fonts.h):
#
#   macOS:   MoltBrowser.app/Contents/Resources/molt_fonts/
#   Linux:   <staging>/opt/moltbrowser/molt_fonts/  (next to the executable)
#   Windows: <staging>/molt_fonts/                  (next to MoltBrowser.exe)
#
# The fonts are data: on macOS they are sealed by the app's own signature
# (codesign records every file of Contents/Resources); they need no signing
# of their own and nothing at notarization.
#
# Usage:
#   scripts/bundle-fonts.sh --app PATH/MoltBrowser.app
#   scripts/bundle-fonts.sh --dest DIR          # installs DIR/molt_fonts
#   scripts/bundle-fonts.sh --verify DIR        # checks DIR/molt_fonts
#
# A release build (MOLT_RELEASE_BUILD=1) fails on any unpinned entry or any
# file that doesn't match its pin; without the set, MoltNet would fall back
# to each OS's own fonts. Needs python3 (standard library only).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PYTHON="${PYTHON:-python3}"
FETCH="$ROOT_DIR/tools/fonts/fetch_fonts.py"

DEST=""
VERIFY=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app)
      DEST="$2/Contents/Resources"
      shift 2
      ;;
    --dest)
      DEST="$2"
      shift 2
      ;;
    --verify)
      VERIFY="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,28p' "$0"
      exit 0
      ;;
    *)
      echo "[bundle-fonts] unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [ -n "$VERIFY" ]; then
  exec "$PYTHON" -I "$FETCH" --verify "$VERIFY/molt_fonts"
fi
if [ -z "$DEST" ]; then
  echo "[bundle-fonts] pass --app PATH or --dest DIR" >&2
  exit 2
fi
if [ ! -d "$DEST" ]; then
  echo "[bundle-fonts] $DEST does not exist" >&2
  exit 2
fi

"$PYTHON" -I "$FETCH" --dest "$DEST/molt_fonts"
"$PYTHON" -I "$FETCH" --verify "$DEST/molt_fonts"
echo "[bundle-fonts] MoltNet font set installed in $DEST/molt_fonts" \
     "($(du -sh "$DEST/molt_fonts" | cut -f1))"
