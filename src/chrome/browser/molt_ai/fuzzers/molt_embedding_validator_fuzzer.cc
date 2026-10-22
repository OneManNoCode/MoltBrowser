// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's checks on what the sandboxed embedding service sends back
// (runtime/embedding_validator.h): a model description and a flat list of
// floats per request, both hostile.
//
// Checks: an accepted reply has exactly texts * dimensions finite values
// and each vector is unit length or all zeros; SplitEmbeddingReply() never
// reads past the values and splits an accepted reply into |texts| vectors
// of |dimensions|; an accepted description matches what was asked for.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/runtime/embedding_validator.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider p(data, size);

  const size_t texts = p.ConsumeBool() ? p.ConsumeIntegral<size_t>()
                                       : p.ConsumeIntegralInRange<size_t>(0, 16);
  const size_t dimensions = p.ConsumeBool()
                                ? p.ConsumeIntegral<size_t>()
                                : p.ConsumeIntegralInRange<size_t>(0, 64);
  std::vector<float> values(p.ConsumeIntegralInRange<size_t>(0, 1024));
  for (float& v : values) {
    switch (p.ConsumeIntegralInRange<int>(0, 5)) {
      case 0:
        v = NAN;
        break;
      case 1:
        v = INFINITY;
        break;
      case 2:
        v = 0.0f;
        break;
      default:
        v = p.ConsumeFloatingPointInRange<float>(-2.0f, 2.0f);
        break;
    }
  }
  // Sometimes a well-formed reply: normalize the rows.
  if (p.ConsumeBool() && dimensions > 0 && dimensions <= 64) {
    for (size_t at = 0; at + dimensions <= values.size(); at += dimensions) {
      double sum = 0.0;
      for (size_t i = 0; i < dimensions; ++i) {
        if (std::isfinite(values[at + i])) {
          sum += double{values[at + i]} * values[at + i];
        }
      }
      for (size_t i = 0; i < dimensions; ++i) {
        values[at + i] = sum > 0.0 && std::isfinite(values[at + i])
                             ? static_cast<float>(values[at + i] / std::sqrt(sum))
                             : 0.0f;
      }
    }
  }

  const EmbeddingReplyError error = CheckEmbeddingReply(values, texts, dimensions);
  CHECK(EmbeddingReplyErrorName(error) != nullptr);
  const std::vector<std::vector<float>> split =
      SplitEmbeddingReply(values, dimensions);
  if (dimensions == 0) {
    CHECK(split.empty());
  } else {
    CHECK(split.size() == values.size() / dimensions);
    for (const std::vector<float>& row : split) CHECK(row.size() == dimensions);
  }
  if (error == EmbeddingReplyError::kNone) {
    CHECK(dimensions > 0 && values.size() == texts * dimensions);
    CHECK(split.size() == texts);
    for (const std::vector<float>& row : split) {
      double norm = 0.0;
      bool zeros = true;
      for (float v : row) {
        CHECK(std::isfinite(v));
        norm += double{v} * v;
        zeros = zeros && v == 0.0f;
      }
      CHECK(zeros || std::fabs(norm - 1.0) <= kEmbeddingNormTolerance);
    }
  }

  EmbeddingExpectations expected;
  expected.dimensions = p.ConsumeIntegralInRange<uint32_t>(0, 4096);
  expected.max_tokens_per_text = p.ConsumeIntegralInRange<uint32_t>(0, 4096);
  expected.native_dimensions = p.ConsumeIntegralInRange<uint32_t>(0, 70000);
  expected.architecture = p.ConsumeRandomLengthString(16);
  const std::string arch = p.ConsumeRandomLengthString(80);
  const std::string backend = p.ConsumeRandomLengthString(8);
  const uint32_t native = p.ConsumeIntegral<uint32_t>();
  const uint32_t dims = p.ConsumeIntegral<uint32_t>();
  const uint32_t max_tokens = p.ConsumeIntegral<uint32_t>();
  const uint32_t vocab = p.ConsumeIntegral<uint32_t>();
  if (IsValidEmbeddingDescription(arch, backend, native, dims, max_tokens,
                                  vocab, expected)) {
    CHECK(dims == expected.dimensions && dims > 0 && native >= dims);
    CHECK(max_tokens <= expected.max_tokens_per_text);
    CHECK(expected.architecture.empty() || arch == expected.architecture);
    CHECK(!arch.empty() && arch.size() <= 64);
  }
  return 0;
}
