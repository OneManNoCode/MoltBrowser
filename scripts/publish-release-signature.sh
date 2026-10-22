#!/usr/bin/env bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Uploads an OFFLINE-signed release manifest and the signed build provenance
# to a draft GitHub release, after checking both against the assets
# actually attached to that release.
# Run on an ONLINE machine; it never needs the signing key.
#
# Usage:
#   scripts/publish-release-signature.sh <tag> <molt-release-manifest.json> <molt-release-manifest.json.sig> <molt-build-provenance.intoto.jsonl>
#
# Then publish the release:  gh release edit <tag> --draft=false
# Full runbook: docs/SIGNED_UPDATES.md
set -euo pipefail

if [ $# -ne 4 ]; then
  sed -n '6,15p' "$0" >&2
  exit 2
fi
tag="$1"
manifest="$2"
sig="$3"
provenance="$4"
repo="${MOLT_REPO:-OneManNoCode/MoltBrowser}"
root="$(cd "$(dirname "$0")/.." && pwd)"
signer="$root/tools/release_signing/molt_release_sign.py"
prov_tool="$root/tools/release_signing/molt_provenance.py"

# gh uploads a file under its own name, and browsers and the verify workflow
# look for these names.
check_name() {
  if [ "$(basename "$1")" != "$2" ]; then
    echo "error: $1 must be named $2" >&2
    exit 1
  fi
}
check_name "$manifest" molt-release-manifest.json
check_name "$sig" molt-release-manifest.json.sig
check_name "$provenance" molt-build-provenance.intoto.jsonl

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Downloading the assets attached to $tag ..."
gh release download "$tag" --repo "$repo" --dir "$work"
rm -f "$work/molt-release-manifest.json" "$work/molt-release-manifest.json.sig" \
  "$work/molt-build-provenance.intoto.jsonl"

echo "Verifying the signed manifest against those assets and this checkout's trusted keys ..."
python3 "$signer" verify --manifest "$manifest" --sig "$sig" --require-all "$work"/*

# The provenance must cover every build output, match it byte for byte, and
# say it was built from the commit the tag points to.
echo "Verifying the signed build provenance ..."
commit="$(gh api "repos/$repo/commits/$tag" --jq .sha)"
python3 "$prov_tool" verify --bundle "$provenance" --tag "$tag" \
  --expect-commit "$commit" --require-all "$work"/*

echo "Uploading the manifest, its signature and the build provenance ..."
gh release upload "$tag" --repo "$repo" --clobber "$manifest" "$sig" "$provenance"

echo
echo "Done. Review the release, then publish it:"
echo "  gh release edit $tag --repo $repo --draft=false"
