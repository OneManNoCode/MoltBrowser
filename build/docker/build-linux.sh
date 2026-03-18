#!/bin/bash
# MoltBrowser Linux Build via Docker
# Copyright 2025 GenEye AI Labs Inc.
#
# Builds MoltBrowser for Linux inside a Docker container.
# Works from macOS, Linux, or any Docker-capable host.
#
# Usage:
#   ./build/docker/build-linux.sh                    # Build only
#   ./build/docker/build-linux.sh --package          # Build + package
#   ./build/docker/build-linux.sh --release 0.1.0    # Full release

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_NAME="moltbrowser-linux"
VERSION=""
DO_PACKAGE=false
DO_RELEASE=false

while [[ $# -gt 0 ]]; do
  case $1 in
    --package) DO_PACKAGE=true; shift ;;
    --release) DO_RELEASE=true; VERSION="$2"; shift 2 ;;
    --rebuild-image) REBUILD=true; shift ;;
    --help|-h)
      echo "Usage: $0 [--package] [--release VERSION] [--rebuild-image]"
      exit 0
      ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

# Check Docker
if ! command -v docker &>/dev/null; then
  echo "ERROR: Docker is required. Install Docker Desktop or docker CLI."
  exit 1
fi

# Build Docker image if needed
if [ "$(docker images -q $IMAGE_NAME 2>/dev/null)" = "" ] || [ "$REBUILD" = true ]; then
  echo "=== Building Docker image: $IMAGE_NAME ==="
  docker build \
    --build-arg BUILD_UID=$(id -u) \
    --build-arg BUILD_GID=$(id -g) \
    -t "$IMAGE_NAME" \
    -f "$SCRIPT_DIR/Dockerfile.linux" \
    "$ROOT_DIR"
  echo "Docker image built."
fi

echo "=== Building MoltBrowser for Linux x64 ==="

# Mount the repo, persist the build cache
DOCKER_ARGS=(
  --rm
  -v "$ROOT_DIR:/moltbrowser"
  -w /moltbrowser
  -e HOME=/tmp/builder
)

# Sync Chromium source if needed
if [ ! -d "$ROOT_DIR/chromium/src/base" ]; then
  echo "Syncing Chromium source (this takes a while on first run)..."
  docker run "${DOCKER_ARGS[@]}" "$IMAGE_NAME" bash -c \
    "cd chromium/src && gclient sync --no-history --shallow"
fi

# Configure + Build
docker run "${DOCKER_ARGS[@]}" "$IMAGE_NAME" bash -c \
  "./scripts/configure.sh --platform linux --arch x64 && ./scripts/build.sh"

echo "Build complete."

# Package
if [ "$DO_PACKAGE" = true ] || [ "$DO_RELEASE" = true ]; then
  echo ""
  echo "=== Packaging ==="
  PACKAGE_CMD="./scripts/package-linux.sh"
  [ -n "$VERSION" ] && PACKAGE_CMD="$PACKAGE_CMD --version $VERSION"

  docker run "${DOCKER_ARGS[@]}" "$IMAGE_NAME" bash -c "$PACKAGE_CMD"
  echo ""
  echo "Packages in: $ROOT_DIR/dist/"
  ls -la "$ROOT_DIR/dist/"*linux* 2>/dev/null || echo "(no packages found)"
fi

echo ""
echo "=== Linux Build Complete ==="
