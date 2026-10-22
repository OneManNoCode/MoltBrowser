#!/usr/bin/env bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Adds the signed Linux package repository to the GitHub Pages site
# (deploy-website.yml). Takes the newest published, non-prerelease release
# that carries MoltBrowser-Linux-repository.tar, downloads it with the
# release's .deb files, and unpacks it into OUT_DIR only after every
# signature verifies against the archive key committed in this checkout.
# CI never signs anything here: it can only publish what the owner signed
# offline, and a bundle that does not verify fails the deploy (the site
# that is live stays as it is).
#
# Without a committed archive key (installer/linux/
# moltbrowser-archive-keyring.asc) or a release with a bundle, it does
# nothing.
#
# Usage: scripts/linux-repo-site.sh OUT_DIR      (e.g. website/linux)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tool="$root/tools/linux_repo/molt_linux_repo.py"
key="$root/installer/linux/moltbrowser-archive-keyring.asc"
repo="${GITHUB_REPOSITORY:-OneManNoCode/MoltBrowser}"
bundle_name="MoltBrowser-Linux-repository.tar"

if [ $# -ne 1 ]; then
  echo "Usage: $0 OUT_DIR" >&2
  exit 2
fi
out="$1"

if [ ! -f "$key" ]; then
  echo "No archive key committed yet ($key): not publishing a Linux repository."
  exit 0
fi

tag=""
while IFS= read -r t; do
  [ -n "$t" ] || continue
  if gh release view "$t" --repo "$repo" --json assets -q '.assets[].name' \
      | grep -qx "$bundle_name"; then
    tag="$t"
    break
  fi
done < <(gh release list --repo "$repo" --exclude-drafts --exclude-pre-releases \
           --limit 30 --json tagName -q '.[].tagName')

if [ -z "$tag" ]; then
  echo "No published release carries $bundle_name yet: not publishing a Linux repository."
  exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
echo "Linux repository from $tag"
gh release download "$tag" --repo "$repo" --dir "$work/assets" \
  --pattern "$bundle_name" --pattern '*.deb'
python3 "$tool" unpack --bundle "$work/assets/$bundle_name" \
  --assets-dir "$work/assets" --keyring "$key" --out "$work/tree"
mkdir -p "$(dirname "$out")"
rm -rf "$out"
mv "$work/tree" "$out"
echo "Linux repository of $tag verified and staged in $out"
