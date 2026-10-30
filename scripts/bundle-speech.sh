#!/bin/bash
# MoltBrowser speech helper bundler
# Copyright 2026 GenEye AI Labs Inc. Licensed under GPLv3.
#
# Builds molt-speech, touchless browsing's resident speech helper
# (docs/design/SPEECH_HELPER_PROTOCOL.md), from pinned sources, and installs
# it with its three models where the browser looks for it:
#
#   macOS:   MoltBrowser.app/Contents/Resources/speech/    (--app)
#   Linux:   <staging>/opt/moltbrowser/speech/              (--dest <staging>/opt/moltbrowser)
#   Windows: <staging>/speech/                             (--dest <staging>, next to MoltBrowser.exe)
#
#   speech/molt-speech[.exe]          the helper (one static executable)
#   speech/models/kws/...             wake phrase: GigaSpeech Zipformer KWS 3.3M
#   speech/models/vad/silero_vad.onnx end of speech: Silero VAD v4
#   speech/models/asr/...             recognition: Kroko EN (2025-08-06)
#   speech/LICENSES/...               the licences and attribution that ship
#   speech/.build-info                versions, pins and the helper's SHA-256
#
# What it builds, each from a source tarball pinned by SHA-256:
#   ONNX Runtime 1.28.2, CPU only, static, with the dependencies its own
#   CMake fetches (cmake/deps.txt is rewritten to point at the pinned local
#   copies); built for client workloads, so its thread pools don't
#   spin-wait (onnxruntime_CLIENT_PACKAGE_BUILD); no ML, generation or
#   float8/float4 operators; then sherpa-onnx 2.0.1, static, with its C API
#   and without TTS, speaker diarization, Python, binaries or other
#   execution providers; then molt-speech (its sources are
#   src/chrome/browser/molt_ai/voice/speech_helper/, built outside GN like
#   whisper-cli), linked statically against both, so one executable ships.
#
# Sources: each tarball is GitHub's (or GitLab's) /archive/ download for a
# pinned commit. If that download fails, it is reproduced from the pinned
# commit with `git archive | gzip -n`, which gives GitHub's bytes exactly
# (checked against every upstream pin that exists). Either way the result
# must match its SHA-256 pin, or the script stops. Models come from
# sherpa-onnx's GitHub releases, pinned the same way, and every model file
# is checked again after unpacking.
#
# Usage:
#   scripts/bundle-speech.sh --app PATH/MoltBrowser.app
#   scripts/bundle-speech.sh --dest DIR        # installs DIR/speech
#   scripts/bundle-speech.sh --build-only      # build into the cache only
#   scripts/bundle-speech.sh --verify DIR      # check DIR/speech's models
# Options:
#   --cache DIR    work directory (default <repo>/.molt-speech-cache, ~6 GB
#                  during the ONNX Runtime build)
#   --jobs N       parallel compile jobs (default 2; ONNX Runtime needs
#                  about 1.5 GB of RAM per job)
#   --fetch MODE   auto (default: download, else git), download, or git
#
# Needs: cmake >= 3.28, a C++20 compiler (GCC 11+, Clang 14+, Xcode 15+,
# Visual Studio 2022), python3, curl, git (for the git fallback), tar,
# bzip2, patch (ONNX Runtime patches its dependencies; without `patch` it
# silently doesn't), and ninja if present. Windows: run from Git Bash in a
# "x64 Native Tools" prompt (cl.exe, lib.exe on PATH); see
# docs/BUILD_MACHINE_CHECKLIST.md ("speech-helper").
#
# Signing: on macOS the helper must be signed explicitly with the hardened
# runtime, like tor/ocr/whisper (codesign --deep skips Contents/Resources);
# scripts/sign-app.sh does it. The models are data, sealed by the app's
# signature.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
HELPER_SRC="$ROOT_DIR/src/chrome/browser/molt_ai/voice/speech_helper"

# shellcheck disable=SC1091
source "$SCRIPT_DIR/lib-bundle-verify.sh"

CACHE_DIR="$ROOT_DIR/.molt-speech-cache"
JOBS=2
FETCH_MODE="auto"
APP_PATH=""
DEST_PARENT=""
VERIFY_DIR=""
BUILD_ONLY=0

