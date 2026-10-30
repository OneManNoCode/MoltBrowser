# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# molt-voice, voice mode's own voice (docs/design/VOICE_HELPER_PROTOCOL.md).
# Built outside GN, like molt-speech: scripts/bundle-speech.sh builds ONNX
# Runtime and sherpa-onnx (with its TTS code) from pinned sources, then
# this, linked statically against both:
#
#   cmake -S src/chrome/browser/molt_ai/voice/voice_helper -B <build> \
#         -DMOLT_VOICE_SHERPA_PREFIX=<sherpa-onnx install prefix> \
#         -DMOLT_VOICE_ORT_LIB_DIR=<dir with the combined libonnxruntime.a>
#
# With -DMOLT_VOICE_TESTS=ON (and neither prefix) it builds only the
# std-only parts' unit tests against the system's GoogleTest instead: the
# same *_unittest.cc files molt_ai_unittests runs.

cmake_minimum_required(VERSION 3.18)
project(molt_voice CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(MOLT_VOICE_TESTS "Build the unit tests of the std-only parts" OFF)
set(MOLT_VOICE_SHERPA_PREFIX "" CACHE PATH "sherpa-onnx install prefix")
set(MOLT_VOICE_ORT_LIB_DIR "" CACHE PATH "Where libonnxruntime.a is")

# Includes are written from the repository's src/, as in GN.
get_filename_component(MOLT_SRC_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# The std-only code GN also builds (BUILD.gn, source_set("core")).
add_library(molt_voice_core STATIC
  voice_frames.cc
  voice_lts.cc
  voice_phonemes.cc
  voice_synth.cc
  voice_text.cc
)
target_include_directories(molt_voice_core PUBLIC "${MOLT_SRC_ROOT}")
if(MSVC)
  target_compile_options(molt_voice_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(molt_voice_core PRIVATE
    -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
    -fno-exceptions)
endif()

if(MOLT_VOICE_TESTS)
  find_package(GTest REQUIRED)
  # Chromium's include path for GoogleTest.
  set(_shim "${CMAKE_CURRENT_BINARY_DIR}/gtest_shim")
  file(WRITE "${_shim}/testing/gtest/include/gtest/gtest.h"
       "#include <gtest/gtest.h>\n")
  add_executable(molt_voice_unittests
    voice_frames_unittest.cc
    voice_lts_unittest.cc
    voice_phonemes_unittest.cc
    voice_synth_unittest.cc
    voice_text_unittest.cc
  )
  target_include_directories(molt_voice_unittests PRIVATE "${_shim}")
  target_link_libraries(molt_voice_unittests PRIVATE molt_voice_core
                        GTest::gtest GTest::gtest_main)
  return()
endif()

if(NOT MOLT_VOICE_SHERPA_PREFIX OR NOT MOLT_VOICE_ORT_LIB_DIR)
  message(FATAL_ERROR "Set MOLT_VOICE_SHERPA_PREFIX and "
                      "MOLT_VOICE_ORT_LIB_DIR (scripts/bundle-speech.sh does)")
endif()

find_package(Threads REQUIRED)

add_executable(molt-voice
  standalone/frame_output.cc
  standalone/main.cc
  standalone/sherpa_voice.cc
)
target_include_directories(molt-voice PRIVATE
  "${MOLT_SRC_ROOT}" "${MOLT_VOICE_SHERPA_PREFIX}/include")
target_link_libraries(molt-voice PRIVATE molt_voice_core)

# sherpa-onnx's static libraries (its sherpa-onnx-static.pc), then ONNX
# Runtime's.
set(_sherpa_libs
  sherpa-onnx-c-api sherpa-onnx-core kaldi-decoder-core
  sherpa-onnx-kaldifst-core sherpa-onnx-fstfar sherpa-onnx-fst
  kaldi-native-fbank-core kissfft-float ssentencepiece_core utf8proc)
set(_libs)
foreach(_name IN LISTS _sherpa_libs)
  find_library(_lib_${_name} NAMES ${_name} lib${_name}
               PATHS "${MOLT_VOICE_SHERPA_PREFIX}/lib" NO_DEFAULT_PATH
               REQUIRED)
  list(APPEND _libs "${_lib_${_name}}")
endforeach()
find_library(_lib_onnxruntime NAMES onnxruntime
             PATHS "${MOLT_VOICE_ORT_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
list(APPEND _libs "${_lib_onnxruntime}")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(molt-voice PRIVATE
    -Wl,--start-group ${_libs} -Wl,--end-group
    Threads::Threads ${CMAKE_DL_LIBS} m)
  # One file: the C++ runtime inside, glibc from the system.
  target_link_options(molt-voice PRIVATE
    -static-libstdc++ -static-libgcc
    -Wl,--gc-sections -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
  target_compile_options(molt-voice PRIVATE
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 -ffunction-sections
    -fdata-sections)
elseif(APPLE)
  target_link_libraries(molt-voice PRIVATE ${_libs} Threads::Threads
                        "-framework Foundation")
  target_link_options(molt-voice PRIVATE -Wl,-dead_strip)
elseif(WIN32)
  target_link_libraries(molt-voice PRIVATE ${_libs})
  set_property(TARGET molt-voice PROPERTY
               MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

if(MSVC)
  target_compile_options(molt-voice PRIVATE /utf-8)
else()
  target_compile_options(molt-voice PRIVATE -Wall -Wextra)
endif()
