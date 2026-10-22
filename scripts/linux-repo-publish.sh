#!/usr/bin/env bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Publishes the OFFLINE-signed Linux package repository (apt + dnf/yum).
# Run on an ONLINE machine; it never needs the signing key. It verifies the
# signed bundle against the archive key committed in this checkout
# (installer/linux/moltbrowser-archive-keyring.asc) before anything leaves
# the machine.
#
# Usage:
#   scripts/linux-repo-publish.sh --bundle MoltBrowser-Linux-repository.tar --tag vX.Y.Z
#       Verifies it (with the release's .deb), then uploads it to the DRAFT
#       release, replacing the unsigned bundle CI attached. Do this BEFORE
#       scripts/publish-release-signature.sh: the signed release manifest
#       covers the bundle. GitHub Pages serves the repository once the
#       release is published (deploy-website.yml, scripts/linux-repo-site.sh).
#
#   scripts/linux-repo-publish.sh --bundle FILE --assets-dir DIR --to-dir OUT
#       Verifies it and writes the complete tree to OUT, for any other static
#       host (rsync OUT/ to the server). Nothing is uploaded.
#
# MOLT_ARCHIVE_KEY overrides the pinned public key (tests).
# Runbook: docs/SIGNED_UPDATES.md. Users: docs/LINUX_REPOSITORIES.md.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tool="$root/tools/linux_repo/molt_linux_repo.py"
key="${MOLT_ARCHIVE_KEY:-$root/installer/linux/moltbrowser-archive-keyring.asc}"
repo="${MOLT_REPO:-OneManNoCode/MoltBrowser}"

bundle=""
tag=""
assets=""
to_dir=""
while [ $# -gt 0 ]; do
  case "$1" in
    --bundle) bundle="$2"; shift 2 ;;
    --tag) tag="$2"; shift 2 ;;
    --assets-dir) assets="$2"; shift 2 ;;
    --to-dir) to_dir="$2"; shift 2 ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "linux-repo-publish: unknown argument $1" >&2; exit 2 ;;
  esac
done
if [ -z "$bundle" ] || { [ -z "$tag" ] && [ -z "$to_dir" ]; }; then
  sed -n '10,21p' "$0" >&2
  exit 2
fi
if [ ! -f "$key" ]; then
  echo "linux-repo-publish: no archive key at $key." >&2
  echo "Create it offline and commit the PUBLIC key first (docs/SIGNED_UPDATES.md)." >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ -z "$assets" ]; then
  if [ -z "$tag" ]; then
    echo "linux-repo-publish: --to-dir needs --assets-dir (the release's .deb files)" >&2
    exit 2
  fi
  echo "Downloading the .deb assets of $tag ..."
  assets="$work/assets"
  gh release download "$tag" --repo "$repo" --pattern '*.deb' --dir "$assets"
fi

echo "Verifying $bundle against $key ..."
python3 "$tool" unpack --bundle "$bundle" --assets-dir "$assets" \
  --keyring "$key" --out "$work/tree"

if [ -n "$to_dir" ]; then
  mkdir -p "$(dirname "$to_dir")"
  if [ -e "$to_dir" ] && [ -n "$(ls -A "$to_dir")" ]; then
    echo "linux-repo-publish: $to_dir is not empty" >&2
    exit 1
  fi
  rm -rf "$to_dir"
  mv "$work/tree" "$to_dir"
  echo "Verified repository written to $to_dir."
fi

if [ -n "$tag" ]; then
  if [ "$(basename "$bundle")" != "MoltBrowser-Linux-repository.tar" ]; then
    echo "linux-repo-publish: the release asset must be named MoltBrowser-Linux-repository.tar" >&2
    exit 1
  fi
  if [ "$(gh release view "$tag" --repo "$repo" --json isDraft -q .isDraft)" != "true" ]; then
    echo "linux-repo-publish: $tag is already published; its signed manifest would not cover a new bundle." >&2
    exit 1
  fi
  echo "Uploading $(basename "$bundle") to the draft release $tag ..."
  gh release upload "$tag" --repo "$repo" --clobber "$bundle"
  echo
  echo "Done. Next: scripts/publish-release-signature.sh $tag molt-release-manifest.json molt-release-manifest.json.sig"
fi
