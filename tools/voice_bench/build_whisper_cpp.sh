#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Builds whisper.cpp at the tag and commit scripts/bundle-whisper.sh pins
# (what MoltBrowser ships today), CPU only, for run_bench.py's whisper-cpp
# step: whisper-cli (one process per request, as voice_service.cc runs it)
# and whisper-bench.
#
#   tools/voice_bench/build_whisper_cpp.sh <data dir> [jobs]
#
# Output: <data dir>/whisper.cpp/build/bin/{whisper-cli,whisper-bench}
# The source checkout's models/for-tests-ggml-*.bin are whisper.cpp's own
# vocabulary-only test models: the real shapes and F16 type with no
# weights, which whisper.cpp runs at full cost. run_bench.py times them
# when no real ggml models are given (--ggml-dir).

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <data dir> [jobs]" >&2
  exit 2
fi
DATA="$1"
JOBS="${2:-2}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# The pin lives in scripts/bundle-whisper.sh; read it rather than copy it.
TAG="$(sed -n 's/^WHISPER_TAG="\(.*\)"$/\1/p' "$ROOT_DIR/scripts/bundle-whisper.sh")"
COMMIT="$(sed -n 's/^EXPECTED_COMMIT_WHISPER_V1_7_5="\(.*\)"$/\1/p' \
  "$ROOT_DIR/scripts/bundle-whisper.sh")"
if [[ -z "$TAG" || -z "$COMMIT" ]]; then
  echo "could not read the whisper.cpp pin from scripts/bundle-whisper.sh" >&2
  exit 1
fi

SRC="$DATA/whisper.cpp"
if [[ ! -d "$SRC/.git" ]]; then
  git clone --quiet --depth 1 --branch "$TAG" \
    https://github.com/ggml-org/whisper.cpp "$SRC"
fi
ACTUAL="$(git -C "$SRC" rev-parse HEAD)"
if [[ "$ACTUAL" != "$COMMIT" ]]; then
  echo "whisper.cpp commit mismatch: want $COMMIT, got $ACTUAL" >&2
  exit 1
fi

cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DGGML_METAL=OFF -DGGML_CUDA=OFF -DGGML_VULKAN=OFF \
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_SDL2=OFF >/dev/null
cmake --build "$SRC/build" --config Release -j "$JOBS" \
  --target whisper-cli whisper-bench >/dev/null
echo "built $SRC/build/bin/whisper-cli and whisper-bench (commit $ACTUAL)"
