// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What the vendored llama-mmap.cpp needs from ggml and llama-impl.cpp, for
// mmap_under_seccomp only: logging to stderr, and an abort.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "llama-impl.h"

extern "C" {

[[noreturn]] void ggml_abort(const char* file, int line, const char*, ...) {
  std::fprintf(stderr, "ggml_abort at %s:%d\n", file, line);
  std::abort();
}

FILE* ggml_fopen(const char* name, const char* mode) {
  return std::fopen(name, mode);
}

int64_t ggml_time_us(void) {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t{t.tv_sec} * 1000000 + t.tv_nsec / 1000;
}

}  // extern "C"

void llama_log_internal(ggml_log_level, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
}

std::string format(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return buf;
}
