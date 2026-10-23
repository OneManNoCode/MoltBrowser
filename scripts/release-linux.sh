#!/bin/bash
# MoltBrowser Linux Release Pipeline
# Copyright 2025 GenEye AI Labs Inc.
#
# Complete Linux release: Build → Package → Upload
# Can run natively on Linux or via Docker from macOS.
#
# Usage:
#   # Native Linux build
#   ./scripts/release-linux.sh --version 0.1.0 --build 1
#
#   # Via Docker (from macOS or any host)
#   ./scripts/release-linux.sh --version 0.1.0 --build 1 --docker
#
#   # Full release with upload
#   ./scripts/release-linux.sh --version 0.1.0 --build 1 --upload --format all

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
# shellcheck source=scripts/lib-release-names.sh disable=SC1091
source "$SCRIPT_DIR/lib-release-names.sh"
# Release builds: bundle downloads must match their pins (no dev override).
export MOLT_RELEASE_BUILD=1

VERSION=""
BUILD_NUM=""
DO_UPLOAD=false
SKIP_BUILD=false
USE_DOCKER=false
FORMAT="all"

while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift 2 ;;
    --build) BUILD_NUM="$2"; shift 2 ;;
    --upload) DO_UPLOAD=true; shift ;;
    --skip-build) SKIP_BUILD=true; shift ;;
    --docker) USE_DOCKER=true; shift ;;
    --format) FORMAT="$2"; shift 2 ;;
    --help|-h)
      echo "MoltBrowser Linux Release Pipeline"
      echo ""
      echo "Usage: $0 --version VERSION --build BUILD_NUM [options]"
      echo ""
      echo "Options:"
      echo "  --version      Version string"
      echo "  --build        Build number"
      echo "  --upload       Upload to GitHub Releases"
      echo "  --skip-build   Skip the build step"
      echo "  --docker       Build inside Docker (works from macOS)"
      echo "  --format       Package format: all, tar, deb, rpm, appimage, flatpak"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

if [ -z "$VERSION" ] || [ -z "$BUILD_NUM" ]; then
  echo "ERROR: --version and --build required"
  exit 1
fi

# A release records its build provenance (scripts/release-provenance.sh).
# Check now, not after the build, that this machine has a builder label
# (MOLT_BUILDER_ID) and the checkout is the clean, tagged source.
if [ "$DO_UPLOAD" = true ]; then
  MOLT_BUILD_STARTED_ON="$("$SCRIPT_DIR/release-provenance.sh" --start --version "$VERSION")"
  export MOLT_BUILD_STARTED_ON
fi

echo "╔══════════════════════════════════════════╗"
echo "║    MoltBrowser Linux Release             ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Version:   $VERSION (build $BUILD_NUM)"
echo "║  Format:    $FORMAT"
echo "║  Docker:    $USE_DOCKER"
echo "║  Upload:    $DO_UPLOAD"
echo "╚══════════════════════════════════════════╝"
echo ""

if [ "$USE_DOCKER" = true ]; then
  # --- Docker-based build ---
  echo "[1/4] Building via Docker..."
  DOCKER_IMG="moltbrowser-linux"

  # Build Docker image if needed
  if [ "$(docker images -q $DOCKER_IMG 2>/dev/null)" = "" ]; then
    docker build \
      --build-arg BUILD_UID="$(id -u)" \
      --build-arg BUILD_GID="$(id -g)" \
      -t "$DOCKER_IMG" \
      -f "$ROOT_DIR/build/docker/Dockerfile.linux" \
      "$ROOT_DIR"
  fi

  DOCKER_ARGS=(--rm -v "$ROOT_DIR:/moltbrowser" -w /moltbrowser)

  if [ "$SKIP_BUILD" = false ]; then
    docker run "${DOCKER_ARGS[@]}" "$DOCKER_IMG" bash -c \
      "./scripts/configure.sh --platform linux --arch x64 && ./scripts/build.sh"
  fi

  echo ""
  echo "[2/4] Packaging..."
  docker run "${DOCKER_ARGS[@]}" "$DOCKER_IMG" bash -c \
    "./scripts/package-linux.sh --version $VERSION --format $FORMAT"

else
  # --- Native build ---
  if [ "$(uname -s)" != "Linux" ]; then
    echo "ERROR: Native Linux build requires Linux. Use --docker from macOS."
    exit 1
  fi

  if [ "$SKIP_BUILD" = false ]; then
    echo "[1/4] Building..."
    "$SCRIPT_DIR/configure.sh" --platform linux --arch x64
    "$SCRIPT_DIR/build.sh"
  else
    echo "[1/4] Skipping build"
  fi

  echo ""
  echo "[2/4] Packaging..."
  "$SCRIPT_DIR/package-linux.sh" --version "$VERSION" --format "$FORMAT"