# ---------------------------------------------------------------- pins
# Source tarballs: name|url|sha256|git repository|commit|archive prefix.
# The prefix is the tarball's top directory, as GitHub names it. To bump a
# pin: download the new tarball on a trusted machine, check it against the
# project's own pin where there is one (sherpa-onnx's cmake/*.cmake carry
# SHA-256s; ONNX Runtime's cmake/deps.txt carries SHA-1s of the .zip form),
# and paste the SHA-256 and the commit.
SOURCES=(
  "sherpa-onnx|https://github.com/k2-fsa/sherpa-onnx/archive/refs/tags/v2.0.1.tar.gz|24ecc0fccf6344c2556e7c229c849e9f61bf9456b915615197d85f35e3f177e7|https://github.com/k2-fsa/sherpa-onnx|af2d8d81dbfa61c4cea4c568da78f3982edca005|sherpa-onnx-2.0.1"
  "onnxruntime|https://github.com/microsoft/onnxruntime/archive/refs/tags/v1.28.2.tar.gz|a5003dfd85d66a4d45bd2757c7f813f9e835142e9a936ac04877c788d6afe938|https://github.com/microsoft/onnxruntime|33ca9628233dc8f002435e868d4c2e9f82766ca1|onnxruntime-1.28.2"
)
# sherpa-onnx's dependencies (the SHA-256 pins are sherpa-onnx's own, from
# its cmake/*.cmake; the file name is the one those files look for).
SHERPA_DEPS=(
  "kaldi-native-fbank-1.22.4|https://github.com/csukuangfj/kaldi-native-fbank/archive/refs/tags/v1.22.4.tar.gz|257fea639585ca6ee00860493bb9d7ab32488b8a5407ffe434c1f361ea545fae|https://github.com/csukuangfj/kaldi-native-fbank|95032e2223c73addc965bb84d2d1b11498eda4e9|kaldi-native-fbank-1.22.4"
  "utf8proc-2.12.0|https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.12.0.tar.gz|f564011d38b2888d583d510b08e69ffa15aa117155db1b9b49ef1dfe1fa25111|https://github.com/JuliaStrings/utf8proc|79cdd5ab40c79afa559f689ffc13b76812dee1ac|utf8proc-2.12.0"
  "kaldi-decoder-0.3.0|https://github.com/k2-fsa/kaldi-decoder/archive/refs/tags/v0.3.0.tar.gz|b9f34cfb4fd3b1344100eead79ef4d37aa15962274b9e3056de345021f76a1b0|https://github.com/k2-fsa/kaldi-decoder|62fda2d74246a90db9570e46fb16b012f813fae7|kaldi-decoder-0.3.0"
  "simple-sentencepiece-0.7|https://github.com/pkufool/simple-sentencepiece/archive/refs/tags/v0.7.tar.gz|1748a822060a35baa9f6609f84efc8eb54dc0e74b9ece3d82367b7119fdc75af|https://github.com/pkufool/simple-sentencepiece|62dd423df1d51da5ea06f1c3a046fc04f01b4f39|simple-sentencepiece-0.7"
  "json-3.12.0|https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz|4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187|https://github.com/nlohmann/json|55f93686c01528224f448c19128836e7df245f72|json-3.12.0"
  "openfst-1.8.5-2026-07-09|https://github.com/csukuangfj/openfst/archive/refs/tags/v1.8.5-2026-07-09.tar.gz|2ff712a32952fcb01d351121a6bc8ccf4fdc6b2aa06ce8df2b3095dedd518c0e|https://github.com/csukuangfj/openfst|f702a6317d600ac953fdbb4978674c3a28b6115c|openfst-1.8.5-2026-07-09"
  "eigen-5.0.1|https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.tar.gz|e9c326dc8c05cd1e044c71f30f1b2e34a6161a3b6ecf445d56b53ff1669e3dec|https://gitlab.com/libeigen/eigen.git|bc3b39870ecb690a623a3f49149a358b95c5781d|eigen-5.0.1"
  # kaldi-decoder's (its cmake/kaldifst.cmake pins the SHA-256).
  "kaldifst-1.8.0|https://github.com/k2-fsa/kaldifst/archive/refs/tags/v1.8.0.tar.gz|3f247b7e5a2409071202f5e2bc6200060f66728c0a3443c03923ad2723e040b3|https://github.com/k2-fsa/kaldifst|ab5bdd013bdf13921e6aeee77db5722ebf9955fb|kaldifst-1.8.0"
)
# Dependencies of sherpa-onnx's dependencies, given to their CMake as
# source directories (FETCHCONTENT_SOURCE_DIR_<NAME>, after our own
# SHA-256 check; their own pin is of a .zip, which can't be reproduced from
# git): kaldi-native-fbank's kissfft.
SHERPA_SOURCE_DIRS=(
  "KISSFFT|https://github.com/mborgerding/kissfft/archive/febd4caeed32e33ad8b2e0bb5ea77542c40f18ec.tar.gz|fe840c3e29fc98250ae3ba843696894f190037f947332296e4be281a18f341cd|https://github.com/mborgerding/kissfft|febd4caeed32e33ad8b2e0bb5ea77542c40f18ec|kissfft-febd4caeed32e33ad8b2e0bb5ea77542c40f18ec"
)
# ONNX Runtime's dependencies for a CPU build (cmake/deps.txt name first).
# deps.txt names the .zip downloads; these are the same commits' .tar.gz
# downloads, which can be reproduced from git (the .zip can't).
ORT_DEPS=(
  "abseil_cpp|https://github.com/abseil/abseil-cpp/archive/refs/tags/20250814.0.tar.gz|9b2b72d4e8367c0b843fa2bcfa2b08debbe3cee34f7aaa27de55a6cbb3e843db|https://github.com/abseil/abseil-cpp|987c57f325f7fa8472fa84e1f885f7534d391b0d|abseil-cpp-20250814.0"
  "re2|https://github.com/google/re2/archive/refs/tags/2024-07-02.tar.gz|eb2df807c781601c14a260a507a5bb4509be1ee626024cb45acbd57cb9d4032b|https://github.com/google/re2|6dcd83d60f7944926bfd308cc13979fc53dd69ca|re2-2024-07-02"
  "protobuf|https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz|22fdaf641b31655d4b2297f9981fa5203b2866f8332d3c6333f6b0107bb320de|https://github.com/protocolbuffers/protobuf|f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c|protobuf-21.12"
  "date|https://github.com/HowardHinnant/date/archive/refs/tags/v3.0.1.tar.gz|7a390f200f0ccd207e8cff6757e04817c1a0aec3e327b006b7eb451c57ee3538|https://github.com/HowardHinnant/date|6e921e1b1d21e84a5c82416ba7ecd98e33a436d0|date-3.0.1"
  "mp11|https://github.com/boostorg/mp11/archive/refs/tags/boost-1.82.0.tar.gz|1705680b0849cac3258e528270c9071b440b95372e5be0b9197da89e84a794eb|https://github.com/boostorg/mp11|0a0b5fb001ce0233ae3a6f99d849c0649e5a7361|mp11-boost-1.82.0"
  "json|https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz|0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406|https://github.com/nlohmann/json|9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03|json-3.11.3"
  "pytorch_cpuinfo|https://github.com/pytorch/cpuinfo/archive/4628dc060ce4e82345dc166bbac875609db4ff69.tar.gz|a550205e891f9f1982044a306cb54556347645cba129af34cd907160f83bd0f1|https://github.com/pytorch/cpuinfo|4628dc060ce4e82345dc166bbac875609db4ff69|cpuinfo-4628dc060ce4e82345dc166bbac875609db4ff69"
  "microsoft_gsl|https://github.com/microsoft/GSL/archive/refs/tags/v4.2.1.tar.gz|d959f1cb8bbb9c94f033ae5db60eaf5f416be1baa744493c32585adca066fe1f|https://github.com/microsoft/GSL|249146590e322123cd0b378b1f364d6069717687|GSL-4.2.1"
  "safeint|https://github.com/dcleblanc/SafeInt/archive/refs/tags/3.0.28.tar.gz|d6b164bcea92a746e4d44132e505c7ab1816d1089ba99ebc674ccd4b70262ed5|https://github.com/dcleblanc/SafeInt|4cafc9196c4da9c817992b20f5253ef967685bf8|SafeInt-3.0.28"
  "flatbuffers|https://github.com/google/flatbuffers/archive/refs/tags/v23.5.26.tar.gz|1cce06b17cddd896b6d73cc047e36a254fb8df4d7ea18a46acf16c4c0cd3f3f3|https://github.com/google/flatbuffers|0100f6a5779831fa7a651e4b67ef389a8752bd9b|flatbuffers-23.5.26"
  "onnx|https://github.com/onnx/onnx/archive/refs/tags/v1.22.0.tar.gz|70bb8b25cf31ea9b1d9f94baacfdc8c4fa27a760f9a10f5d93881bc9eede5fbc|https://github.com/onnx/onnx|2bb50465112feca9003e1ed654d77f01ff1415ca|onnx-1.22.0"
  "eigen|https://github.com/eigen-mirror/eigen/archive/1d8b82b0740839c0de7f1242a3585e3390ff5f33.tar.gz|c8adb6532fbfacd2ec1418c66404159b00c34d855bdc19cefa1f0db0e5e0aa5f|https://github.com/eigen-mirror/eigen|1d8b82b0740839c0de7f1242a3585e3390ff5f33|eigen-1d8b82b0740839c0de7f1242a3585e3390ff5f33"
  "microsoft_wil|https://github.com/microsoft/wil/archive/refs/tags/v1.0.250325.1.tar.gz|c9e667d5f86ded43d17b5669d243e95ca7b437e3a167c170805ffd4aa8a9a786|https://github.com/microsoft/wil|b6ec76a2d9a609897f25a7fa0a0bdf4238e94e35|wil-1.0.250325.1"
)
# protoc, which compiles ONNX's .proto files at build time (never shipped).
# On Linux and Windows ONNX Runtime builds it from the pinned protobuf; on a
# Mac it insists on protobuf's release binary: deps.txt name|url|sha256 (its
# own SHA-1 pin is checked too).
PROTOC_MAC="protoc_mac_universal|https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protoc-21.12-osx-universal_binary.zip|1acab3d1f774d120d5a38bccb78fd379d35226b254dc258978bc45593478ca4c"
# Models (sherpa-onnx's GitHub releases; the same pins as
# tools/voice_bench/artifacts.json): name|url|sha256.
MODEL_ARCHIVES=(
  "kws|https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2|f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a"
  "asr|https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06.tar.bz2|c8676e5ff9ac2a85296e53ee0fd4d5fb1db6770e7a7647166eeafe349ade6834"
  "vad|https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/silero_vad.onnx|9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6"
)
# Every file that ships under speech/models/: path|sha256|where it comes
# from (archive member, or "generated" by make_speech_vocab.py from the
# pinned files above).
MODEL_FILES=(
  "kws/encoder.int8.onnx|1e721676515bcd42a186979733981213c66c80db680e1cc582dfedf3be76e678|sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
  "kws/decoder.onnx|f61ebd3eed3773a44d088d53dfae92dbb6aec4839f4dcaee2d402414741663a3|sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/decoder-epoch-12-avg-2-chunk-16-left-64.onnx"
  "kws/joiner.int8.onnx|eae9da0c7e1e6c6a3f4cc42d167899c388f6c6701b94cb96320e4f55df79624c|sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
  "kws/tokens.txt|fd2ded4050a55d2b1578870ba8697d02371980217806b7558bd0a5cc60f3ba53|sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/tokens.txt"
  "kws/README.md|74e42d37d63acd2366b042151d576fcc0c4917a41de339414a37f1e79f4e80e2|sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/README.md"
  "kws/pieces.vocab|@KWS_VOCAB_SHA256@|generated"
  "asr/encoder.onnx|d4881c57449d581e0770fd53fa66c2fdc6cd167d92ece7c715e603defc96d9d4|sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06/encoder.onnx"
  "asr/decoder.onnx|455ba38466fce8d5a57e7db68a323b684079ca4d9e1dd93a740d9b2429aae3b1|sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06/decoder.onnx"
  "asr/joiner.onnx|d406f616736350e2a7df3e39398b78eb2fc1a2ca6973a19d3853fa3227e25b52|sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06/joiner.onnx"
  "asr/tokens.txt|396dbeb5f4858875690716084f54e90d339679d0ba3e6b5b584f3d7589254d2d|sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06/tokens.txt"
  "asr/README.md|f42a5c6d54eb811dd0dd624cbee0e86205fc7bcb05507097f3278cf1b0b5c6af|sherpa-onnx-streaming-zipformer-en-kroko-2025-08-06/README.md"
  "asr/bpe.vocab|@ASR_VOCAB_SHA256@|generated"
  "vad/silero_vad.onnx|9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6|silero_vad.onnx"
)
# The KWS model's bpe.model, read only to generate kws/pieces.vocab.
KWS_BPE_MODEL="sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/bpe.model|c8a2a0129c4ab8e463164c142f82d25649661b122c8cd0b7aab5c9e80b90ad24"

