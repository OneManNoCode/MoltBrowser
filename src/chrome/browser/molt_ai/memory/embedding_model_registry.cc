// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/embedding_model_registry.h"

#include <cstring>

#include "chrome/browser/molt_ai/memory/memory_types.h"

namespace molt_ai {
namespace memory {

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024;

// Qwen3-Embedding 0.6B (Alibaba Qwen team), Apache 2.0: multilingual (100+
// languages), 1024-dimensional vectors trained for Matryoshka truncation
// (we keep 256), last-token pooling over a trailing <|endoftext|>, and an
// instruction on queries only. Official GGUF from the Qwen organization,
// Q8_0. Why this model: docs/design/SEMANTIC_SEARCH.md ("Choosing the
// model").
//
// Pinned (November 2026) by tools/models/pin_models.py from the Hugging Face
// API reply the model-pin workflow fetched (.github/workflows/
// molt-model-pin.yml); the file was then downloaded and hashed on the
// runner against the pin, and its GGUF header read (tools/models/
// hf_survey.py verify). Without all three values the model is not offered
// at all (fail closed, IsPinned()). Re-pin the same way, never by hand. The
// entry keeps pin_models.py's layout (id, name, repo, file, revision,
// sha256, size on their own lines).
// clang-format off: tools/models/pin_models.py reads and rewrites these
// entries one value per line.
constexpr EmbeddingModelSpec kEmbeddingModels[] = {
    {"qwen3-embedding-0.6b",
     "Qwen3-Embedding 0.6B",
     "Qwen/Qwen3-Embedding-0.6B-GGUF",
     "Qwen3-Embedding-0.6B-Q8_0.gguf",
     /*revision=*/"370f27d7550e0def9b39c1f16d3fbaa13aa67728",
     /*sha256=*/"06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439",
     /*size=*/639150592ULL,
     /*approx_size_bytes=*/640ULL * 1000 * 1000,
     /*ram_required_bytes=*/1536 * kMiB,
     "Apache License 2.0",
     "https://www.apache.org/licenses/LICENSE-2.0",
     "Qwen",
     EmbeddingPoolingKind::kLast,
     /*terminator=*/"<|endoftext|>",
     /*max_tokens_per_text=*/512,
     /*native_dimensions=*/1024,
     /*dimensions=*/static_cast<uint32_t>(kEmbeddingDim),
     /*architecture=*/"qwen3",
     /*query_prefix=*/
     "Instruct: Given a web search query, retrieve relevant passages that "
     "answer the query\nQuery:",
     /*document_prefix=*/""},
};
// clang-format on

bool IsLowerHex(const char* s, size_t length) {
  if (!s || std::strlen(s) != length) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

const EmbeddingModelSpec& DefaultEmbeddingModel() {
  return kEmbeddingModels[0];
}

bool IsPinned(const EmbeddingModelSpec& spec) {
  return IsLowerHex(spec.revision, 40) && IsLowerHex(spec.sha256, 64) &&
         spec.file_size_bytes > 0;
}

EmbedderSpace SpaceOf(const EmbeddingModelSpec& spec) {
  EmbedderSpace space;
  space.model = std::string(spec.model_id) + "@" +
                std::string(spec.sha256 ? spec.sha256 : "").substr(0, 12);
  space.dimensions = spec.dimensions;
  return space;
}

}  // namespace memory
}  // namespace molt_ai
