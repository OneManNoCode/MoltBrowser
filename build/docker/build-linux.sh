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

# We always build/run as linux/amd64 so the prebuilt x86_64 binaries that
# ship with Chromium's depot_tools (python3, gn, ninja, etc.) execute.
# On Apple Silicon hosts this uses Rosetta 2 emulation (slower than native
# but reliable). Without this, the build dies with "Exec format error" the
# first time it tries to run depot_tools/python-bin/python3.
PLATFORM_FLAG="--platform=linux/amd64"

# Build Docker image if needed
if [ "$(docker images -q $IMAGE_NAME 2>/dev/null)" = "" ] || [ "$REBUILD" = true ]; then
  echo "=== Building Docker image: $IMAGE_NAME ==="
  docker build \
    $PLATFORM_FLAG \
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
  $PLATFORM_FLAG
  --rm
  -v "$ROOT_DIR:/moltbrowser"
  -w /moltbrowser
  -e HOME=/tmp/builder
)

# Ensure .gclient declares Linux as a target OS so gclient sync fetches
# Linux-specific binaries (buildtools/linux64/gn, sysroots, etc). Without
# this, syncing on a macOS host only pulls Mac toolchains, and the
# Linux build later dies with "Could not find gn executable".
GCLIENT_FILE="$ROOT_DIR/chromium/.gclient"
if [ -f "$GCLIENT_FILE" ] && ! grep -q "target_os" "$GCLIENT_FILE"; then
  echo "Adding target_os=['linux'] to $GCLIENT_FILE"
  echo "target_os = ['linux']" >> "$GCLIENT_FILE"
fi

# Sync if either: (a) src/base is missing (cold checkout), or
# (b) Linux buildtools are missing (synced on Mac only).
NEED_SYNC=0
[ ! -d "$ROOT_DIR/chromium/src/base" ] && NEED_SYNC=1
[ ! -x "$ROOT_DIR/chromium/src/buildtools/linux64/gn" ] && NEED_SYNC=1

if [ "$NEED_SYNC" = "1" ]; then
  echo "Syncing Chromium source for Linux (takes ~30-60 min on first run)..."
  docker run "${DOCKER_ARGS[@]}" "$IMAGE_NAME" bash -c \
    "cd chromium/src && gclient sync --no-history --shallow"
fi

# Configure + Build
# Cap parallelism with -j 4. Chromium's heaviest C++ files (V8 codegen,
# Blink renderer) can consume 5-8 GB each. With Docker capped at 15.6 GB,
# more than 4 parallel jobs reliably OOMs the container around step
# ~28000 — silently, because Docker kills the container without writing
# to our build log. 4 jobs caps peak memory at ~12 GB and still keeps
# Rosetta CPUs busy.
docker run "${DOCKER_ARGS[@]}" "$IMAGE_NAME" bash -c \
  "./scripts/configure.sh --platform linux --arch x64 && ./scripts/build.sh --jobs 4"

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
