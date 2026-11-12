#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# The vendored llama.cpp maps a model file without a call the inference
# process's seccomp policy kills (mmap_under_seccomp.cc): built with
# LLAMA_MOLTBROWSER_SANDBOXED (as BUILD.gn's :llama_sandboxed builds it),
# llama_mmap maps the file; built without it, it is trapped, which shows
# the filter catches what it should. Linux x86_64, any C++17 compiler.
#
# Usage: tools/llama_sandbox/check.sh [out dir]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLAMA="$(cd "$HERE/../../src/third_party/llama_cpp" && pwd)"
OUT="${1:-$(mktemp -d)}"
CXX="${CXX:-g++}"
mkdir -p "$OUT"

build() {
  local out="$1"
  shift
  "$CXX" -std=c++17 -O1 -D_GNU_SOURCE "$@" \
    -I"$LLAMA/include" -I"$LLAMA/ggml/include" -I"$LLAMA/src" \
    "$HERE/mmap_under_seccomp.cc" "$HERE/ggml_stubs.cc" \
    "$LLAMA/src/llama-mmap.cpp" -o "$out"
}

build "$OUT/sandboxed" -DLLAMA_MOLTBROWSER_SANDBOXED
build "$OUT/upstream"

# A file to map: 64 KiB, its first byte "G".
{ printf 'G'; head -c 65535 /dev/zero; } > "$OUT/model.bin"

status=0
"$OUT/sandboxed" "$OUT/model.bin" || status=$?
if [[ $status -ne 0 ]]; then
  echo "FAIL: llama_mmap built for the sandbox was stopped (exit $status)"
  exit 1
fi
status=0
"$OUT/upstream" "$OUT/model.bin" 2>"$OUT/upstream.stderr" || status=$?
if [[ $status -ne 3 ]]; then
  echo "FAIL: upstream llama_mmap wasn't trapped (exit $status): the filter" \
       "no longer catches what the sandbox kills"
  exit 1
fi
echo "ok: built for the sandbox it maps the file; upstream is trapped" \
     "($(cat "$OUT/upstream.stderr"))"
