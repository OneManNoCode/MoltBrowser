// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The embedding engine's helpers (engine/embedding_utils.h) on what the
// model file and the model give it: token lists of any length and
// terminator, and pooled vectors of any floats (NaN, infinities, huge,
// subnormal).
//
// Checks: FitEmbeddingTokens() returns at most max(max_tokens, 2) tokens,
// keeps the input's head, ends with the terminator when there is one, and
// keeps a tokenizer-added last token; TruncateAndNormalize() refuses
// non-finite input and otherwise writes a unit vector (or zeros), never
// touching memory past |dimensions|.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/embedding_utils.h"

namespace {

void CheckFit(FuzzedDataProvider* provider) {
  std::vector<int32_t> tokens(provider->ConsumeIntegralInRange<size_t>(0, 4096));
  for (int32_t& t : tokens) t = provider->ConsumeIntegralInRange<int32_t>(0, 9);
  // mojom kMaxEmbedTokensPerText is 2048; the engine clamps to at least 2.
  const size_t max_tokens = provider->ConsumeIntegralInRange<size_t>(0, 4096);
  const bool keep_last = provider->ConsumeBool();
  const int32_t terminator = provider->ConsumeIntegralInRange<int32_t>(-1, 9);

  const std::vector<int32_t> out =
      molt_ai::FitEmbeddingTokens(tokens, max_tokens, keep_last, terminator);
  const size_t cap = max_tokens < 2 ? 2 : max_tokens;
  CHECK(out.size() <= cap);
  if (terminator >= 0) {
    CHECK(!out.empty() && out.back() == terminator);
  } else if (tokens.empty()) {
    CHECK(out.empty());
  }
  const bool terminated = terminator >= 0 && !tokens.empty() &&
                          tokens.back() == terminator;
  const bool keeps_last = keep_last || terminated;
  // Everything but the kept last token / added terminator is a prefix of
  // the input.
  size_t head = out.size();
  if (terminator >= 0 && !terminated) --head;
  if (tokens.size() > head && keeps_last && head > 0) {
    CHECK(out[head - 1] == tokens.back());
    --head;
  }
  for (size_t i = 0; i < head; ++i) CHECK(out[i] == tokens[i]);
  if (tokens.size() + (terminator >= 0 && !terminated ? 1 : 0) <= cap) {
    // It fits: nothing is cut.
    CHECK(out.size() == tokens.size() + (terminator >= 0 && !terminated));
  }
}

void CheckNormalize(FuzzedDataProvider* provider) {
  const size_t native = provider->ConsumeIntegralInRange<size_t>(0, 512);
  const size_t dimensions = provider->ConsumeIntegralInRange<size_t>(0, 600);
  std::vector<float> values(native);
  for (float& v : values) {
    switch (provider->ConsumeIntegralInRange<int>(0, 7)) {
      case 0:
        v = NAN;
        break;
      case 1:
        v = provider->ConsumeBool() ? INFINITY : -INFINITY;
        break;
      case 2:
        v = provider->ConsumeBool() ? 3.4e38f : 1e-45f;
        break;
      case 3:
        v = 0.0f;
        break;
      default:
        v = provider->ConsumeFloatingPointInRange<float>(-1000.0f, 1000.0f);
        break;
    }
  }
  // Exactly |dimensions| floats, so ASan sees a write past them.
  std::vector<float> out(dimensions, 7.0f);
  const bool ok = molt_ai::TruncateAndNormalize(
      values.empty() ? nullptr : values.data(), native, dimensions,
      out.empty() ? nullptr : out.data());
  if (dimensions == 0 || dimensions > native) {
    CHECK(!ok);
    return;
  }
  bool finite = true;
  for (size_t i = 0; i < dimensions; ++i) {
    if (!std::isfinite(values[i])) finite = false;
  }
  if (!finite) {
    CHECK(!ok);
    return;
  }
  if (!ok) return;  // squares overflowed
  double norm = 0.0;
  bool zeros = true;
  for (float v : out) {
    CHECK(std::isfinite(v));
    norm += double{v} * double{v};
    if (v != 0.0f) zeros = false;
  }
  CHECK(zeros || std::fabs(norm - 1.0) < 1e-3);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider provider(data, size);
  if (provider.ConsumeBool()) {
    CheckFit(&provider);
  } else {
    CheckNormalize(&provider);
  }
  return 0;
}
