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
// TODO(pin): not pinned yet (huggingface.co was unreachable where this was
// written): revision, sha256 and size are EMPTY, so the model is not
// offered at all (fail closed, IsPinned()). Fill them in with
//   tools/models/pin_models.py --only qwen3-embedding-0.6b --write
// on a machine that can reach huggingface.co, confirming the repo and file
// names below at the same time; never type a hash in by hand. The entry
// keeps pin_models.py's layout (id, name, repo, file, revision, sha256,
// size on their own lines).
constexpr EmbeddingModelSpec kEmbeddingModels[] = {
    {"qwen3-embedding-0.6b",
     "Qwen3-Embedding 0.6B",
     "Qwen/Qwen3-Embedding-0.6B-GGUF",  // TODO(pin): confirm repo name
     "Qwen3-Embedding-0.6B-Q8_0.gguf",  // TODO(pin): confirm file name
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/0ULL,
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
