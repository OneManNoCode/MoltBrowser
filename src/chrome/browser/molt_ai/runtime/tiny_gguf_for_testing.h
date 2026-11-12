// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A tiny random-weight llama-architecture GGUF, written by the test that
// uses it, for loading a real model through the sandboxed inference service
// (inference_sandbox_browsertest.cc). Its output is gibberish by design: it
// is there to load and generate, as a real model does, under the real
// sandbox. About 300 KB: one block, 64-wide, a SentencePiece vocabulary of
// 271 pieces with byte fallback (so any text tokenizes), every tensor F32.
// Deterministic. Std-only.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_TINY_GGUF_FOR_TESTING_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_TINY_GGUF_FOR_TESTING_H_

#include <cstdint>
#include <string>

namespace molt_ai {

inline constexpr uint32_t kTinyGgufVocabSize = 271;
inline constexpr uint32_t kTinyGgufContextLength = 256;

// The file's bytes.
std::string TinyLlamaGgufForTesting();

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_TINY_GGUF_FOR_TESTING_H_
