#!/usr/bin/env bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Uploads an OFFLINE-signed release manifest to a draft GitHub release, after
# checking it against the assets actually attached to that release.
# Run on an ONLINE machine; it never needs the signing key.
#
# Usage:
#   scripts/publish-release-signature.sh <tag> <molt-release-manifest.json> <molt-release-manifest.json.sig>
#
# Then publish the release:  gh release edit <tag> --draft=false
# Full runbook: docs/SIGNED_UPDATES.md
set -euo pipefail

if [ $# -ne 3 ]; then
  sed -n '6,13p' "$0" >&2
  exit 2
fi
tag="$1"
manifest="$2"
sig="$3"
repo="${MOLT_REPO:-OneManNoCode/MoltBrowser}"
root="$(cd "$(dirname "$0")/.." && pwd)"
signer="$root/tools/release_signing/molt_release_sign.py"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Downloading the assets attached to $tag ..."
gh release download "$tag" --repo "$repo" --dir "$work"
rm -f "$work/molt-release-manifest.json" "$work/molt-release-manifest.json.sig"

echo "Verifying the signed manifest against those assets and this checkout's trusted keys ..."
python3 "$signer" verify --manifest "$manifest" --sig "$sig" --require-all "$work"/*

echo "Uploading the manifest and signature ..."
gh release upload "$tag" --repo "$repo" --clobber "$manifest" "$sig"

echo
echo "Done. Review the release, then publish it:"
echo "  gh release edit $tag --repo $repo --draft=false"
