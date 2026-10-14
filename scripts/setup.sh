#!/bin/bash
# MoltBrowser Development Setup Script
# Copyright 2025 GenEye AI Labs Inc.
#
# This script sets up the complete development environment for MoltBrowser.
# It handles:
#   1. Checking prerequisites
#   2. Installing depot_tools (Chromium build toolchain)
#   3. Fetching Chromium at the EXACT revision in chromium_version.txt
#      (never tip-of-tree: the overlay only compiles against its pin)
#   4. Syncing DEPS + running hooks for that revision
#   5. Applying the MoltBrowser overlay (src/ -> chromium/src/)
#
# llama.cpp is vendored in src/third_party/llama_cpp and arrives with the
# overlay; nothing is cloned at HEAD.
#
# Usage:
#   ./scripts/setup.sh                          # toolchains for this host OS
#   ./scripts/setup.sh --target-os mac,linux    # shared mac+linux checkout
#   ./scripts/setup.sh --resync                 # move an existing checkout to
#                                               # the pin (discards local edits
#                                               # in chromium/src, then
#                                               # re-applies the overlay)
#   ./scripts/setup.sh --no-overlay             # checkout + sync only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_DIR="$ROOT_DIR/chromium"
CHROMIUM_URL="https://chromium.googlesource.com/chromium/src.git"
TARGET_OS=""
RESYNC=false
APPLY_OVERLAY=true

while [[ $# -gt 0 ]]; do
  case $1 in
    --target-os) TARGET_OS="$2"; shift 2 ;;
    --resync) RESYNC=true; shift ;;
    --no-overlay) APPLY_OVERLAY=false; shift ;;
    -h|--help)
      sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done

CHROMIUM_REVISION="$("$ROOT_DIR/tools/chromium/pin.sh" revision)"
CHROMIUM_VERSION="$("$ROOT_DIR/tools/chromium/pin.sh" version)"

if [ -z "$TARGET_OS" ]; then
  case "$(uname -s)" in
    Darwin) TARGET_OS="mac" ;;
    Linux) TARGET_OS="linux" ;;
    MINGW*|MSYS*|CYGWIN*) TARGET_OS="win" ;;
    *) TARGET_OS="linux" ;;
  esac
fi

echo "=== MoltBrowser Development Setup ==="
echo "Root directory: $ROOT_DIR"
echo "Chromium pin:   $CHROMIUM_VERSION ($CHROMIUM_REVISION)"
echo "target_os:      $TARGET_OS"
echo ""

# Step 1: Check prerequisites
echo "[1/5] Checking prerequisites..."
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 required"; exit 1; }
command -v git >/dev/null 2>&1 || { echo "ERROR: git required"; exit 1; }
# ninja/gn come from depot_tools + the checkout. cmake is only needed to
# build the bundled whisper.cpp on macOS (scripts/bundle-whisper.sh).
command -v cmake >/dev/null 2>&1 || \
  echo "NOTE: cmake not found (only needed for bundle-whisper.sh: brew install cmake)"

# Check disk space (need ~100GB). df -Pk is portable across macOS and Linux.
AVAILABLE_GB=$(( $(df -Pk "$ROOT_DIR" | awk 'NR==2 {print $4}') / 1048576 ))
echo "Available disk space: ${AVAILABLE_GB}GB"
if [ "$AVAILABLE_GB" -lt 80 ]; then
  echo "WARNING: Less than 80GB free. Chromium build needs ~100GB."
  if [ -t 0 ]; then
    echo "Continue? (y/n)"
    read -r response
    if [ "$response" != "y" ]; then exit 1; fi
  fi
fi

# Check RAM
if [ "$(uname)" = "Darwin" ]; then
  RAM_GB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
else
  RAM_GB=$(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1048576 ))
fi
echo "System RAM: ${RAM_GB}GB"
if [ "$RAM_GB" -lt 12 ]; then
  echo "WARNING: Less than 12GB RAM. Build may be slow or fail."
fi

echo "Prerequisites OK."
echo ""

# Step 2: Get depot_tools
echo "[2/5] Setting up depot_tools..."
if [ ! -d "$ROOT_DIR/depot_tools" ]; then
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git \
    "$ROOT_DIR/depot_tools"
