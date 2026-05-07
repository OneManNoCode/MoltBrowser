#!/bin/bash
# Download the bundled AI model (TinyLlama 1.1B Q4_K_M, ~638MB) used by
# package-dmg.sh to ship MoltBrowser with on-device AI working out of the box.
#
# This file is too large for git, so we fetch it on demand. The model is
# the smallest of our supported models so the DMG stays under 1GB.
#
# Usage: ./scripts/download-bundled-model.sh
#
# Copyright 2025 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
MODEL_DIR="$REPO_DIR/branding/models"
MODEL_FILE="$MODEL_DIR/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
MODEL_URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
EXPECTED_SIZE_MB=638  # Approximate

mkdir -p "$MODEL_DIR"

if [ -f "$MODEL_FILE" ]; then
  ACTUAL_SIZE=$(du -h "$MODEL_FILE" | cut -f1)
  echo "Model already present: $MODEL_FILE ($ACTUAL_SIZE)"
  exit 0
fi

echo "Downloading TinyLlama 1.1B Chat (Q4_K_M, ~638MB)..."
echo "Source: $MODEL_URL"
echo ""

curl -L --progress-bar -o "$MODEL_FILE" "$MODEL_URL"

ACTUAL_SIZE=$(du -h "$MODEL_FILE" | cut -f1)
echo ""
echo "Done. Saved to: $MODEL_FILE"
echo "Size: $ACTUAL_SIZE"
