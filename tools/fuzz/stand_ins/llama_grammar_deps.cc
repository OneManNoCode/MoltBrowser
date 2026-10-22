// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN: the few symbols the vendored
// src/llama-grammar.cpp needs from the rest of llama.cpp and ggml, so
// tools/fuzz/molt_fuzz.py can link the real grammar engine on its own
// (the GN fuzzer_test links //third_party/llama_cpp instead).
//
// The grammars are created with a null vocabulary, so the vocabulary is
// only reached for token references (<[id]>), which the converter never
// writes: those calls abort.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "llama-impl.h"
#include "llama-vocab.h"

void llama_log_internal(ggml_log_level level, const char* format, ...) {
  if (!std::getenv("MOLT_FUZZ_LLAMA_LOG")) return;
  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
}

void ggml_abort(const char* file, int line, const char* fmt, ...) {
  std::fprintf(stderr, "%s:%d: GGML_ABORT: ", file, line);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fprintf(stderr, "\n");
  std::abort();
}

bool llama_vocab::is_eog(llama_token) const {
  std::abort();
}

int32_t llama_vocab::tokenize(const char*,
                              int32_t,
                              llama_token*,
                              int32_t,
                              bool,
                              bool) const {
  std::abort();
}

const std::string& llama_vocab::token_to_piece(llama_token) const {
  std::abort();
}
