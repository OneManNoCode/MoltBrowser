#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Builds apm_sim (apm_sim.cc): the pinned Chromium's WebRTC audio processing
# (tools/chromium/pin.sh), standalone, with voice mode's echo canceller
# tuning (src/media/webrtc/molt_speech_recognizer_processing.cc) compiled in.
#
#   tools/voice_bench/aec/build.sh [WORK_DIR]
#
# WORK_DIR (default ~/.cache/molt-aec) gets the pinned source tarball's
# third_party/webrtc and abseil-cpp (streamed from the official tarball, about
# 2 GB to download, 40 MB kept) and the objects; apm_sim lands in it. Needs
# clang, curl, xz, make. About ten minutes on four cores.
#
# Only the audio processing apm_sim uses is built: AGC1 and AGC2's voice
# detectors, which voice mode's stream turns off, are linked as stubs that
# trap if they are ever called.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="${1:-$HOME/.cache/molt-aec}"
VERSION="$("$REPO/tools/chromium/pin.sh" version)"
SRC="$WORK/chromium-$VERSION"
W="$SRC/third_party/webrtc"
T="$SRC/third_party"
mkdir -p "$WORK"

if [ ! -d "$W/modules/audio_processing/aec3" ]; then
  echo "build.sh: fetching Chromium $VERSION's WebRTC and abseil sources"
  url="https://commondatastorage.googleapis.com/chromium-browser-official/chromium-$VERSION-lite.tar.xz"
  p="chromium-$VERSION/third_party"
  curl -fsSL "$url" | xz -dc -T0 | tar x -C "$WORK" --wildcards \
    "$p/webrtc/api/*" "$p/webrtc/rtc_base/*" \
    "$p/webrtc/modules/audio_processing/*" "$p/webrtc/common_audio/*" \
    "$p/webrtc/system_wrappers/*" "$p/webrtc/modules/include/*" \
    "$p/webrtc/experiments/*" "$p/abseil-cpp/absl/*" "$p/pffft/*"
fi

B="$WORK/build-$VERSION"
mkdir -p "$B/gen/experiments" "$B/inc/base" "$B/inc/third_party" "$B/inc/media/webrtc"
# The generated list of registered field trials (only used to warn about
# unregistered ones).
cat > "$B/gen/experiments/registered_field_trials.h" <<'EOF'
#ifndef GEN_EXPERIMENTS_REGISTERED_FIELD_TRIALS_H_
#define GEN_EXPERIMENTS_REGISTERED_FIELD_TRIALS_H_
#include <array>
#include "absl/strings/string_view.h"
namespace webrtc {
inline constexpr std::array<absl::string_view, 1> kRegisteredFieldTrials = {
    "WebRTC-Aec3TransparentModeHmm"};
}  // namespace webrtc
#endif
EOF
# What the tuning file includes from Chromium, outside WebRTC.
echo "#define COMPONENT_EXPORT(x)" > "$B/inc/base/component_export.h"
ln -sfn "$W" "$B/inc/third_party/webrtc"
cp "$REPO/src/media/webrtc/molt_speech_recognizer_processing.h" "$B/inc/media/webrtc/"

