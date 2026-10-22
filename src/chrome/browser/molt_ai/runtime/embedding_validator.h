// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Browser-side checks on what the sandboxed embedding model sends
// (docs/design/SEMANTIC_SEARCH.md). The process is treated as hostile, as
// for generations (stream_validator.h): a compromised process can send any
// values mojo's validation lets through, and these vectors end up in the
// memory index and decide what the user is shown.
//
// Std-only, so it is unit-tested standalone.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_EMBEDDING_VALIDATOR_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_EMBEDDING_VALIDATOR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {

enum class EmbeddingReplyError {
  kNone,
  // Not texts * dimensions values.
  kWrongCount,
  // NaN or an infinity.
  kNotFinite,
  // A vector that is neither unit length nor all zeros.
  kNotNormalized,
};

const char* EmbeddingReplyErrorName(EmbeddingReplyError error);

// How far a vector's squared norm may be from 1 (float rounding over a few
// thousand components stays far below this).
inline constexpr double kEmbeddingNormTolerance = 1e-3;

// An Embed() reply for |texts| texts of |dimensions| (> 0) values each: the
// count must match, every value must be finite, and every vector must be
// L2-normalized or all zeros (a text with no tokens).
EmbeddingReplyError CheckEmbeddingReply(const std::vector<float>& values,
                                        size_t texts,
                                        size_t dimensions);

// What the browser asked the model for, to check its description against.
struct EmbeddingExpectations {
  // As requested (exact).
  uint32_t dimensions = 0;
  // As requested (the service may lower it, never raise it).
  uint32_t max_tokens_per_text = 0;
  // From the model registry; 0 / "" = not checked.
  uint32_t native_dimensions = 0;
  std::string architecture;
};

// An EmbeddingModelDescription is valid when: the architecture is 1-64
// bytes of [a-z0-9_.-] (and the expected one, if any); the backend is
// "metal", "cpu" or "vulkan"; dimensions are exactly as requested and the
// model's own width is at least that and at most 65536 (and the expected
// one, if any); max_tokens_per_text is within [8, requested]; and the vocab
// size is within (0, 2^22].
bool IsValidEmbeddingDescription(std::string_view architecture,
                                 std::string_view backend,
                                 uint32_t native_dimensions,
                                 uint32_t dimensions,
                                 uint32_t max_tokens_per_text,
                                 uint32_t vocab_size,
                                 const EmbeddingExpectations& expected);

// Splits a checked reply into one vector per text.
std::vector<std::vector<float>> SplitEmbeddingReply(
    const std::vector<float>& values,
    size_t dimensions);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_EMBEDDING_VALIDATOR_H_