# ---------------------------------------------------------------- options
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app) APP_PATH="$2"; shift 2 ;;
    --dest) DEST_PARENT="$2"; shift 2 ;;
    --verify) VERIFY_DIR="$2"; shift 2 ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --cache) CACHE_DIR="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --fetch) FETCH_MODE="$2"; shift 2 ;;
    -h|--help) sed -n '2,62p' "$0"; exit 0 ;;
    *) echo "[bundle-speech] unknown option: $1" >&2; exit 2 ;;
  esac
done

log() { echo "[bundle-speech] $*"; }
die() { echo "[bundle-speech] FATAL: $*" >&2; exit 1; }

case "$FETCH_MODE" in auto|download|git) ;; *) die "--fetch must be auto, download or git" ;; esac
[[ "$JOBS" =~ ^[1-9][0-9]?$ ]] || die "--jobs must be 1 to 99"

case "$(uname -s)" in
  Darwin) OS=mac ;;
  Linux) OS=linux ;;
  MINGW*|MSYS*|CYGWIN*) OS=win ;;
  *) die "unsupported OS $(uname -s)" ;;
esac
case "$(uname -m)" in
  x86_64|amd64) ARCH=x64 ;;
  arm64|aarch64) ARCH=arm64 ;;
  *) die "unsupported CPU $(uname -m)" ;;