# WebRTC's audio processing and what it uses: every source but tests,
# tools, other CPUs' code, the AEC dump's protobuf writer, the neural
# residual echo estimator (TFLite) and the voice detectors (stubbed below).
{
  find "$W/modules/audio_processing" "$W/common_audio" \( -name "*.cc" -o -name "*.c" \) |
    grep -vE "_unittest|unittests|/test/|_test\.|test_|mock|benchmark|fuzz|_neon|_mips|_arm|/arm/|/mips/|aec_dump_impl|capture_stream_info|aec_dump/aec_dump|debug_dump|neural|tflite|ml_|_tool\.cc|/rnn_vad/|agc2/vad_wrapper\.cc|vad/voice_activity_detector\.cc|vad/vad_audio_proc\.cc|allocation_counter|/android/|_win\.|/ios/|windows_"
  for f in api/audio/audio_processing.cc api/audio/builtin_audio_processing_builder.cc \
           api/audio/echo_canceller3_config.cc api/audio/echo_canceller3_factory.cc \
           api/audio/echo_detector_creator.cc api/audio/audio_processing_statistics.cc \
           api/audio/channel_layout.cc api/audio/audio_frame.cc \
           api/environment/environment_factory.cc api/environment/force_test_environment.cc \
           api/environment/deprecated_global_field_trials.cc api/field_trials.cc \
           api/field_trials_registry.cc api/rtc_event_log/rtc_event_log.cc \
           api/task_queue/task_queue_base.cc api/task_queue/default_task_queue_factory_stdlib.cc \
           api/units/time_delta.cc api/units/timestamp.cc api/units/data_rate.cc \
           api/units/frequency.cc api/units/data_size.cc \
           rtc_base/checks.cc rtc_base/logging.cc rtc_base/platform_thread_types.cc \
           rtc_base/string_encode.cc rtc_base/string_utils.cc rtc_base/string_to_number.cc \
           rtc_base/strings/string_builder.cc rtc_base/race_checker.cc rtc_base/time_utils.cc \
           rtc_base/system_time.cc rtc_base/system/system_time.cc rtc_base/memory/aligned_malloc.cc \
           rtc_base/experiments/field_trial_parser.cc rtc_base/experiments/field_trial_units.cc \
           rtc_base/event.cc rtc_base/event_tracer.cc \
           rtc_base/synchronization/sequence_checker_internal.cc \
           rtc_base/synchronization/yield_policy.cc rtc_base/synchronization/yield.cc \
           rtc_base/platform_thread.cc rtc_base/task_queue_stdlib.cc \
           rtc_base/numerics/histogram_percentile_counter.cc rtc_base/cpu_info.cc \
           rtc_base/denormal_disabler.cc rtc_base/ref_counter.cc rtc_base/system/file_wrapper.cc \
           rtc_base/random.cc system_wrappers/source/metrics.cc system_wrappers/source/clock.cc; do
    [ -f "$W/$f" ] && echo "$W/$f"
  done
  for f in strings/numbers.cc strings/internal/charconv_bigint.cc strings/internal/charconv_parse.cc \
           strings/charconv.cc strings/ascii.cc strings/str_cat.cc strings/match.cc \
           strings/string_view.cc strings/internal/memutil.cc strings/internal/utf8.cc \
           strings/internal/str_format/arg.cc strings/internal/str_format/bind.cc \
           strings/internal/str_format/extension.cc strings/internal/str_format/float_conversion.cc \
           strings/internal/str_format/output.cc strings/internal/str_format/parser.cc \
           base/internal/raw_logging.cc base/throw_delegate.cc numeric/int128.cc; do
    [ -f "$T/abseil-cpp/absl/$f" ] && echo "$T/abseil-cpp/absl/$f"
  done
  echo "$T/pffft/src/pffft.c"
  echo "$REPO/src/media/webrtc/molt_speech_recognizer_processing.cc"
  echo "$HERE/apm_sim.cc"
  echo "$HERE/stubs.cc"
} | sort -u > "$B/sources.txt"

DEFS="-DWEBRTC_POSIX -DWEBRTC_LINUX -DWEBRTC_APM_DEBUG_DUMP=0 -DNDEBUG -DRTC_DCHECK_IS_ON=0 \
-DWEBRTC_ENABLE_AVX2 -DWEBRTC_ARCH_X86_FAMILY -DWEBRTC_HAS_SSE2 -DRTC_DISABLE_TRACE_EVENTS"
INC="-I$W -I$T/abseil-cpp -I$T/.. -I$T -I$B/gen -I$B/inc"
cat > "$B/Makefile" <<EOF
CXXFLAGS=-std=c++20 -O2 -w -fPIC -include math.h $DEFS $INC
CFLAGS=-O2 -w -fPIC $DEFS $INC
OBJS=\$(foreach s,\$(shell cat sources.txt),obj/\$(subst /,_,\$(s)).o)
all: \$(OBJS)
EOF
# The rules' $(CFLAGS), $< and $@ are make's, written as is.
# shellcheck disable=SC2016
while read -r s; do
  o="obj/$(echo "$s" | tr / _).o"
  case "$s" in
    *.c) echo "$o: $s"; printf '\t@mkdir -p obj && clang $(CFLAGS) -c $< -o $@\n' ;;
    *avx2*) echo "$o: $s"; printf '\t@mkdir -p obj && clang++ $(CXXFLAGS) -mavx2 -mfma -c $< -o $@\n' ;;
    *) echo "$o: $s"; printf '\t@mkdir -p obj && clang++ $(CXXFLAGS) -c $< -o $@\n' ;;
  esac
done < "$B/sources.txt" >> "$B/Makefile"
make -C "$B" -j"$(nproc)" -s

# Link; whatever AGC1's and AGC2's voice detectors leave undefined becomes a
# trap (voice mode's stream never creates them).
cd "$B"
link() { clang++ -o "$WORK/apm_sim" obj/*.o "$@" -lpthread -Wl,--no-demangle; }
if ! link 2> link.err; then
  grep -oE "undefined reference to \`[^']+'" link.err | sed "s/undefined reference to \`//; s/'$//" |
    sort -u > undefined.txt
  [ -s undefined.txt ] || { cat link.err >&2; exit 1; }
  {
    echo '.section .note.GNU-stack,"",@progbits'
    echo ".text"
    while read -r s; do printf '.globl %s\n.type %s,@function\n%s:\n  ud2\n' "$s" "$s" "$s"; done < undefined.txt
  } > traps.s
  clang -c traps.s -o traps.o
  link traps.o
fi
echo "build.sh: $WORK/apm_sim"
