# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# Builds sherpa_bench against a prebuilt sherpa-onnx release (fetched by
# fetch.py). Linux and macOS:
#   cmake -S tools/voice_bench/driver -B <data>/build \
#         -DSHERPA_ONNX_DIR=<data>/a/sherpa-onnx-linux-x64
#   cmake --build <data>/build -j 2
# Windows (x64 Native Tools prompt, or with the VS generator):
#   cmake -S tools\voice_bench\driver -B <data>\build ^
#         -DSHERPA_ONNX_DIR=<data>\a\sherpa-onnx-win-x64 ^
#         -DSHERPA_ONNX_LIB_DIR=<data>\a\sizes-win-x64-shared-lib\lib
#   cmake --build <data>\build --config Release
# run_bench.py does this for you (--build).

cmake_minimum_required(VERSION 3.16)
project(voice_bench_driver C)

set(SHERPA_ONNX_DIR "" CACHE PATH
    "An unpacked sherpa-onnx release with include/ and lib/")
set(SHERPA_ONNX_LIB_DIR "" CACHE PATH
    "Where the C API library is, if not in SHERPA_ONNX_DIR/lib")

if(NOT SHERPA_ONNX_DIR)
  message(FATAL_ERROR "Set -DSHERPA_ONNX_DIR=<unpacked sherpa-onnx release>")
endif()
if(NOT SHERPA_ONNX_LIB_DIR)
  set(SHERPA_ONNX_LIB_DIR "${SHERPA_ONNX_DIR}/lib")
endif()

find_library(SHERPA_ONNX_C_API NAMES sherpa-onnx-c-api
             PATHS "${SHERPA_ONNX_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(sherpa_bench sherpa_bench.c)
target_include_directories(sherpa_bench PRIVATE "${SHERPA_ONNX_DIR}/include")
target_link_libraries(sherpa_bench PRIVATE "${SHERPA_ONNX_C_API}")
if(WIN32)
  target_link_libraries(sherpa_bench PRIVATE psapi)
  # The DLLs have to sit next to the executable.
  file(GLOB _dlls "${SHERPA_ONNX_LIB_DIR}/*.dll")
  foreach(_dll IN LISTS _dlls)
    add_custom_command(TARGET sherpa_bench POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}"
              "$<TARGET_FILE_DIR:sherpa_bench>")
  endforeach()
else()
  set_target_properties(sherpa_bench PROPERTIES
    BUILD_RPATH "${SHERPA_ONNX_LIB_DIR}")
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sherpa_bench PRIVATE -Wall -Wextra -O2)
  endif()
endif()
