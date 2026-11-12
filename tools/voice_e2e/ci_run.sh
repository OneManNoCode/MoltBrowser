#!/bin/bash
# MoltBrowser voice mode, end to end, on a CI machine (docs/VOICE_E2E.md)
# Copyright 2026 GenEye AI Labs Inc. Licensed under GPLv3.
#
#   tools/voice_e2e/ci_run.sh --fetch-voices   (only the pinned voices)
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
#   VOICE_E2E_FIELD_TRIALS  Trial/Group/ pairs for the browser
#                           (--force-fieldtrials; default none)
#   VOICE_E2E_VOICES        the voices to rotate through (default all three)
#   VOICE_E2E_DATA          the voices' download directory (default
#                           ~/.cache/moltbrowser-voice-e2e)
#   VOICE_E2E_SPEECH_CHECK  0 to skip the recognizer check (default 1)
#   VOICE_E2E_MODEL         1 when an on-device AI model is installed
#   VOICE_E2E_MODEL_FILE    a model file at its pin (tools/models/
#                           fetch_model.py), linked into each scenario's
#                           home: the model scenarios run with it
#   VOICE_E2E_VENV          where numpy goes when python3 has none
#                           (default <VOICE_E2E_DATA>/venv)
#
# Exit status: the runner's (0 Phase 1's exit met, 1 not met, 2 couldn't
# run); the reports are left either way.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DATA_DIR="${VOICE_E2E_DATA:-$HOME/.cache/moltbrowser-voice-e2e}"
# The engine and voices the scenarios' speech is rendered with, pinned in
# tools/voice_bench/artifacts.json. "ci_run.sh --fetch-voices" fetches only
# these (the workflow does, to keep them between runs).
VOICES_TO_FETCH=sherpa-onnx-linux-x64,tts-piper-en_US-ljspeech-medium,tts-piper-en_US-libritts_r-medium-int8,tts-lexicon-en-us
fetch_voices() {
  python3 -I "$ROOT_DIR/tools/voice_bench/fetch.py" --dir "$DATA_DIR" \
    --only "$VOICES_TO_FETCH"
}
if [[ $# -eq 1 && "$1" == --fetch-voices ]]; then
  fetch_voices
  exit
fi

if [[ $# -ne 3 ]]; then
  sed -n '2,38p' "$0"
  exit 2
fi
OUT_DIR="$(cd "$1" && pwd)"
BUNDLE_DIR="$(cd "$2" && pwd)"
mkdir -p "$3"
REPORT_DIR="$(cd "$3" && pwd)"
RUNS="${VOICE_E2E_RUNS:-pulse:clean,pulse:music10,file:clean,typed}"

log() { echo "[voice-e2e] $*"; }

# Until the suite itself runs, a failed step means it couldn't run (2), not
# that it ran and missed Phase 1's exit (1).
trap 'log "a setup step failed (line $LINENO)"; exit 2' ERR

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
  # In a venv: "python3 -I" ignores the user site, and Ubuntu 24.04's
  # Python refuses "pip install --user" (it is externally managed).
  VENV="${VOICE_E2E_VENV:-$DATA_DIR/venv}"
  if ! python3 -c "import ensurepip, venv" 2>/dev/null; then
    log "installing python3-venv"
    $SUDO apt-get install -y --no-install-recommends python3-venv
  fi
  log "installing numpy (pinned by hash) into $VENV"
  python3 -m venv "$VENV"
  "$VENV/bin/python3" -m pip install -q --require-hashes \
    -r "$ROOT_DIR/tools/voice_bench/requirements.txt"
  export PATH="$VENV/bin:$PATH"
  python3 -I -c "import numpy" || { log "numpy didn't install"; exit 2; }
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
fetch_voices
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
if [[ -n "${VOICE_E2E_FIELD_TRIALS:-}" ]]; then
  if ! [[ "$VOICE_E2E_FIELD_TRIALS" =~ ^([A-Za-z0-9_-]+/[A-Za-z0-9_-]+/)+$ ]]; then
    log "VOICE_E2E_FIELD_TRIALS must be Trial/Group/ pairs"; exit 2
  fi
  # One argument: argparse takes a separate value that starts with "--"
  # for an option of its own.
  args+=("--browser-flag=--force-fieldtrials=$VOICE_E2E_FIELD_TRIALS")
fi
[[ -n "${VOICE_E2E_VOICES:-}" ]] && args+=(--voices "$VOICE_E2E_VOICES")
[[ "${VOICE_E2E_MODEL:-0}" == 1 ]] && args+=(--model)
[[ -n "${VOICE_E2E_MODEL_FILE:-}" ]] && args+=(--model-file "$VOICE_E2E_MODEL_FILE")
args+=("${SANDBOX_FLAGS[@]}")
log "running: ${args[*]}"
trap - ERR
status=0
python3 -I "$SCRIPT_DIR/run_e2e.py" "${args[@]}" || status=$?
log "report: $REPORT_DIR/report.md (exit $status)"
# The browser's and voice mode's logs of each failure, in the job's log
# too (the reports' artifacts aren't always reachable where it's read).
python3 -I "$SCRIPT_DIR/run_e2e.py" digest --report "$REPORT_DIR" \
  > "$REPORT_DIR/digest.txt" 2>&1 || true
exit "$status"
