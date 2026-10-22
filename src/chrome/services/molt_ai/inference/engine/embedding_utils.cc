// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/embedding_utils.h"

#include <cmath>
#include <utility>

namespace molt_ai {

std::vector<int32_t> FitEmbeddingTokens(std::vector<int32_t> tokens,
                                        size_t max_tokens,
                                        bool keep_last,
                                        int32_t terminator) {
  if (max_tokens < 2) max_tokens = 2;
  bool add_terminator = terminator >= 0;
  if (add_terminator && !tokens.empty() && tokens.back() == terminator) {
    // Already terminated (the tokenizer adds it): it must survive the cut.
    add_terminator = false;
    keep_last = true;
  }
  const size_t budget = max_tokens - (add_terminator ? 1 : 0);
  if (tokens.size() > budget) {
    if (keep_last) {
      const int32_t last = tokens.back();
      tokens.resize(budget - 1);
      tokens.push_back(last);
    } else {
      tokens.resize(budget);
    }
  }
  if (add_terminator) tokens.push_back(terminator);
  return tokens;
}

bool TruncateAndNormalize(const float* values,
                          size_t native,
                          size_t dimensions,
                          float* out) {
  if (!values || !out || dimensions == 0 || dimensions > native) return false;
  double sum_sq = 0.0;
  for (size_t i = 0; i < dimensions; ++i) {
    const double v = values[i];
    if (!std::isfinite(v)) return false;
    sum_sq += v * v;
  }
  if (!std::isfinite(sum_sq)) return false;
  if (!(sum_sq > 0.0)) {
    for (size_t i = 0; i < dimensions; ++i) out[i] = 0.0f;
    return true;
  }
  const double inv = 1.0 / std::sqrt(sum_sq);
  for (size_t i = 0; i < dimensions; ++i) {
    out[i] = static_cast<float>(values[i] * inv);
  }
  return true;
}

}  // namespace molt_ai