fi

# --- Copy installer integration files ---
echo ""
echo "[3/4] Copying desktop integration files..."
DIST_DIR="$ROOT_DIR/dist"

# Copy .desktop file, appdata, and man page into dist for inclusion
cp "$ROOT_DIR/installer/linux/moltbrowser.desktop" "$DIST_DIR/" 2>/dev/null || true
cp "$ROOT_DIR/installer/linux/moltbrowser.appdata.xml" "$DIST_DIR/" 2>/dev/null || true
cp "$ROOT_DIR/installer/linux/moltbrowser.1" "$DIST_DIR/" 2>/dev/null || true

# --- Upload ---
echo ""
if [ "$DO_UPLOAD" = true ]; then
  echo "[4/4] Uploading to GitHub Releases..."
  cd "$ROOT_DIR"

  # The packages under the fixed names the in-app updater downloads
  # (package-linux.sh writes them, after removing any older ones).
  ARTIFACTS=()
  for f in "$DIST_DIR/$MOLT_ASSET_LINUX_DEB" "$DIST_DIR/$MOLT_ASSET_LINUX_RPM" \
           "$DIST_DIR/$MOLT_ASSET_LINUX_TAR" "$DIST_DIR/$MOLT_ASSET_LINUX_APPIMAGE"; do
    if [ -f "$f" ]; then ARTIFACTS+=("$f"); fi
  done

  if [ ${#ARTIFACTS[@]} -eq 0 ]; then
    echo "ERROR: No Linux artifacts for $VERSION found in $DIST_DIR/"
    exit 1
  fi

  TAG="v${VERSION}"
  # The UNSIGNED apt/rpm repository, signed offline later
  # (docs/SIGNED_UPDATES.md); not in the checksums, since signing replaces it.
  REPO_PKGS=()
  for f in "${ARTIFACTS[@]}"; do
    case "$f" in *.deb|*.rpm) REPO_PKGS+=("$f") ;; esac
  done
  REPO_BUNDLE="$DIST_DIR/MoltBrowser-Linux-repository.tar"
  rm -f "$REPO_BUNDLE"
  if [ ${#REPO_PKGS[@]} -gt 0 ]; then
    "$SCRIPT_DIR/linux-repo-build.sh" "$REPO_BUNDLE" "${REPO_PKGS[@]}"
  fi
  # One SBOM per package (docs/SBOM.md).
  mapfile -t SBOMS < <("$SCRIPT_DIR/release-sbom.sh" --platform linux --arch x64 \
    --version "$VERSION" "${ARTIFACTS[@]}")
  if [ ${#SBOMS[@]} -ne ${#ARTIFACTS[@]} ]; then
    echo "ERROR: SBOM generation failed"
    exit 1
  fi
  ARTIFACTS+=("${SBOMS[@]}")
  SUMS="$DIST_DIR/SHA256SUMS-linux-x64"
  "$SCRIPT_DIR/release-checksums.sh" "$SUMS" "${ARTIFACTS[@]}"

  # Build provenance (docs/SIGNED_UPDATES.md). A Docker build's toolchain
  # is the container's: probe it there, and record the image.
  PROV_ARGS=(--platform linux --arch x64 --version "$VERSION" --out-dir "$DIST_DIR")
  if [ "$USE_DOCKER" = true ]; then
    FACTS="$(mktemp)"
    docker run "${DOCKER_ARGS[@]}" "$DOCKER_IMG" python3 \
      tools/release_signing/molt_provenance.py probe --platform linux \
      --chromium-src chromium/src > "$FACTS"
    PROV_ARGS+=(--builder-facts "$FACTS" --container-image \
      "$DOCKER_IMG@$(docker image inspect --format '{{.Id}}' "$DOCKER_IMG")")
  fi
  PROV="$("$SCRIPT_DIR/release-provenance.sh" "${PROV_ARGS[@]}" "${ARTIFACTS[@]}" "$SUMS")"
  [ -n "${FACTS:-}" ] && rm -f "$FACTS"

  # DRAFT until the offline-signed manifest is attached (release-draft.sh).
  "$SCRIPT_DIR/release-draft.sh" "$TAG"
  [ -f "$REPO_BUNDLE" ] && ARTIFACTS+=("$REPO_BUNDLE")
  gh release upload "$TAG" "${ARTIFACTS[@]}" "$SUMS" "$PROV" --clobber
  echo "  Uploaded ${#ARTIFACTS[@]} files + $(basename "$SUMS") + $(basename "$PROV") to the draft release $TAG."
else
  echo "[4/4] Skipping upload"
fi

echo ""
echo "=== Linux Release Complete ==="
echo "Artifacts:"
ls -lh "$DIST_DIR/"MoltBrowser-Linux-x64.* 2>/dev/null || echo "  (none)"