esac
EXE=molt-speech
[[ "$OS" == win ]] && EXE=molt-speech.exe

sha1_of() {
  if command -v sha1sum >/dev/null 2>&1; then
    sha1sum "$1" | awk '{print $1}'
  else
    shasum -a 1 "$1" | awk '{print $1}'
  fi
}

# Fills in the generated files' pins: the vocabularies are a pure function
# of the pinned bpe.model and tokens.txt (make_speech_vocab.py).
KWS_VOCAB_SHA256="171faa7841128c4cfbf1f7e19479cf8b2c7c41d5e0d85b3a30315d551ad28810"
ASR_VOCAB_SHA256="5b1f3dfe7271e7693750834bd1a32a4e6b65322603a02adfafae5b025fc29180"
model_files() {
  local f
  for f in "${MODEL_FILES[@]}"; do
    f="${f/@KWS_VOCAB_SHA256@/$KWS_VOCAB_SHA256}"
    echo "${f/@ASR_VOCAB_SHA256@/$ASR_VOCAB_SHA256}"
  done
}

# verify_models DIR: every file of MODEL_FILES under DIR/models matches its
# pin, and nothing else is there.
verify_models() {
  local dir="$1" path sha _rest actual count=0 present
  [[ -d "$dir/models" ]] || { echo "[bundle-speech] $dir/models is missing"; return 1; }
  while IFS='|' read -r path sha _rest; do
    [[ -f "$dir/models/$path" && ! -L "$dir/models/$path" ]] || {
      echo "[bundle-speech] missing: models/$path"; return 1; }
    actual="$(sha256_of "$dir/models/$path")"
    if [[ "$actual" != "$sha" ]]; then
      echo "[bundle-speech] SHA-256 mismatch: models/$path"
      echo "[bundle-speech]   expected $sha"
      echo "[bundle-speech]   actual   $actual"
      return 1
    fi
    count=$((count + 1))
  done < <(model_files)
  present="$(find "$dir/models" -type f -o -type l | wc -l | tr -d ' ')"
  if [[ "$present" != "$count" ]]; then
    echo "[bundle-speech] models/ holds $present files; expected exactly $count"
    return 1
  fi
  echo "[bundle-speech] OK: $count model files match their pins"
}

if [[ -n "$VERIFY_DIR" ]]; then
  verify_models "$VERIFY_DIR/speech"
  [[ -x "$VERIFY_DIR/speech/$EXE" ]] || die "$VERIFY_DIR/speech/$EXE is missing"
  exit 0
fi

if [[ "$BUILD_ONLY" == 0 && -z "$APP_PATH" && -z "$DEST_PARENT" ]]; then
  die "pass --app PATH, --dest DIR or --build-only (see --help)"
fi

# ---------------------------------------------------------------- preflight
missing=""
for tool in cmake curl tar bzip2 patch python3; do
  command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
[[ "$FETCH_MODE" != download ]] && ! command -v git >/dev/null 2>&1 && missing="$missing git"
[[ -n "$missing" ]] && die "missing tools:$missing"
cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
python3 -I -c "import sys; v=tuple(int(x) for x in sys.argv[1].split('.')[:2]); sys.exit(0 if v >= (3, 28) else 1)" \
  "$cmake_version" || die "cmake $cmake_version is too old: ONNX Runtime 1.28 needs 3.28"
GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); fi

mkdir -p "$CACHE_DIR/downloads" "$CACHE_DIR/build"
DL="$CACHE_DIR/downloads"
WORK="$CACHE_DIR/build/$OS-$ARCH"
mkdir -p "$WORK"

