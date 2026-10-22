// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The on-device embedding model behind "Search by meaning" in AI memory
// (docs/design/SEMANTIC_SEARCH.md): which GGUF, its supply-chain pin, its
// licence, and how the inference service must run it. Std-only.
//
// Fail closed: the model is offered, downloaded and loaded only when its
// Hugging Face revision, SHA-256 and exact size are ALL pinned
// (IsPinned()). Until then memory search uses HashingEmbedder and the
// memory page says the model isn't available in this build. The pin is
// filled in on a machine that can reach huggingface.co with
//   tools/models/pin_models.py --only qwen3-embedding-0.6b --write
// (it rewrites the entry in embedding_model_registry.cc); the full
// procedure, including the independent check of the hash, is in
// docs/design/SEMANTIC_SEARCH.md ("Pinning the model").

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_MODEL_REGISTRY_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_MODEL_REGISTRY_H_

#include <cstdint>
#include <string>

#include "chrome/browser/molt_ai/memory/memory_embedder.h"

namespace molt_ai {
namespace memory {

// molt_ai.mojom.EmbeddingPooling, without the mojom dependency.
enum class EmbeddingPoolingKind { kMean, kCls, kLast };

struct EmbeddingModelSpec {
  const char* model_id;
  const char* display_name;
  const char* huggingface_id;
  const char* filename;
  // Pinned Hugging Face commit (40 hex digits); "" = not pinned.
  const char* revision;
  // SHA-256 of the GGUF (64 lowercase hex digits); "" = not pinned.
  const char* sha256;
  // Exact size in bytes; 0 = not pinned.
  uint64_t file_size_bytes;
  // Shown before the exact size is known.
  uint64_t approx_size_bytes;
  // Free memory needed to start the model (weights, a one-text context,
  // compute buffers).
  uint64_t ram_required_bytes;
  const char* license_name;
  const char* license_url;
  const char* company;

  // How the inference service runs it (EmbeddingModelLoadParams).
  EmbeddingPoolingKind pooling;
  // Special token ending every text ("" = none).
  const char* terminator;
  uint32_t max_tokens_per_text;
  // The model's own width, and the leading components kept (Matryoshka):
  // kEmbeddingDim, so the hot index stays 1 KB per chunk.
  uint32_t native_dimensions;
  uint32_t dimensions;
  // GGUF general.architecture the loaded file must report.
  const char* architecture;

  // Prepended to queries / documents, as the model was trained.
  const char* query_prefix;
  const char* document_prefix;
};

// The model memory uses (there is one).
const EmbeddingModelSpec& DefaultEmbeddingModel();

// Revision, SHA-256 and exact size are all pinned (fail closed otherwise).
bool IsPinned(const EmbeddingModelSpec& spec);

// The space of the model's vectors: "<model_id>@<first 12 hex digits of
// its SHA-256>" (a re-pinned file is a new space), |dimensions| long.
// Only meaningful for a pinned spec.
EmbedderSpace SpaceOf(const EmbeddingModelSpec& spec);

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_MODEL_REGISTRY_H_
