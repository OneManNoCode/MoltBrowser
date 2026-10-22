#!/usr/bin/env bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# CI side of the signed Linux package repository: builds the UNSIGNED apt and
# rpm repository from the release's .deb/.rpm and writes the unsigned bundle
# (metadata only; the packages are release assets already). The release
# jobs attach it to the draft release; the owner signs it OFFLINE with
#   tools/linux_repo/molt_linux_repo.py sign
# which replaces it with the signed bundle. Nothing is signed here.
#
# The bundle is not in SHA256SUMS-linux-x64 or the provenance attestation:
# signing replaces it. The offline-signed release manifest covers the signed
# one. Runbook: docs/SIGNED_UPDATES.md.
#
# Usage: scripts/linux-repo-build.sh OUT.tar PACKAGE.deb|PACKAGE.rpm...
set -euo pipefail

if [ $# -lt 2 ]; then
  sed -n '15p' "$0" >&2
  exit 2
fi
out="$1"
shift
root="$(cd "$(dirname "$0")/.." && pwd)"

args=()
for f in "$@"; do
  case "$f" in
    *.deb) args+=(--deb "$f") ;;
    *.rpm) args+=(--rpm "$f") ;;
    *) echo "linux-repo-build: not a .deb or .rpm: $f" >&2; exit 2 ;;
  esac
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 "$root/tools/linux_repo/molt_linux_repo.py" build --out "$work/tree" \
  "${args[@]}" --pack "$out"