# ---------------------------------------------------------------- fetching
# fetch_pinned FILE URL SHA256 [GIT_URL COMMIT PREFIX]
fetch_pinned() {
  local file="$1" url="$2" sha="$3" git_url="${4:-}" commit="${5:-}" prefix="${6:-}"
  local rc=0
  if [[ -f "$file" ]]; then
    verify_sha256 "$file" "$sha" >/dev/null && return 0
    log "cached $file does not match its pin; fetching it again"
    rm -f "$file"
  fi
  if [[ "$FETCH_MODE" != git ]]; then
    log "downloading $url"
    if curl -fsSL --retry 2 -o "$file.tmp" "$url"; then
      rc=0
      verify_sha256 "$file.tmp" "$sha" || rc=$?
      if [[ $rc == 0 ]]; then mv "$file.tmp" "$file"; return 0; fi
      rm -f "$file.tmp"
      die "$url does not match its SHA-256 pin"
    fi
    rm -f "$file.tmp"
    [[ "$FETCH_MODE" == download || -z "$git_url" ]] && die "download failed: $url"
    log "download failed; reproducing it from git"
  fi
  [[ -n "$git_url" ]] || die "no git source for $url"
  local tmp
  tmp="$(mktemp -d "$CACHE_DIR/build/git.XXXXXX")"
  git init -q "$tmp/r"
  git -C "$tmp/r" fetch -q --depth 1 "$git_url" "$commit" ||
    { rm -rf "$tmp"; die "git fetch $git_url $commit failed"; }
  local got
  got="$(git -C "$tmp/r" rev-parse 'FETCH_HEAD^{commit}')"
  if [[ "$got" != "$commit" ]]; then
    rm -rf "$tmp"
    die "$git_url gave commit $got, pinned $commit"
  fi
  git -C "$tmp/r" archive --format=tar --prefix="$prefix/" "$commit" | gzip -cn > "$file.tmp"
  rm -rf "$tmp"
  rc=0
  verify_sha256 "$file.tmp" "$sha" || rc=$?
  if [[ $rc != 0 ]]; then
    rm -f "$file.tmp"
    die "the archive of $git_url@$commit does not match its pin"
  fi
  mv "$file.tmp" "$file"
}

fetch_table() {
  local entry name url sha git_url commit prefix
  for entry in "$@"; do
    IFS='|' read -r name url sha git_url commit prefix <<< "$entry"
    fetch_pinned "$DL/$prefix.tar.gz" "$url" "$sha" "$git_url" "$commit" "$prefix"
  done
}

log "fetching sources into $DL"
fetch_table "${SOURCES[@]}" "${SHERPA_DEPS[@]}" "${SHERPA_SOURCE_DIRS[@]}" \
  "${ORT_DEPS[@]}"

PROTOC_NAME=""
if [[ "$OS" == mac ]]; then
  IFS='|' read -r PROTOC_NAME url sha <<< "$PROTOC_MAC"
  PROTOC_ZIP="$DL/$(basename "$url")"
  fetch_pinned "$PROTOC_ZIP" "$url" "$sha"
fi

for entry in "${MODEL_ARCHIVES[@]}"; do
  IFS='|' read -r name url sha <<< "$entry"
  fetch_pinned "$DL/$(basename "$url")" "$url" "$sha"
done

# fresh_extract TARBALL DIR: unpacks into a new, empty DIR.
fresh_extract() {
  rm -rf "$2"
  mkdir -p "$2"
  tar -xzf "$1" -C "$2"
}

# ---------------------------------------------------------------- ONNX Runtime
ORT_PREFIX="onnxruntime-1.28.2"
ORT_SRC="$WORK/src/$ORT_PREFIX"
ORT_BUILD="$WORK/ort-build"
ORT_OUT="$WORK/ort"
ORT_STAMP="$ORT_OUT/.stamp"
ORT_SHA="$(sha256_of "$DL/$ORT_PREFIX.tar.gz")"
# A build directory configured from this tarball (and this script's ORT
# flags) is reused, so an interrupted build continues.
sed -n '/^  ort_flags=(/,/^  )/p' "$0" > "$WORK/ort-flags.txt"
ORT_CONFIG_KEY="$ORT_SHA $(sha256_of "$WORK/ort-flags.txt")"
if [[ ! -f "$ORT_STAMP" || "$(cat "$ORT_STAMP")" != "$ORT_SHA" ]]; then
  log "building ONNX Runtime (CPU, static; about an hour at -j2)"
  if [[ ! -f "$ORT_BUILD/.configured" || "$(cat "$ORT_BUILD/.configured")" != "$ORT_CONFIG_KEY" ]]; then
  fresh_extract "$DL/$ORT_PREFIX.tar.gz" "$WORK/src"
  # Point every dependency the build needs at its pinned local tarball.
  deps_txt="$ORT_SRC/cmake/deps.txt"
  cp "$deps_txt" "$deps_txt.orig"
  for entry in "${ORT_DEPS[@]}"; do
    IFS='|' read -r name url sha git_url commit prefix <<< "$entry"
    local_file="$DL/$prefix.tar.gz"
    python3 -I - "$deps_txt" "$name" "$local_file" "$(sha1_of "$local_file")" <<'EOF'
import sys
path, name, url, sha1 = sys.argv[1:]
lines = open(path, encoding="utf-8").read().splitlines()
hits = 0
for i, line in enumerate(lines):
    if line.split(";", 1)[0] == name:
        lines[i] = "%s;%s;%s" % (name, url, sha1)
        hits += 1
