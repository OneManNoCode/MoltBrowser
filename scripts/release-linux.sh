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
      --build-arg BUILD_UID=$(id -u) \
      --build-arg BUILD_GID=$(id -g) \
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

  # Collect all Linux artifacts
  ARTIFACTS=()
  for f in "$DIST_DIR/"*linux* "$DIST_DIR/"moltbrowser_*; do
    [ -f "$f" ] && ARTIFACTS+=("$f")
  done

  if [ ${#ARTIFACTS[@]} -eq 0 ]; then
    echo "ERROR: No Linux artifacts found in $DIST_DIR/"
    exit 1
  fi

  TAG="v${VERSION}"

  # Check if release exists
  if gh release view "$TAG" &>/dev/null; then
    echo "  Release $TAG exists — uploading additional assets..."
    gh release upload "$TAG" "${ARTIFACTS[@]}" --clobber
  else
    echo "  Creating release $TAG..."
    git tag -f "$TAG" HEAD
    gh release create "$TAG" \
      --title "MoltBrowser v${VERSION}" \
      --notes "Linux release for MoltBrowser v${VERSION}" \
      "${ARTIFACTS[@]}"
  fi
  echo "  Uploaded ${#ARTIFACTS[@]} files."
else
  echo "[4/4] Skipping upload"
fi

echo ""
echo "=== Linux Release Complete ==="
echo "Artifacts:"
ls -lh "$DIST_DIR/"*linux* "$DIST_DIR/"moltbrowser_* 2>/dev/null || echo "  (none)"
