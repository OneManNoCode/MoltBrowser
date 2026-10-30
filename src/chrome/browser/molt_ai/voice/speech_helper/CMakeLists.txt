# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# molt-speech, touchless browsing's resident speech helper
# (docs/design/SPEECH_HELPER_PROTOCOL.md). Built outside GN, like
# whisper-cli: scripts/bundle-speech.sh builds ONNX Runtime and sherpa-onnx
# from pinned sources, then this, linked statically against both:
#
#   cmake -S src/chrome/browser/molt_ai/voice/speech_helper -B <build> \
#         -DMOLT_SPEECH_SHERPA_PREFIX=<sherpa-onnx install prefix> \
#         -DMOLT_SPEECH_ORT_LIB_DIR=<dir with the combined libonnxruntime.a>
#
# With -DMOLT_SPEECH_TESTS=ON (and neither prefix) it builds only the
# std-only parts' unit tests against the system's GoogleTest instead: the
# same *_unittest.cc files molt_ai_unittests runs.

cmake_minimum_required(VERSION 3.18)
project(molt_speech CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(MOLT_SPEECH_TESTS "Build the unit tests of the std-only parts" OFF)
set(MOLT_SPEECH_SHERPA_PREFIX "" CACHE PATH "sherpa-onnx install prefix")
set(MOLT_SPEECH_ORT_LIB_DIR "" CACHE PATH "Where libonnxruntime.a is")

# Includes are written from the repository's src/, as in GN.
get_filename_component(MOLT_SRC_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

# The std-only code GN also builds (BUILD.gn, source_set("core")).
add_library(molt_speech_core STATIC
  frame_reader.cc
  listener.cc
  speech_config.cc
  speech_events.cc
  speech_text.cc
)
target_include_directories(molt_speech_core PUBLIC "${MOLT_SRC_ROOT}")
if(MSVC)
  target_compile_options(molt_speech_core PRIVATE /W4)
else()
  target_compile_options(molt_speech_core PRIVATE
    -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
    -fno-exceptions)
endif()

if(MOLT_SPEECH_TESTS)
  find_package(GTest REQUIRED)
  # Chromium's include path for GoogleTest.
  set(_shim "${CMAKE_CURRENT_BINARY_DIR}/gtest_shim")
  file(WRITE "${_shim}/testing/gtest/include/gtest/gtest.h"
       "#include <gtest/gtest.h>\n")
  add_executable(molt_speech_unittests
    frame_reader_unittest.cc
    listener_unittest.cc
    speech_config_unittest.cc
    speech_events_unittest.cc
    speech_text_unittest.cc
  )
  target_include_directories(molt_speech_unittests PRIVATE "${_shim}")
  target_link_libraries(molt_speech_unittests PRIVATE molt_speech_core
                        GTest::gtest GTest::gtest_main)
  return()
endif()

if(NOT MOLT_SPEECH_SHERPA_PREFIX OR NOT MOLT_SPEECH_ORT_LIB_DIR)
  message(FATAL_ERROR "Set MOLT_SPEECH_SHERPA_PREFIX and "
                      "MOLT_SPEECH_ORT_LIB_DIR (scripts/bundle-speech.sh does)")
endif()

find_package(Threads REQUIRED)

add_executable(molt-speech
  standalone/main.cc
  standalone/output_queue.cc
  standalone/sherpa_engines.cc
)
target_include_directories(molt-speech PRIVATE
  "${MOLT_SRC_ROOT}" "${MOLT_SPEECH_SHERPA_PREFIX}/include")
target_link_libraries(molt-speech PRIVATE molt_speech_core)

# sherpa-onnx's static libraries (its sherpa-onnx-static-no-tts.pc), then
# ONNX Runtime's.
set(_sherpa_libs
  sherpa-onnx-c-api sherpa-onnx-core kaldi-decoder-core
  sherpa-onnx-kaldifst-core sherpa-onnx-fstfar sherpa-onnx-fst
  kaldi-native-fbank-core kissfft-float ssentencepiece_core utf8proc)
set(_libs)
foreach(_name IN LISTS _sherpa_libs)
  find_library(_lib_${_name} NAMES ${_name} lib${_name}
               PATHS "${MOLT_SPEECH_SHERPA_PREFIX}/lib" NO_DEFAULT_PATH
               REQUIRED)
  list(APPEND _libs "${_lib_${_name}}")
endforeach()
find_library(_lib_onnxruntime NAMES onnxruntime
             PATHS "${MOLT_SPEECH_ORT_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
list(APPEND _libs "${_lib_onnxruntime}")

# Hardening, for all the code this file compiles into the helper: the core
# (the frame reader and the CONFIG parser, which read the browser's input)
# as well as the program, whatever the toolchain's defaults are. Not for
# the unit tests above, which are built with sanitizers.
#   - Linux and macOS: stack protector (strong), _FORTIFY_SOURCE (3 where
#     the compiler supports it: GCC 12 and Clang; macOS's libc reads 2),
#     stack clash protection where the compiler has it, a position-
#     independent executable; on Linux also full RELRO with immediate
#     binding and a non-executable stack (below).
#   - Windows: /GS (MSVC's default, stated) and Control Flow Guard; ASLR
#     (/DYNAMICBASE, /HIGHENTROPYVA) and DEP (/NXCOMPAT) are the linker's
#     defaults.
include(CheckCXXCompilerFlag)
include(CheckPIESupported)
check_pie_supported()
set_target_properties(molt_speech_core molt-speech PROPERTIES
                      POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  set(_hardening /GS /guard:cf)
  target_link_options(molt-speech PRIVATE /guard:cf)
else()
  set(_fortify 3)
  if(APPLE OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
               CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12))
    set(_fortify 2)
  endif()
  set(_hardening -fstack-protector-strong
                 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=${_fortify})
  check_cxx_compiler_flag(-fstack-clash-protection
                          MOLT_SPEECH_HAVE_STACK_CLASH_PROTECTION)
  if(MOLT_SPEECH_HAVE_STACK_CLASH_PROTECTION)
    list(APPEND _hardening -fstack-clash-protection)
  endif()
endif()
target_compile_options(molt_speech_core PRIVATE ${_hardening})
target_compile_options(molt-speech PRIVATE ${_hardening})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(molt-speech PRIVATE
    -Wl,--start-group ${_libs} -Wl,--end-group
    Threads::Threads ${CMAKE_DL_LIBS} m)
  # One file: the C++ runtime inside, glibc from the system.
  target_link_options(molt-speech PRIVATE
    -static-libstdc++ -static-libgcc
    -Wl,--gc-sections -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
  target_compile_options(molt-speech PRIVATE
    -ffunction-sections -fdata-sections)
elseif(APPLE)
  target_link_libraries(molt-speech PRIVATE ${_libs} Threads::Threads
                        "-framework Foundation")
  target_link_options(molt-speech PRIVATE -Wl,-dead_strip)
elseif(WIN32)
  target_link_libraries(molt-speech PRIVATE ${_libs})
  set_property(TARGET molt-speech PROPERTY
               MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

if(NOT MSVC)
  target_compile_options(molt-speech PRIVATE -Wall -Wextra)
endif()