if hits != 1:
    sys.exit("deps.txt: %d entries named %s" % (hits, name))
open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
EOF
  done
  # protoc (Mac): the pinned zip, after ONNX Runtime's own SHA-1 pin agrees.
  if [[ -n "$PROTOC_NAME" ]]; then
    want_sha1="$(awk -F';' -v n="$PROTOC_NAME" '$1 == n {print $3}' "$deps_txt.orig")"
    [[ "$(sha1_of "$PROTOC_ZIP")" == "$want_sha1" ]] ||
      die "$PROTOC_ZIP does not match ONNX Runtime's SHA-1 pin"
    python3 -I - "$deps_txt" "$PROTOC_NAME" "$PROTOC_ZIP" "$want_sha1" <<'EOF'
import sys
path, name, url, sha1 = sys.argv[1:]
lines = open(path, encoding="utf-8").read().splitlines()
lines = ["%s;%s;%s" % (name, url, sha1) if l.split(";", 1)[0] == name else l
         for l in lines]
open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
EOF
  fi
  ort_flags=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER
    -DFETCHCONTENT_QUIET=ON
    -DPython_EXECUTABLE="$(command -v python3)"
    -Donnxruntime_BUILD_SHARED_LIB=OFF
    -Donnxruntime_BUILD_UNIT_TESTS=OFF
    -Donnxruntime_BUILD_BENCHMARKS=OFF
    -Donnxruntime_RUN_ONNX_TESTS=OFF
    -Donnxruntime_ENABLE_PYTHON=OFF
    -Donnxruntime_USE_MIMALLOC=OFF
    -Donnxruntime_USE_TELEMETRY=OFF
    -Donnxruntime_ENABLE_LTO=OFF
    -Donnxruntime_CLIENT_PACKAGE_BUILD=ON
    -Donnxruntime_DISABLE_ML_OPS=ON
    -Donnxruntime_DISABLE_GENERATION_OPS=ON
    -Donnxruntime_DISABLE_FLOAT8_TYPES=ON
    -Donnxruntime_DISABLE_FLOAT4_TYPES=ON
  )
  case "$OS" in
    mac) ort_flags+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0) ;;
    win) ort_flags+=(-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded) ;;
  esac
  rm -rf "$ORT_BUILD"
  cmake "${GENERATOR[@]}" -S "$ORT_SRC/cmake" -B "$ORT_BUILD" "${ort_flags[@]}" > "$WORK/ort-configure.log" 2>&1 ||
    { tail -40 "$WORK/ort-configure.log"; die "ONNX Runtime configure failed ($WORK/ort-configure.log)"; }
  # Nothing may come from the network: every Fetch line names a local file.
  if grep -E "^-- Fetch [A-Za-z0-9_]+ from https?://" "$WORK/ort-configure.log"; then
    die "ONNX Runtime's configure fetched from the network (see above)"
  fi
  echo "$ORT_CONFIG_KEY" > "$ORT_BUILD/.configured"
  fi
  # Its libraries, and re2, which they use but nothing in "all" builds
  # without the shared library.
  for target in "" re2; do
    target_args=()
    [[ -n "$target" ]] && target_args=(--target "$target")
    cmake --build "$ORT_BUILD" --config Release --parallel "$JOBS" "${target_args[@]}" \
        >> "$WORK/ort-build.log" 2>&1 ||
      { grep -E "error|Error" "$WORK/ort-build.log" | head -30; die "ONNX Runtime build failed ($WORK/ort-build.log)"; }
  done
  # One static library of everything it built (but protoc's and the
  # full protobuf, which only the build tools use), and the C/C++ API headers.
  rm -rf "$ORT_OUT"
  mkdir -p "$ORT_OUT/lib" "$ORT_OUT/include"
  cp "$ORT_SRC/include/onnxruntime/core/session/"*.h "$ORT_OUT/include/"
  if [[ "$OS" == win ]]; then
    find "$ORT_BUILD" -name '*.lib' -path '*Release*' ! -name 'libprotoc*' ! -name 'libprotobuf.lib' \
      ! -name 'libprotobufd*' | sort > "$WORK/ort-libs.txt"
    ort_libs=()
    while IFS= read -r lib; do ort_libs+=("$(cygpath -w "$lib")"); done < "$WORK/ort-libs.txt"
    lib.exe /NOLOGO /OUT:"$(cygpath -w "$ORT_OUT/lib/onnxruntime.lib")" "${ort_libs[@]}"
  else
    find "$ORT_BUILD" -name '*.a' ! -name 'libprotoc.a' ! -name 'libprotobuf.a' \
      ! -name 'libgtest*' ! -name 'libgmock*' | sort > "$WORK/ort-libs.txt"
    if [[ "$OS" == mac ]]; then
      ort_libs=()
      while IFS= read -r lib; do ort_libs+=("$lib"); done < "$WORK/ort-libs.txt"
      libtool -static -o "$ORT_OUT/lib/libonnxruntime.a" "${ort_libs[@]}"
    else
      {
        echo "CREATE $ORT_OUT/lib/libonnxruntime.a"
        sed 's/^/ADDLIB /' "$WORK/ort-libs.txt"
        echo "SAVE"
        echo "END"
      } | ar -M
    fi
  fi
  log "ONNX Runtime: $(wc -l < "$WORK/ort-libs.txt" | tr -d ' ') static libraries combined"
  echo "$ORT_SHA" > "$ORT_STAMP"
fi

