#!/bin/bash
# MoltBrowser: make sure a GitHub release exists for a tag, as a DRAFT.
# Copyright 2026 GenEye AI Labs Inc.
#
# Every release script and workflow uploads into a draft. A release stays a
# draft until the offline-signed manifest and build provenance are attached:
#   molt-release-manifest.json + molt-release-manifest.json.sig
#   molt-build-provenance.intoto.jsonl
# made with tools/release_signing/molt_release_sign.py on the OFFLINE
# signing machine (the in-app updater rejects releases without a valid
# signature) and uploaded by scripts/publish-release-signature.sh. Only then
# publish it, by hand:
#   gh release edit vX.Y.Z --draft=false
#
# The tag must already be pushed (--verify-tag): gh would otherwise create
# it from the default branch, which may not be the commit that was built.
#
# Usage:
#   ./scripts/release-draft.sh vX.Y.Z [--repo OWNER/REPO] [--notes-file FILE]

set -euo pipefail

TAG="${1:-}"
shift || true
REPO_ARGS=()
NOTES_FILE=""
while [[ $# -gt 0 ]]; do
  case $1 in
    --repo) REPO_ARGS=(--repo "$2"); shift 2 ;;
    --notes-file) NOTES_FILE="$2"; shift 2 ;;
    *) echo "release-draft: unknown option $1" >&2; exit 2 ;;
  esac
done

if ! [[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]]; then
  echo "release-draft: '$TAG' is not a release tag (vX.Y.Z[-suffix])" >&2
  exit 1
fi

if gh release view "$TAG" ${REPO_ARGS[@]+"${REPO_ARGS[@]}"} >/dev/null 2>&1; then
  if [ "$(gh release view "$TAG" ${REPO_ARGS[@]+"${REPO_ARGS[@]}"} --json isDraft -q .isDraft)" != "true" ]; then
    echo "WARNING: release $TAG is already PUBLISHED; new uploads change a live"
    echo "         release. Re-sign the manifest so it covers every asset."
  fi
  exit 0
fi

NOTES="DRAFT - do not publish until molt-release-manifest.json, molt-release-manifest.json.sig and molt-build-provenance.intoto.jsonl (signed offline with tools/release_signing/molt_release_sign.py) are attached, and the Linux repository bundle (MoltBrowser-Linux-repository.tar) has been signed offline. Each platform also ships SHA256SUMS-<platform>, an SPDX SBOM per installer (*.spdx.json) and its build provenance statement (molt-provenance-<platform>-<arch>.json; check the signed copy with tools/release_signing/molt_provenance.py verify)."
NOTES_ARGS=(--notes "$NOTES")
[ -n "$NOTES_FILE" ] && NOTES_ARGS=(--notes-file "$NOTES_FILE")
PRE_ARGS=()
[[ "$TAG" == *-* ]] && PRE_ARGS=(--prerelease)

echo "Creating DRAFT release $TAG"
# Parallel jobs may race to create it; losing the race is fine.
gh release create "$TAG" ${REPO_ARGS[@]+"${REPO_ARGS[@]}"} --draft --verify-tag \
  --title "MoltBrowser $TAG" "${NOTES_ARGS[@]}" ${PRE_ARGS[@]+"${PRE_ARGS[@]}"} ||
  gh release view "$TAG" ${REPO_ARGS[@]+"${REPO_ARGS[@]}"} >/dev/null
