#!/bin/bash
# MoltBrowser release SBOMs
# Copyright 2026 GenEye AI Labs Inc.
#
# Writes one SPDX 2.3 SBOM per release artifact, next to it, and validates
# it. Prints the SBOM paths, one per line, so callers can add them to the
# checksums, the provenance attestation and the upload:
#
#   mapfile -t sboms < <(./scripts/release-sbom.sh --platform linux \
#       --arch x64 --version "$VERSION" dist/MoltBrowser-Linux-x64.deb ...)
#
# An SBOM is named after its artifact with every "." turned into "-":
# MoltBrowser-Linux-x64.deb -> MoltBrowser-Linux-x64-deb.spdx.json. (The
# in-app updater picks its installer by ".deb", ".dmg", ... in the asset
# name, so an SBOM must not contain those.)
#
# It lists Chromium, every third-party component of the chrome target (from
# the checkout's tools/licenses/licenses.py), MoltBrowser's vendored code and
# the bundled helpers with their pins and SHA-256. Run it on the build
# machine after packaging (and after code signing: it hashes the shipped
# files). With MOLT_RELEASE_BUILD=1 (set by the release scripts and
# workflows) an incomplete SBOM is an error. Details: docs/SBOM.md.
#
# Usage:
#   ./scripts/release-sbom.sh --platform macos|linux|windows --arch ARCH \
#       --version VERSION [--payload DIR] [--out-dir DIR] ARTIFACT...
#
#   --payload   the shipped tree, for listing bundled helpers:
#               MoltBrowser.app on macOS, the staging folder on Windows
#   CHROMIUM_SRC / GN_OUT_DIR override chromium/src and its out dir
#   (out/MoltBrowser, or out/MoltBrowser-linux for --platform linux).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
SBOM_TOOL="$ROOT_DIR/tools/sbom/molt_sbom.py"

PLATFORM=""
ARCH=""
VERSION=""
PAYLOAD=""
OUT_DIR=""
ARTIFACTS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) PLATFORM="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --payload) PAYLOAD="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    -*) echo "release-sbom: unknown option $1" >&2; exit 2 ;;
    *) ARTIFACTS+=("$1"); shift ;;
  esac
done

if [ -z "$PLATFORM" ] || [ -z "$ARCH" ] || [ -z "$VERSION" ] || [ ${#ARTIFACTS[@]} -eq 0 ]; then
  echo "Usage: $0 --platform P --arch A --version V [--payload DIR] [--out-dir DIR] ARTIFACT..." >&2
  exit 2
fi

CHROMIUM_SRC="${CHROMIUM_SRC:-$ROOT_DIR/chromium/src}"
# Linux builds have their own out dir (scripts/configure.sh).
if [ "$PLATFORM" = "linux" ]; then
  GN_OUT_DIR="${GN_OUT_DIR:-$CHROMIUM_SRC/out/MoltBrowser-linux}"
else
  GN_OUT_DIR="${GN_OUT_DIR:-$CHROMIUM_SRC/out/MoltBrowser}"
fi
# licenses.py imports depot_tools modules; depot_tools' python3 (first on
# PATH in the release jobs) is the one Chromium's tools expect.
PYTHON="$(command -v python3)"

ARGS=(generate --platform "$PLATFORM" --arch "$ARCH" --version "$VERSION"
      --chromium-src "$CHROMIUM_SRC" --gn-out-dir "$GN_OUT_DIR"
      --python "$PYTHON")
[ -n "$PAYLOAD" ] && ARGS+=(--payload "$PAYLOAD")
[ -n "$OUT_DIR" ] && ARGS+=(--out-dir "$OUT_DIR")
[ "${MOLT_RELEASE_BUILD:-0}" = "1" ] && ARGS+=(--release)
for a in "${ARTIFACTS[@]}"; do
  ARGS+=(--artifact "$a")
done

mapfile -t SBOMS < <("$PYTHON" "$SBOM_TOOL" "${ARGS[@]}")
if [ ${#SBOMS[@]} -ne ${#ARTIFACTS[@]} ]; then
  echo "release-sbom: expected ${#ARTIFACTS[@]} SBOM(s), got ${#SBOMS[@]}" >&2
  exit 1
fi

VALIDATE=(validate)
for i in "${!SBOMS[@]}"; do
  VALIDATE+=(--artifact "${ARTIFACTS[$i]}")
done
"$PYTHON" "$SBOM_TOOL" "${VALIDATE[@]}" "${SBOMS[@]}" >&2
printf '%s\n' "${SBOMS[@]}"