# ---------------------------------------------------------------- sherpa-onnx
SHERPA_PREFIX_DIR="sherpa-onnx-2.0.1"
SHERPA_SRC="$WORK/src/$SHERPA_PREFIX_DIR"
SHERPA_BUILD="$WORK/sherpa-build"
SHERPA_OUT="$WORK/sherpa"
SHERPA_STAMP="$SHERPA_OUT/.stamp"
sherpa_key="$(sha256_of "$DL/$SHERPA_PREFIX_DIR.tar.gz") $(cat "$ORT_STAMP")"
if [[ ! -f "$SHERPA_STAMP" || "$(cat "$SHERPA_STAMP")" != "$sherpa_key" ]]; then
  log "building sherpa-onnx (static, C API, no TTS)"
  rm -rf "$SHERPA_SRC" "$SHERPA_BUILD" "$SHERPA_OUT"
  mkdir -p "$WORK/src" "$SHERPA_BUILD"
  tar -xzf "$DL/$SHERPA_PREFIX_DIR.tar.gz" -C "$WORK/src"
  # sherpa-onnx's cmake/*.cmake use a pre-downloaded tarball in the build
  # directory (and check it against their own SHA-256 pins).
  for entry in "${SHERPA_DEPS[@]}"; do
    IFS='|' read -r name url sha git_url commit prefix <<< "$entry"
    cp "$DL/$prefix.tar.gz" "$SHERPA_BUILD/$name.tar.gz"
  done
  source_dir_flags=()
  for entry in "${SHERPA_SOURCE_DIRS[@]}"; do
    IFS='|' read -r name url sha git_url commit prefix <<< "$entry"
    mkdir -p "$WORK/deps"
    rm -rf "${WORK:?}/deps/$prefix"
    tar -xzf "$DL/$prefix.tar.gz" -C "$WORK/deps"
    source_dir_flags+=("-DFETCHCONTENT_SOURCE_DIR_$name=$WORK/deps/$prefix")
  done
  sherpa_flags=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_INSTALL_PREFIX="$SHERPA_OUT"
    -DBUILD_SHARED_LIBS=OFF
    -DSHERPA_ONNX_ENABLE_C_API=ON
    -DSHERPA_ONNX_ENABLE_TTS=OFF
    -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF
    -DSHERPA_ONNX_ENABLE_PYTHON=OFF
    -DSHERPA_ONNX_ENABLE_TESTS=OFF
    -DSHERPA_ONNX_ENABLE_CHECK=OFF
    -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF
    -DSHERPA_ONNX_ENABLE_JNI=OFF
    -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF
    -DSHERPA_ONNX_ENABLE_GPU=OFF
    -DSHERPA_ONNX_ENABLE_BINARY=OFF
    -DSHERPA_ONNX_BUILD_C_API_EXAMPLES=OFF
    -DSHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE=ON
    -DFETCHCONTENT_QUIET=OFF
  )
  case "$OS" in
    mac) sherpa_flags+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0) ;;
    win) sherpa_flags+=(-DSHERPA_ONNX_USE_STATIC_CRT=ON) ;;
  esac
  # HOME points at an empty directory so sherpa-onnx's lookup in
  # ~/Downloads can't pick up anything but the pinned copies.
  mkdir -p "$WORK/empty-home"
  HOME="$WORK/empty-home" \
  SHERPA_ONNXRUNTIME_LIB_DIR="$ORT_OUT/lib" \
  SHERPA_ONNXRUNTIME_INCLUDE_DIR="$ORT_OUT/include" \
    cmake "${GENERATOR[@]}" -S "$SHERPA_SRC" -B "$SHERPA_BUILD" "${sherpa_flags[@]}" \
      "${source_dir_flags[@]}" \
      > "$WORK/sherpa-configure.log" 2>&1 ||
    { tail -40 "$WORK/sherpa-configure.log"; die "sherpa-onnx configure failed ($WORK/sherpa-configure.log)"; }
  # Nothing may come from the network: with FETCHCONTENT_QUIET off, CMake's
  # download step names every URL it uses ("Using src='...'"); the pinned
  # copies are local files, which it unpacks without one.
  if grep -E "Using src='(https?|ftp)://" "$WORK/sherpa-configure.log"; then
    die "sherpa-onnx's configure fetched from the network (see above)"
  fi
  cmake --build "$SHERPA_BUILD" --config Release --parallel "$JOBS" --target install \
      > "$WORK/sherpa-build.log" 2>&1 ||
    { grep -E "error|Error" "$WORK/sherpa-build.log" | head -30; die "sherpa-onnx build failed ($WORK/sherpa-build.log)"; }
  echo "$sherpa_key" > "$SHERPA_STAMP"
fi

# ---------------------------------------------------------------- models
MODELS_OUT="$WORK/speech/models"
rm -rf "$WORK/speech" "$WORK/model-src"
mkdir -p "$WORK/model-src" "$MODELS_OUT/kws" "$MODELS_OUT/asr" "$MODELS_OUT/vad"
for entry in "${MODEL_ARCHIVES[@]}"; do
  IFS='|' read -r name url sha <<< "$entry"
  file="$DL/$(basename "$url")"
  case "$file" in
    *.tar.bz2) tar -xjf "$file" -C "$WORK/model-src" ;;
    *) cp "$file" "$WORK/model-src/" ;;
  esac
done
while IFS='|' read -r path sha from; do
  [[ "$from" == generated ]] && continue
  src="$WORK/model-src/$from"
  [[ -f "$src" && ! -L "$src" ]] || die "model archive lacks $from"
  install -m 644 "$src" "$MODELS_OUT/$path"
