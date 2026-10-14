#!/bin/bash
# MoltBrowser release checksums
# Copyright 2026 GenEye AI Labs Inc.
#
# Writes a SHA256SUMS file for release assets: one "<sha256>  <file name>"
# line per asset, sorted, the format `sha256sum -c` reads. Fails if an
# asset is missing or empty.
#
# Each platform job publishes its own file next to its assets
# (SHA256SUMS-macos-arm64, SHA256SUMS-linux-x64, SHA256SUMS-windows-x64), so
# jobs that upload to the same release in parallel never overwrite each
# other's sums. Users check a download with
#   sha256sum -c --ignore-missing SHA256SUMS-linux-x64     (Linux)
#   shasum -a 256 -c --ignore-missing SHA256SUMS-macos-arm64 (macOS)
# The offline-signed molt-release-manifest.json (tools/release_signing/) is
# what the updater trusts; these sums are for people and mirrors.
#
# Usage:
#   ./scripts/release-checksums.sh dist/SHA256SUMS-linux-x64 dist/MoltBrowser-Linux-x64.*

set -euo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 OUT_FILE ASSET..." >&2
  exit 2
fi
OUT="$1"
shift

if command -v sha256sum >/dev/null 2>&1; then
  sha256() { sha256sum "$1" | awk '{print $1}'; }
else
  sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
for f in "$@"; do
  if [ ! -s "$f" ]; then
    echo "release-checksums: missing or empty asset: $f" >&2
    exit 1
  fi
  printf '%s  %s\n' "$(sha256 "$f")" "$(basename "$f")" >> "$TMP"
done
# Same file name twice would make the sums ambiguous.
if [ -n "$(cut -c67- "$TMP" | sort | uniq -d)" ]; then
  echo "release-checksums: duplicate asset names" >&2
  exit 1
fi
LC_ALL=C sort -k2 "$TMP" > "$OUT"

# Re-hash and compare, so a file that changed underneath us is caught here.
for f in "$@"; do
  want="$(awk -v n="$(basename "$f")" 'substr($0, 67) == n {print $1}' "$OUT")"
  if [ "$(sha256 "$f")" != "$want" ]; then
    echo "release-checksums: $f changed while hashing" >&2
    exit 1
  fi
done
echo "Wrote $OUT:"
cat "$OUT"
