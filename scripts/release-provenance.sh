#!/bin/bash
# MoltBrowser release build provenance
# Copyright 2026 GenEye AI Labs Inc.
#
# Writes the build provenance statement of one platform build:
# molt-provenance-<platform>-<arch>.json, an in-toto Statement with a SLSA
# Provenance v1 predicate (what was built from what, on which builder, with
# which toolchain, when). The owner signs it offline with the release
# manifest (docs/SIGNED_UPDATES.md, "Build provenance").
#
# 1. Before the build (fails now, not hours later, if this machine has no
#    builder label, the tree has uncommitted changes or HEAD isn't the tag):
#
#      MOLT_BUILD_STARTED_ON="$(./scripts/release-provenance.sh --start --version "$VERSION")"
#      export MOLT_BUILD_STARTED_ON
#
#    It prints the start time (or MOLT_BUILD_STARTED_ON, if already set).
#
# 2. After the SBOMs and SHA256SUMS, with the build's release assets:
#
#      prov="$(./scripts/release-provenance.sh --platform linux --arch x64 \
#          --version "$VERSION" --out-dir dist dist/MoltBrowser-Linux-x64.deb ...)"
#
#    It prints the statement's path; upload it with the assets.
#
# Options of step 2:
#   --payload DIR          the shipped app / staging folder (bundled models)
#   --gn-out-dir DIR       out dir the build used (repeatable; default
#                          $GN_OUT_DIR or $CHROMIUM_SRC/out/MoltBrowser)
#   --builder-facts FILE   toolchain JSON from `molt_provenance.py probe`, run
#                          inside the build container
#   --container-image REF  NAME@sha256:<image id> of that container
#   --out-dir DIR          where to write the statement (default: .)
#
# Environment: MOLT_BUILDER_ID (this machine's label, required),
# MOLT_BUILD_STARTED_ON (from step 1), CHROMIUM_SRC, GN_OUT_DIR,
# MOLT_RELEASE_BUILD=1 (anything incomplete is an error). On GitHub Actions
# the run URL is recorded as the invocation id.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
TOOL="$ROOT_DIR/tools/release_signing/molt_provenance.py"
PYTHON="$(command -v python3)"

if [ "${1:-}" = "--start" ]; then
  shift
  exec "$PYTHON" "$TOOL" start "$@"
fi

PLATFORM=""
ARCH=""
VERSION=""
OUT_DIR="."
EXTRA=()
ASSETS=()
GN_OUT_DIRS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform) PLATFORM="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --gn-out-dir) GN_OUT_DIRS+=(--gn-out-dir "$2"); shift 2 ;;
    --payload|--builder-facts|--container-image) EXTRA+=("$1" "$2"); shift 2 ;;
    -h|--help) sed -n '2,38p' "$0"; exit 0 ;;
    -*) echo "release-provenance: unknown option $1" >&2; exit 2 ;;
    *) ASSETS+=("$1"); shift ;;
  esac
done

if [ -z "$PLATFORM" ] || [ -z "$ARCH" ] || [ -z "$VERSION" ] || [ ${#ASSETS[@]} -eq 0 ]; then
  echo "Usage: $0 --platform P --arch A --version V [options] ASSET..." >&2
  exit 2
fi
if [ -z "${MOLT_BUILD_STARTED_ON:-}" ]; then
  echo "release-provenance: MOLT_BUILD_STARTED_ON is not set; run" >&2
  echo "  $0 --start --version $VERSION" >&2
  echo "before the build and export its output." >&2
  exit 1
fi

CHROMIUM_SRC="${CHROMIUM_SRC:-$ROOT_DIR/chromium/src}"
ARGS=(generate --platform "$PLATFORM" --arch "$ARCH" --version "$VERSION"
      --started-on "$MOLT_BUILD_STARTED_ON"
      --finished-on "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
      --chromium-src "$CHROMIUM_SRC" --out-dir "$OUT_DIR")
if [ ${#GN_OUT_DIRS[@]} -gt 0 ]; then
  ARGS+=("${GN_OUT_DIRS[@]}")
fi
if [ ${#EXTRA[@]} -gt 0 ]; then
  ARGS+=("${EXTRA[@]}")
fi
if [ -n "${GITHUB_RUN_ID:-}" ] && [ -n "${GITHUB_REPOSITORY:-}" ]; then
  ARGS+=(--invocation-id "${GITHUB_SERVER_URL:-https://github.com}/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/attempts/${GITHUB_RUN_ATTEMPT:-1}")
fi
[ "${MOLT_RELEASE_BUILD:-0}" = "1" ] && ARGS+=(--release)

"$PYTHON" "$TOOL" "${ARGS[@]}" "${ASSETS[@]}"
