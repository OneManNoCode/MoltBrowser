#!/bin/bash
# MoltBrowser voice mode, end to end, on a CI machine (docs/VOICE_E2E.md)
# Copyright 2026 GenEye AI Labs Inc. Licensed under GPLv3.
#
#   tools/voice_e2e/ci_run.sh <out dir> <bundle dir> <report dir>
#
#   <out dir>     the build's output directory, with the chrome binary
#   <bundle dir>  scripts/bundle-speech.sh's output: speech/ (molt-speech
#                 and its models) and voice/ (molt-voice), for example
#                 <cache>/build/linux-x64 after --build-only --parts
#                 speech,voice; copied next to the browser
#   <report dir>  where report.json, report.md, the rendered speech, the
#                 speech check and every scenario's logs and screenshots go
#
# On Ubuntu it installs what it needs (Xvfb, PulseAudio, fonts, openssl,
# cmake and a compiler for the voice bench's driver, numpy pinned by hash),
# lets the browser's and the helpers' sandboxes make user namespaces, fetches
# the pinned voices (tools/voice_bench/fetch.py), renders the scenarios'
# speech, checks it against the bundle's recognizer, then runs the suite:
# PulseAudio in quiet and with music, Chromium's file capture, and typed.
#
# Environment:
#   VOICE_E2E_RUNS          the runs (default
#                           pulse:clean,pulse:music10,file:clean,typed)
#   VOICE_E2E_SCENARIOS     scenario ids to run (default all)
#   VOICE_E2E_DATA          the voices' download directory (default
#                           ~/.cache/moltbrowser-voice-e2e)
#   VOICE_E2E_SPEECH_CHECK  0 to skip the recognizer check (default 1)
#   VOICE_E2E_MODEL         1 when an on-device AI model is installed
#
# Exit status: the runner's (0 Phase 1's exit met, 1 not met, 2 couldn't
# run); the reports are left either way.

set -euo pipefail

if [[ $# -ne 3 ]]; then
  sed -n '2,32p' "$0"
  exit 2
fi
OUT_DIR="$(cd "$1" && pwd)"
BUNDLE_DIR="$(cd "$2" && pwd)"
mkdir -p "$3"
REPORT_DIR="$(cd "$3" && pwd)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DATA_DIR="${VOICE_E2E_DATA:-$HOME/.cache/moltbrowser-voice-e2e}"
RUNS="${VOICE_E2E_RUNS:-pulse:clean,pulse:music10,file:clean,typed}"

log() { echo "[voice-e2e] $*"; }

BROWSER="$OUT_DIR/chrome"
[[ -x "$BROWSER" ]] || { log "no browser at $BROWSER"; exit 2; }
[[ -x "$BUNDLE_DIR/speech/molt-speech" ]] || { log "no molt-speech in $BUNDLE_DIR/speech"; exit 2; }

SUDO=""
if [[ "$(id -u)" -ne 0 ]] && command -v sudo >/dev/null 2>&1; then
  SUDO="sudo"
fi

# ---------------------------------------------------------------- packages
need=()
for pair in Xvfb:xvfb pulseaudio:pulseaudio pactl:pulseaudio-utils openssl:openssl \
            cmake:cmake cc:build-essential fc-list:fontconfig; do
  command -v "${pair%%:*}" >/dev/null 2>&1 || need+=("${pair#*:}")
done
[[ -d /usr/share/fonts/truetype/dejavu ]] || need+=(fonts-dejavu-core)
if [[ ${#need[@]} -gt 0 ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    log "installing ${need[*]}"
    $SUDO apt-get update -q
    $SUDO apt-get install -y --no-install-recommends "${need[@]}"
  else
    log "missing ${need[*]} and no apt-get"; exit 2
  fi
fi
if ! python3 -I -c "import numpy" 2>/dev/null; then
  log "installing numpy (pinned by hash)"
  python3 -m pip install --user --require-hashes -r "$ROOT_DIR/tools/voice_bench/requirements.txt"
fi

# ---------------------------------------------------------------- sandboxes
# Ubuntu 24.04 keeps unprivileged user namespaces from unconfined programs;
# the browser's sandbox and the speech helper's need them.
if [[ -e /proc/sys/kernel/apparmor_restrict_unprivileged_userns ]] &&
   [[ "$(cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns)" != 0 ]]; then
  if [[ -n "$SUDO" || "$(id -u)" -eq 0 ]]; then
    $SUDO sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
  fi
fi
SANDBOX_FLAGS=()
if [[ "$(id -u)" -eq 0 ]]; then
  # Chromium won't run its sandbox as root.
  SANDBOX_FLAGS=(--no-sandbox)
fi

# ---------------------------------------------------------------- speech
log "fetching the pinned voices into $DATA_DIR"
python3 -I "$ROOT_DIR/tools/voice_bench/fetch.py" --dir "$DATA_DIR" \
  --only sherpa-onnx-linux-x64,tts-piper-en_US-ljspeech-medium,tts-piper-en_US-libritts_r-medium-int8,tts-lexicon-en-us
python3 -I "$ROOT_DIR/tools/voice_bench/run_bench.py" --dir "$DATA_DIR" --build
SPEECH_DIR="$REPORT_DIR/speech"
log "rendering the scenarios' speech"
python3 -I "$SCRIPT_DIR/run_e2e.py" render --data "$DATA_DIR" \
  --driver "$DATA_DIR/build/sherpa_bench" --out "$SPEECH_DIR" \
  --hashes "$SCRIPT_DIR/render_hashes.json" | tee "$REPORT_DIR/render.log"
if [[ "${VOICE_E2E_SPEECH_CHECK:-1}" != 0 ]]; then
  log "checking the speech against the bundle's recognizer"
  python3 -I "$SCRIPT_DIR/run_e2e.py" check-speech --speech "$SPEECH_DIR" \
    --bundle "$BUNDLE_DIR" --out "$REPORT_DIR/speech-check.json" \
    | tee "$REPORT_DIR/speech-check.md"
fi

# ---------------------------------------------------------------- the suite
args=(run --browser "$BROWSER" --bundle "$BUNDLE_DIR" --speech "$SPEECH_DIR"
      --report "$REPORT_DIR" --runs "$RUNS")
[[ -n "${VOICE_E2E_SCENARIOS:-}" ]] && args+=(--scenarios "$VOICE_E2E_SCENARIOS")
[[ "${VOICE_E2E_MODEL:-0}" == 1 ]] && args+=(--model)
args+=("${SANDBOX_FLAGS[@]}")
log "running: ${args[*]}"
status=0
python3 -I "$SCRIPT_DIR/run_e2e.py" "${args[@]}" || status=$?
log "report: $REPORT_DIR/report.md (exit $status)"
exit "$status"