done < <(model_files)
IFS='|' read -r bpe_member bpe_sha <<< "$KWS_BPE_MODEL"
verify_sha256 "$WORK/model-src/$bpe_member" "$bpe_sha" >/dev/null ||
  die "the KWS bpe.model does not match its pin"
python3 -I "$HELPER_SRC/make_speech_vocab.py" sentencepiece \
  "$WORK/model-src/$bpe_member" "$MODELS_OUT/kws/tokens.txt" \
  "$MODELS_OUT/kws/pieces.vocab"
python3 -I "$HELPER_SRC/make_speech_vocab.py" tokens \
  "$MODELS_OUT/asr/tokens.txt" "$MODELS_OUT/asr/bpe.vocab"
verify_models "$WORK/speech" || die "the models do not match their pins"
rm -rf "$WORK/model-src"

# ---------------------------------------------------------------- molt-speech
HELPER_BUILD="$WORK/helper-build"
log "building molt-speech"
rm -rf "$HELPER_BUILD"
helper_flags=(
  -DCMAKE_BUILD_TYPE=Release
  -DMOLT_SPEECH_SHERPA_PREFIX="$SHERPA_OUT"
  -DMOLT_SPEECH_ORT_LIB_DIR="$ORT_OUT/lib"
)
case "$OS" in
  mac) helper_flags+=(-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0) ;;
  win) helper_flags+=(-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded) ;;
esac
cmake "${GENERATOR[@]}" -S "$HELPER_SRC" -B "$HELPER_BUILD" "${helper_flags[@]}" \
    > "$WORK/helper-configure.log" 2>&1 ||
  { tail -40 "$WORK/helper-configure.log"; die "molt-speech configure failed"; }
cmake --build "$HELPER_BUILD" --config Release --parallel "$JOBS" --target molt-speech \
    > "$WORK/helper-build.log" 2>&1 ||
  { grep -E "error|Error" "$WORK/helper-build.log" | head -30; die "molt-speech build failed ($WORK/helper-build.log)"; }
HELPER_BIN="$(find "$HELPER_BUILD" -name "$EXE" -type f | head -1)"
[[ -n "$HELPER_BIN" ]] || die "the build produced no $EXE"
install -m 755 "$HELPER_BIN" "$WORK/speech/$EXE"
if [[ "$OS" != win ]]; then
  strip "$WORK/speech/$EXE" 2>/dev/null || true
fi

# It must load nothing but the OS: no ONNX Runtime or sherpa-onnx library.
case "$OS" in
  linux)
    if ldd "$WORK/speech/$EXE" | grep -E "onnx|sherpa|not found"; then
      die "molt-speech loads a library it should have linked statically"
    fi ;;
  mac)
    if otool -L "$WORK/speech/$EXE" | tail -n +2 | grep -vE "^\s+/(usr/lib|System/Library)/"; then
      die "molt-speech loads a library from outside the OS"
    fi ;;
esac

# Licences and attribution that ship with it (Kroko is CC BY-SA 4.0).
mkdir -p "$WORK/speech/LICENSES"
install -m 644 "$HELPER_SRC/LICENSES.md" "$WORK/speech/LICENSES/README.md"
install -m 644 "$WORK/src/$ORT_PREFIX/LICENSE" "$WORK/speech/LICENSES/onnxruntime-LICENSE.txt"
install -m 644 "$WORK/src/$ORT_PREFIX/ThirdPartyNotices.txt" "$WORK/speech/LICENSES/onnxruntime-ThirdPartyNotices.txt"
install -m 644 "$SHERPA_SRC/LICENSE" "$WORK/speech/LICENSES/sherpa-onnx-LICENSE.txt"
for dep in "$SHERPA_BUILD"/_deps/*-src "$WORK"/deps/*; do
  [[ -d "$dep" ]] || continue
  dep_name="$(basename "$dep")"
  dep_name="${dep_name%-src}"
  dep_name="${dep_name%%-[0-9a-f]*[0-9a-f]}"
  for f in "$dep"/LICENSE* "$dep"/COPYING* "$dep"/NOTICE*; do
    [[ -f "$f" ]] || continue
    install -m 644 "$f" "$WORK/speech/LICENSES/$dep_name-$(basename "$f")"
  done
done

HELPER_SHA="$(sha256_of "$WORK/speech/$EXE")"
{
  echo "molt-speech protocol 1"
  echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ) on $OS-$ARCH"
  echo "sherpa-onnx 2.0.1 af2d8d81dbfa61c4cea4c568da78f3982edca005"
  echo "onnxruntime 1.28.2 33ca9628233dc8f002435e868d4c2e9f82766ca1"
  echo "$EXE sha256 $HELPER_SHA"
  model_files | awk -F'|' '{print "models/" $1 " sha256 " $2}'
} > "$WORK/speech/.build-info"
log "molt-speech $(du -h "$WORK/speech/$EXE" | cut -f1), SHA-256 $HELPER_SHA"

[[ "$BUILD_ONLY" == 1 ]] && { log "built into $WORK/speech"; exit 0; }

# ---------------------------------------------------------------- install
if [[ -n "$APP_PATH" ]]; then
  [[ -d "$APP_PATH/Contents/Resources" ]] || die "$APP_PATH is not an app bundle"
  DEST="$APP_PATH/Contents/Resources/speech"
else
  [[ -d "$DEST_PARENT" ]] || die "$DEST_PARENT does not exist"
  DEST="$DEST_PARENT/speech"
fi
rm -rf "${DEST:?}"
cp -R "$WORK/speech" "$DEST"
verify_models "$DEST" || die "the installed models do not match their pins"
if [[ "$OS" == mac ]]; then
  xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
fi
log "installed into $DEST"
