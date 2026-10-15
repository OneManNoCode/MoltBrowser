// Stand-in for the header upstream CMake generates from
// src/llama-version.h.in (configure_file in src/CMakeLists.txt).
// Values for the vendored tag (see README.chromium): LLAMA_VERSION is what a
// default (LLAMA_BUILD_IS_DEV=ON) upstream build of that tag reports, from
// LLAMA_VERSION_{MAJOR,MINOR,PATCH} in llama.cpp's top-level CMakeLists.txt;
// LLAMA_COMMIT is the tag's short hash. Update both with every llama.cpp
// update.
#pragma once

#define LLAMA_VERSION "0.6.0-dev"
#define LLAMA_COMMIT  "843e7dc"
