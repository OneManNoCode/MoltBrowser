#!/bin/bash
# MoltBrowser Development Setup Script
# Copyright 2025 GenEye AI Labs Inc.
#
# This script sets up the complete development environment for MoltBrowser.
# It handles:
#   1. Installing depot_tools (Chromium build toolchain)
#   2. Fetching Chromium source
#   3. Cloning llama.cpp
#   4. Running initial dependency sync

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CHROMIUM_DIR="$ROOT_DIR/chromium"

echo "=== MoltBrowser Development Setup ==="
echo "Root directory: $ROOT_DIR"
echo ""

# Step 1: Check prerequisites
echo "[1/5] Checking prerequisites..."
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 required"; exit 1; }
command -v git >/dev/null 2>&1 || { echo "ERROR: git required"; exit 1; }
command -v ninja >/dev/null 2>&1 || { echo "ERROR: ninja required. Install: brew install ninja"; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "ERROR: cmake required. Install: brew install cmake"; exit 1; }

# Check disk space (need ~100GB)
AVAILABLE_GB=$(df -g "$ROOT_DIR" | tail -1 | awk '{print $4}')
echo "Available disk space: ${AVAILABLE_GB}GB"
if [ "$AVAILABLE_GB" -lt 80 ]; then
  echo "WARNING: Less than 80GB free. Chromium build needs ~100GB."
  echo "Continue? (y/n)"
  read -r response
  if [ "$response" != "y" ]; then exit 1; fi
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

# Step 3: Fetch Chromium source
echo "[3/5] Fetching Chromium source (this will take 30-60 minutes)..."
mkdir -p "$CHROMIUM_DIR"
cd "$CHROMIUM_DIR"
if [ ! -d "src" ]; then
  fetch --nohooks --no-history chromium
fi
echo "Chromium source fetched."
echo ""

# Step 4: Sync dependencies
echo "[4/5] Syncing Chromium dependencies..."
cd "$CHROMIUM_DIR/src"
gclient runhooks
echo "Dependencies synced."
echo ""

# Step 5: Clone llama.cpp
echo "[5/5] Setting up llama.cpp..."
LLAMA_DIR="$CHROMIUM_DIR/src/third_party/llama_cpp"
if [ ! -d "$LLAMA_DIR" ]; then
  git clone https://github.com/ggerganov/llama.cpp.git "$LLAMA_DIR"
fi
echo "llama.cpp ready."
echo ""

echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  1. Run: ./scripts/configure.sh"
echo "  2. Run: ./scripts/build.sh"
echo ""
