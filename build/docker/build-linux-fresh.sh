#!/bin/bash
# build/docker/build-linux-fresh.sh
# Copyright 2025 GenEye AI Labs Inc. — GPLv3
#
# ONE-SHOT cold-start Linux build for a FRESH machine (no prior Chromium
# checkout). Everything runs inside a reproducible Ubuntu 22.04 Docker image,
# so the only host requirements are Docker, disk, RAM, and time.
#
# What it does:
#   [1] Build the build-env image (Dockerfile.linux — all Chromium deps + depot_tools)
#   [2] Cold-fetch Chromium, pinned to the EXACT revision the overlay targets
#   [3] VERIFY the pin — fail fast BEFORE the multi-hour build if it's wrong
#   [4] Apply the MoltBrowser source overlay (repo/src -> chromium/src)
#   [5] VERIFY the overlay landed
#   [6] Configure (GN) + build (autoninja) + optionally package (.deb/.rpm/.tar.gz)
#
# Usage (on the Linux machine, from the repo root):
#   ./build/docker/build-linux-fresh.sh            # build only
#   ./build/docker/build-linux-fresh.sh --package  # build + .deb/.rpm/.tar.gz
#   ./build/docker/build-linux-fresh.sh --package --jobs 8   # more parallelism
#
# Requirements:
#   - x86_64 Linux with Docker (Docker Engine or Docker Desktop)
#   - ~120 GB free disk  (Chromium checkout ~30 GB + build output)
#   - 16 GB+ RAM         (32 GB recommended; each heavy C++ TU can use 5-8 GB)
#   - TIME: first run downloads ~30 GB and compiles for SEVERAL HOURS.
#           Re-runs are incremental and fast.
set -euo pipefail

# ---------------------------------------------------------------------------
# The EXACT Chromium base revision the MoltBrowser overlay is written against.
# Building against any other revision will NOT compile (upstream APIs drift).
# Bump these two together whenever the Mac tree is rebased onto a new Chromium.
# ---------------------------------------------------------------------------
CHROMIUM_PIN="51a413be46f7632ebc3ae331c68af71daa5d6bbf"
CHROMIUM_VERSION="148.0.7729.0"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_NAME="moltbrowser-linux"
DO_PACKAGE=false
VERSION="0.2.5"
JOBS=4
REBUILD=false

while [[ $# -gt 0 ]]; do
  case $1 in
    --package)       DO_PACKAGE=true; shift ;;
    --version)       VERSION="$2"; shift 2 ;;
    --jobs|-j)       JOBS="$2"; shift 2 ;;
    --rebuild-image) REBUILD=true; shift ;;
    -h|--help)
      echo "Usage: $0 [--package] [--version X.Y.Z] [--jobs N] [--rebuild-image]"
      exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

command -v docker >/dev/null 2>&1 || {
  echo "ERROR: Docker is required. Install Docker Engine, then re-run."
  echo "  Ubuntu/Debian: sudo apt-get install -y docker.io && sudo usermod -aG docker \$USER"
  echo "  (log out/in so the group change takes effect)"
  exit 1
}

# We always target linux/amd64 so Chromium's prebuilt x86_64 depot_tools
# binaries run. On a native x86_64 host this is a no-op (no emulation).
PLATFORM_FLAG="--platform=linux/amd64"

# --- Preflight: disk ---
AVAIL_GB="$(df -BG "$ROOT_DIR" | awk 'NR==2 {gsub("G","",$4); print $4}')"
echo "Free disk at $ROOT_DIR: ${AVAIL_GB:-?} GB"
if [ -n "${AVAIL_GB:-}" ] && [ "$AVAIL_GB" -lt 100 ]; then
  echo "WARNING: <100 GB free. A full Chromium checkout + build needs ~120 GB."
fi

# --- [1] Build the build-env image ---
if [ -z "$(docker images -q "$IMAGE_NAME" 2>/dev/null)" ] || [ "$REBUILD" = true ]; then
  echo "=== [1/6] Building Docker build-env image ($IMAGE_NAME) ==="
  docker build $PLATFORM_FLAG \
    --build-arg BUILD_UID="$(id -u)" \
    --build-arg BUILD_GID="$(id -g)" \
    -t "$IMAGE_NAME" \
    -f "$SCRIPT_DIR/Dockerfile.linux" \
    "$ROOT_DIR"
else
  echo "=== [1/6] Reusing existing image $IMAGE_NAME (use --rebuild-image to force) ==="
fi

