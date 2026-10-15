#!/bin/bash
# Build llama.cpp's Metal shader library (default.metallib) from the vendored
# ggml sources. ggml-metal-device.m loads it from the framework's Resources/;
# without it Metal compiles the kernel sources at run time, which fails under
# the hardened runtime, and no on-device model can load.
#
# Upstream split ggml-metal.metal (as vendored since the b11641 update) into
# ggml/src/ggml-metal/kernels/<kind>.metal plus shared headers. Like upstream's
# ggml/src/ggml-metal/CMakeLists.txt (non-embedded build), compile every
# kernel source listed in its METALLIB_KERNEL_SOURCES to .air and link them
# into one default.metallib. The kernels include "common.h" etc. from
# kernels/ and "ggml-metal-impl.h" / "ggml-common.h" from one level up, so the
# work dir mirrors that layout. The optional ggml-tensor.metallib (Metal 4
# tensor API, macOS 26 SDK) is not built; ggml then disables the tensor API,
# as before.
#
# Usage: ./scripts/build-ggml-metallib.sh <output default.metallib>
#        ./scripts/build-ggml-metallib.sh --print-hash
#   --print-hash prints a hash of every input (for cache keys) and exits.
#
# Copyright 2026 GenEye AI Labs Inc. Licensed under GPLv3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
GGML_SRC="$REPO_DIR/src/third_party/llama_cpp/ggml/src"
METAL_SRC="$GGML_SRC/ggml-metal"

# Kernel sources = upstream's METALLIB_KERNEL_SOURCES list.
KERNELS=()
while IFS= read -r k; do
  KERNELS+=("$k")
done < <(sed -n '/^set(METALLIB_KERNEL_SOURCES/,/^)/p' "$METAL_SRC/CMakeLists.txt" \
           | grep -o 'kernels/[A-Za-z0-9_]*\.metal')
if [ "${#KERNELS[@]}" -eq 0 ]; then
  echo "build-ggml-metallib: no METALLIB_KERNEL_SOURCES in $METAL_SRC/CMakeLists.txt" >&2
  exit 1
fi

if [ "${1:-}" = "--print-hash" ]; then
  (cd "$METAL_SRC" && cat CMakeLists.txt ggml-metal-impl.h kernels/*.metal kernels/*.h \
     "$GGML_SRC/ggml-common.h") | shasum -a 256 | cut -c1-16
  exit 0
fi

OUT="${1:?usage: build-ggml-metallib.sh <output default.metallib>}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/kernels" "$WORK/air"
cp "$GGML_SRC/ggml-common.h" "$METAL_SRC/ggml-metal-impl.h" "$WORK/"
cp "$METAL_SRC"/kernels/*.metal "$METAL_SRC"/kernels/*.h "$WORK/kernels/"

AIR=()
for k in "${KERNELS[@]}"; do
  name=$(basename "$k" .metal)
  xcrun -sdk macosx metal -O2 -fno-fast-math -I "$WORK" -c "$WORK/$k" -o "$WORK/air/$name.air"
  AIR+=("$WORK/air/$name.air")
done
xcrun -sdk macosx metallib "${AIR[@]}" -o "$WORK/default.metallib"
mkdir -p "$(dirname "$OUT")"
cp "$WORK/default.metallib" "$OUT"
echo "  default.metallib: ${#AIR[@]} kernel sources -> $OUT"
