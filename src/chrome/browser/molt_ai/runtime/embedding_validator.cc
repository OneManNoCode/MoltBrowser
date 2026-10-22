// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/embedding_validator.h"

#include <cmath>
#include <limits>

namespace molt_ai {

namespace {

constexpr size_t kMaxArchitectureBytes = 64;
constexpr uint32_t kMaxNativeDimensions = 65536;
constexpr uint32_t kMinTokensPerText = 8;
constexpr uint32_t kMaxVocabSize = 1u << 22;

bool IsArchitectureName(std::string_view s) {
  if (s.empty() || s.size() > kMaxArchitectureBytes) return false;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

const char* EmbeddingReplyErrorName(EmbeddingReplyError error) {
  switch (error) {
    case EmbeddingReplyError::kNone:
      return "none";
    case EmbeddingReplyError::kWrongCount:
      return "wrong number of embedding values";
    case EmbeddingReplyError::kNotFinite:
      return "embedding value is not finite";
    case EmbeddingReplyError::kNotNormalized:
      return "embedding is not normalized";
  }
  return "unknown";
}

EmbeddingReplyError CheckEmbeddingReply(const std::vector<float>& values,
                                        size_t texts,
                                        size_t dimensions) {
  if (dimensions == 0 ||
      texts > std::numeric_limits<size_t>::max() / dimensions ||
      values.size() != texts * dimensions) {
    return EmbeddingReplyError::kWrongCount;
  }
  for (size_t row = 0; row < texts; ++row) {
    const float* v = values.data() + row * dimensions;
    double sum_sq = 0.0;
    bool all_zero = true;
    for (size_t i = 0; i < dimensions; ++i) {
      if (!std::isfinite(v[i])) return EmbeddingReplyError::kNotFinite;
      all_zero = all_zero && v[i] == 0.0f;
      sum_sq += static_cast<double>(v[i]) * v[i];
    }
    if (!all_zero && std::fabs(sum_sq - 1.0) > kEmbeddingNormTolerance) {
      return EmbeddingReplyError::kNotNormalized;
    }
  }
  return EmbeddingReplyError::kNone;
}

bool IsValidEmbeddingDescription(std::string_view architecture,
                                 std::string_view backend,
                                 uint32_t native_dimensions,
                                 uint32_t dimensions,
                                 uint32_t max_tokens_per_text,
                                 uint32_t vocab_size,
                                 const EmbeddingExpectations& expected) {
  if (!IsArchitectureName(architecture)) return false;
  if (!expected.architecture.empty() &&
      architecture != expected.architecture) {
    return false;
  }
  if (backend != "metal" && backend != "cpu" && backend != "vulkan") {
    return false;
  }
  if (expected.dimensions == 0 || dimensions != expected.dimensions ||
      native_dimensions < dimensions ||
      native_dimensions > kMaxNativeDimensions) {
    return false;
  }
  if (expected.native_dimensions != 0 &&
      native_dimensions != expected.native_dimensions) {
    return false;
  }
  if (max_tokens_per_text < kMinTokensPerText ||
      max_tokens_per_text > expected.max_tokens_per_text) {
    return false;
  }
  return vocab_size > 0 && vocab_size <= kMaxVocabSize;
}

std::vector<std::vector<float>> SplitEmbeddingReply(
    const std::vector<float>& values,
    size_t dimensions) {
  std::vector<std::vector<float>> out;
  if (dimensions == 0) return out;
  out.reserve(values.size() / dimensions);
  for (size_t at = 0; at + dimensions <= values.size(); at += dimensions) {
    out.emplace_back(values.begin() + static_cast<std::ptrdiff_t>(at),
                     values.begin() +
                         static_cast<std::ptrdiff_t>(at + dimensions));
  }
  return out;
}

}  // namespace molt_ai