fi
export PATH="$ROOT_DIR/depot_tools:$PATH"
echo "depot_tools ready."
echo ""

# Step 3: Fetch Chromium at the pinned revision (shallow, no history).
echo "[3/5] Fetching Chromium $CHROMIUM_VERSION (first run: 30-60 minutes)..."
mkdir -p "$CHROMIUM_DIR"
cd "$CHROMIUM_DIR"
if [ ! -f .gclient ]; then
  TARGET_OS_LIST="$(echo "$TARGET_OS" | sed "s/[^,][^,]*/'&'/g; s/,/, /g")"
  cat > .gclient <<EOF
solutions = [
  {
    "name": "src",
    "url": "$CHROMIUM_URL",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
target_os = [$TARGET_OS_LIST]
EOF
elif ! grep -q "target_os" .gclient; then
  echo "NOTE: chromium/.gclient has no target_os; add e.g. target_os = ['mac', 'linux']"
fi

if [ ! -d src/.git ]; then
  mkdir -p src
  git -C src init -q
  git -C src remote add origin "$CHROMIUM_URL"
fi

CURRENT="$(git -C src rev-parse -q --verify HEAD 2>/dev/null || echo none)"
NEED_SYNC=false
if [ "$CURRENT" != "$CHROMIUM_REVISION" ]; then
  if [ "$CURRENT" != "none" ] && [ "$RESYNC" != true ]; then
    echo ""
    echo "ERROR: chromium/src is at $CURRENT"
    echo "       but chromium_version.txt pins $CHROMIUM_REVISION ($CHROMIUM_VERSION)."
    echo "       Re-run with --resync to move it (discards local changes in"
    echo "       chromium/src; the MoltBrowser overlay is re-applied afterwards)."
    exit 1
  fi
  if [ "$CURRENT" != "none" ]; then
    # Overlay copies make the tree dirty; drop them before switching.
    # clean -fd (no -x) keeps the gitignored out/ build cache.
    git -C src reset -q --hard
    git -C src clean -fdq
  fi
  git -C src fetch --depth=1 --no-tags origin "$CHROMIUM_REVISION"
  git -C src checkout -q --detach FETCH_HEAD
  NEED_SYNC=true
fi
if [ "$(git -C src rev-parse HEAD)" != "$CHROMIUM_REVISION" ]; then
  echo "FATAL: chromium/src is not at the pinned revision after checkout."
  exit 1
fi
[ -f "$CHROMIUM_DIR/.molt-synced" ] && \
  [ "$(cat "$CHROMIUM_DIR/.molt-synced")" = "$CHROMIUM_REVISION" ] || NEED_SYNC=true
echo "chromium/src @ $CHROMIUM_VERSION ($CHROMIUM_REVISION)"
echo ""

# Step 4: Sync DEPS for exactly that revision and run hooks.
echo "[4/5] Syncing Chromium dependencies..."
if [ "$NEED_SYNC" = true ]; then
  # --reset: drops overlay copies left in DEPS repos by an earlier run.
  gclient sync --revision "src@$CHROMIUM_REVISION" --no-history --shallow \
    -D --force --reset --nohooks
  gclient runhooks
  echo "$CHROMIUM_REVISION" > "$CHROMIUM_DIR/.molt-synced"
else
  echo "Already synced at the pin (use --resync to force)."
fi
echo "Dependencies synced."
echo ""

# Step 5: Apply the MoltBrowser overlay.
echo "[5/5] Applying MoltBrowser overlay (src/ -> chromium/src/)..."
if [ "$APPLY_OVERLAY" = true ]; then
  cp -R "$ROOT_DIR/src/." "$CHROMIUM_DIR/src/"
  if [ ! -d "$CHROMIUM_DIR/src/chrome/browser/molt_ai" ]; then
    echo "FATAL: overlay copy failed (chrome/browser/molt_ai missing)."
    exit 1
  fi
  echo "Overlay applied."
else
  echo "Skipped (--no-overlay)."
fi
echo ""

echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Run: ./scripts/configure.sh"
echo "  2. Run: ./scripts/build.sh"
echo ""
