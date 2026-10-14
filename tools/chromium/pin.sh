#!/bin/bash
# Prints the Chromium pin from chromium_version.txt, validated.
# Copyright 2026 GenEye AI Labs Inc.
#
# Usage:
#   tools/chromium/pin.sh revision   # 40-hex commit, e.g. for git fetch
#   tools/chromium/pin.sh version    # e.g. 148.0.7729.0
#   tools/chromium/pin.sh env        # CHROMIUM_VERSION=... / CHROMIUM_REVISION=...
#                                    # (append to "$GITHUB_ENV" in workflows)
#
# CHROMIUM_PIN_FILE overrides the file location (tests only).
set -euo pipefail

PIN_FILE="${CHROMIUM_PIN_FILE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/chromium_version.txt}"

read_key() {
  sed -n "s/^$1=//p" "$PIN_FILE" | tr -d '\r[:space:]' | head -n 1
}

[ -f "$PIN_FILE" ] || { echo "pin.sh: $PIN_FILE not found" >&2; exit 1; }
VERSION="$(read_key CHROMIUM_VERSION)"
REVISION="$(read_key CHROMIUM_REVISION)"

if ! [[ "$REVISION" =~ ^[0-9a-f]{40}$ ]]; then
  echo "pin.sh: CHROMIUM_REVISION in $PIN_FILE is not a 40-hex commit: '$REVISION'" >&2
  exit 1
fi
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "pin.sh: CHROMIUM_VERSION in $PIN_FILE is malformed: '$VERSION'" >&2
  exit 1
fi

case "${1:-env}" in
  revision) echo "$REVISION" ;;
  version)  echo "$VERSION" ;;
  env)      printf 'CHROMIUM_VERSION=%s\nCHROMIUM_REVISION=%s\n' "$VERSION" "$REVISION" ;;
  *)        echo "usage: $0 [revision|version|env]" >&2; exit 2 ;;
esac