# Helper: run a command inside the build-env image with the repo mounted.
run_in_docker() {
  docker run --rm $PLATFORM_FLAG \
    -v "$ROOT_DIR:/moltbrowser" -w /moltbrowser -e HOME=/tmp/builder \
    "$IMAGE_NAME" bash -lc "$1"
}

# --- [2] Cold-fetch Chromium pinned to the base revision ---
if [ ! -d "$ROOT_DIR/chromium/src/.git" ]; then
  echo "=== [2/6] Cold-fetching Chromium @ $CHROMIUM_VERSION — ~30 GB, 30-90 min ==="
  run_in_docker "
    set -e
    mkdir -p chromium && cd chromium
    if [ ! -d src ]; then
      # 'fetch' bootstraps the checkout AND writes .gclient. --no-history keeps it shallow.
      fetch --nohooks --no-history chromium
    fi
    # Ensure gclient fetches Linux-specific toolchains/sysroots.
    if ! grep -q \"target_os\" .gclient; then echo \"target_os = ['linux']\" >> .gclient; fi
    cd src
    # Pin to the exact base revision the overlay targets.
    git fetch --depth 1 origin $CHROMIUM_PIN
    git checkout --detach $CHROMIUM_PIN
    cd /moltbrowser/chromium
    # Sync all DEPS sub-repos to match the pinned src, and run hooks.
    gclient sync --no-history --shallow -D --force --reset
    gclient runhooks
  "
else
  echo "=== [2/6] Chromium checkout already present — skipping fetch ==="
fi

# --- [3] VERIFY the pin BEFORE spending hours compiling ---
echo "=== [3/6] Verifying Chromium revision ==="
ACTUAL="$(git -C "$ROOT_DIR/chromium/src" rev-parse HEAD 2>/dev/null || echo none)"
if [ "$ACTUAL" != "$CHROMIUM_PIN" ]; then
  echo ""
  echo "  FATAL: chromium/src is at $ACTUAL"
  echo "         expected      $CHROMIUM_PIN ($CHROMIUM_VERSION)"
  echo "  The overlay only compiles against its pinned base revision."
  echo "  Fix: cd chromium/src && git fetch --depth 1 origin $CHROMIUM_PIN &&"
  echo "       git checkout --detach $CHROMIUM_PIN && (cd .. && gclient sync -D --force)"
  echo "  Aborting now — before the multi-hour build — so no time is wasted."
  exit 1
fi
echo "  OK: chromium/src @ $CHROMIUM_VERSION"

# --- [4] Apply the MoltBrowser overlay ---
echo "=== [4/6] Applying MoltBrowser overlay (src -> chromium/src) ==="
cp -R "$ROOT_DIR/src/." "$ROOT_DIR/chromium/src/"

# --- [5] VERIFY the overlay landed ---
if [ ! -d "$ROOT_DIR/chromium/src/chrome/browser/molt_ai" ]; then
  echo "  FATAL: overlay copy failed — chrome/browser/molt_ai missing in chromium/src."
  exit 1
fi
echo "  OK: overlay present (chrome/browser/molt_ai and friends)"

# --- [6] Configure + build (+ package) ---
echo "=== [5/6] Configure (GN) + build (autoninja, -j $JOBS) — SEVERAL HOURS on first run ==="
run_in_docker "./scripts/configure.sh --platform linux --arch x64 && ./scripts/build.sh --jobs $JOBS"

if [ "$DO_PACKAGE" = true ]; then
  echo "=== [6/6] Packaging (.deb / .rpm / .tar.gz) ==="
  run_in_docker "./scripts/package-linux.sh --version $VERSION"
fi

echo ""
echo "=== Linux build complete ==="
BIN="$ROOT_DIR/chromium/src/out/MoltBrowser/chrome"
[ -f "$BIN" ] && echo "  Binary:   $BIN"
if [ "$DO_PACKAGE" = true ]; then
  echo "  Packages: $ROOT_DIR/dist/"
  ls -la "$ROOT_DIR/dist/"*linux* "$ROOT_DIR/dist/"*.deb "$ROOT_DIR/dist/"*.rpm 2>/dev/null || true
  echo ""
  echo "  Install (Debian/Ubuntu): sudo dpkg -i dist/moltbrowser_${VERSION}_amd64.deb || sudo apt-get -f install -y"
  echo "  Install (Fedora/RHEL):   sudo rpm -i dist/moltbrowser-${VERSION}*.rpm"
  echo "  Portable:                tar xzf dist/MoltBrowser-${VERSION}-linux-x64.tar.gz && ./MoltBrowser*/chrome"
else
  echo "  Run: $BIN"
fi
