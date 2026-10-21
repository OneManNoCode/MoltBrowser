#!/bin/bash
# MoltBrowser OCR smoke test (macOS)
# Copyright 2026 GenEye AI Labs Inc.
#
# Runs an app's bundled tesseract on ocr_smoke.png the way the AI chat runs it
# on an attached image (molt_ai::ocr::OcrImageWithTesseract ->
# RunSandboxedTesseract -> RunSandboxedHelper): under /usr/bin/sandbox-exec
# with the browser's own Seatbelt profile (sandbox_exec_argv.cc is built from
# seatbelt_profile.cc, so the profile can't drift from the browser's), with
# only HOME, TMPDIR and PATH set, in a private scratch directory, with the same
# tesseract arguments. Then it checks the text that comes back.
#
# It fails on what stops OCR in the app: a tesseract or lib/ dylib that loads
# a library from outside Resources/ocr/ (under the sandbox dyld says "Library
# not loaded ... Operation not permitted"), a lib/ dylib that library
# validation refuses (ad hoc, or another team's, next to a tesseract signed
# with the hardened runtime), missing tessdata, or a sandbox rule that denies
# something tesseract needs.
#
# Usage:
#   tools/ocr_smoke/ocr_smoke_mac.sh                     # /Applications/MoltBrowser.app
#   tools/ocr_smoke/ocr_smoke_mac.sh --app /path/to/MoltBrowser.app
#
# Needs clang++ (Xcode command line tools) and python3. Exits 0 if OCR works,
# 1 if it doesn't, 2 if the app bundles no tesseract or the test can't run.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$HERE/../.." && pwd)"
APP_PATH="/Applications/MoltBrowser.app"
# The second line of ocr_smoke.png.
EXPECTED="sandboxed tesseract reads this"

while [[ $# -gt 0 ]]; do
  case $1 in
    --app) APP_PATH="$2"; shift 2 ;;
    --help|-h)
      echo "Usage: $0 [--app PATH]   (default: $APP_PATH)"
      exit 0
      ;;
    *) echo "Unknown arg: $1"; exit 2 ;;
  esac
done

log() { echo "[ocr-smoke] $*"; }

if [[ "$(uname)" != "Darwin" ]]; then
  log "macOS only (the helper runs under /usr/bin/sandbox-exec)."
  exit 2
fi

OCR_DIR="$APP_PATH/Contents/Resources/ocr"
BIN="$OCR_DIR/tesseract"
TESSDATA="$OCR_DIR/tessdata"
if [[ ! -x "$BIN" ]]; then
  log "$APP_PATH bundles no tesseract (scripts/bundle-tesseract.sh --app ...)."
  exit 2
fi
log "tesseract: $BIN"
log "  $(codesign -dv "$BIN" 2>&1 | grep -E '^(TeamIdentifier|CodeDirectory)' | tr '\n' ' ')"

# 1. Its load commands: nothing from outside Resources/ocr/ but the OS.
if ! python3 "$ROOT_DIR/scripts/macho_bundle_deps.py" --check "$BIN" \
       --lib-dir "$OCR_DIR/lib"; then
  log "FAIL: tesseract loads code from outside the app. Re-bundle it:"
  log "  scripts/bundle-tesseract.sh --app \"$APP_PATH\""
  exit 1
fi

# 2. A private scratch directory holding input.<ext>, as OcrImageWithTesseract
# makes one (base's temp dir, prefix molt_ocr, mode 0700); tesseract writes
# input.txt next to the input.
WORK="$(mktemp -d)"
TMP_ROOT="${TMPDIR:-/tmp}"
SCRATCH="$(mktemp -d "${TMP_ROOT%/}/molt_ocrXXXXXX")"
trap 'rm -rf "$WORK" "$SCRATCH"' EXIT
INPUT="$SCRATCH/input.png"
STEM="$SCRATCH/input"
cp "$HERE/ocr_smoke.png" "$INPUT"

# 3. The browser's sandbox-exec command line for RunSandboxedTesseract:
#   tesseract <input> <stem> -l eng --tessdata-dir <tessdata>
# reading its own directory, tessdata/ and the input; writing SCRATCH.
if ! clang++ -std=c++20 -O1 -I "$ROOT_DIR/src" \
       "$HERE/sandbox_exec_argv.cc" \
       "$ROOT_DIR/src/chrome/browser/molt_ai/common/seatbelt_profile.cc" \
       -o "$WORK/sandbox_exec_argv"; then
  log "could not build sandbox_exec_argv (needs clang++: xcode-select --install)"
  exit 2
fi
ARGV=()
while IFS= read -r -d '' arg; do
  ARGV+=("$arg")
done < <("$WORK/sandbox_exec_argv" "$BIN" "$SCRATCH" \
           "$OCR_DIR" "$TESSDATA" "$INPUT" -- \
           "$INPUT" "$STEM" -l eng --tessdata-dir "$TESSDATA")
if [[ "${ARGV[0]:-}" != "/usr/bin/sandbox-exec" ]]; then
  log "sandbox_exec_argv printed no command line"
  exit 2
fi

# 4. Run it as RunSandboxedHelper does: an environment of HOME, TMPDIR and
# PATH only, the scratch directory as working directory, stdin /dev/null,
# stdout and stderr together.
set +e
( cd "$SCRATCH" &&
  exec env -i HOME="$SCRATCH" TMPDIR="$SCRATCH" PATH=/usr/bin:/bin \
       "${ARGV[@]}" ) </dev/null >"$WORK/output" 2>&1
STATUS=$?
set -e

TEXT=""
if [[ -f "$STEM.txt" ]]; then
  TEXT="$(tr -s '[:space:]' ' ' <"$STEM.txt" | tr '[:upper:]' '[:lower:]')"
fi
if [[ $STATUS -ne 0 || "$TEXT" != *"$EXPECTED"* ]]; then
  log "FAIL: tesseract exit=$STATUS under the helper sandbox. Its output:"
  sed 's/^/    /' "$WORK/output"
  [[ -n "$TEXT" ]] && log "text read: $TEXT"
  log "Sandbox denials, if any:"
  log "  log show --last 5m --style compact --predicate 'sender == \"Sandbox\"'"
  exit 1
fi
log "OK: read \"$EXPECTED\" under the helper sandbox"
