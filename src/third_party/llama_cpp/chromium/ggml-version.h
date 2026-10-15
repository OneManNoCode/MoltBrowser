// Stand-in for the header upstream CMake generates from
// ggml/src/ggml-version.h.in (configure_file in ggml/src/CMakeLists.txt).
// Values for the vendored tag (see README.chromium): GGML_VERSION is
// GGML_VERSION_{MAJOR,MINOR,PATCH} from ggml/CMakeLists.txt, GGML_COMMIT the
// short hash of the llama.cpp tag. Update both with every llama.cpp update.
#pragma once

#define GGML_VERSION "0.26.0"
#define GGML_COMMIT  "843e7dc"
