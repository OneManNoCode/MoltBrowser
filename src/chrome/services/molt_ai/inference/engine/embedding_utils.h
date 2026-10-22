// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Std-only helpers of the embedding engine (llama_embedding_engine.cc):
// fitting one text's tokens into the model's window, and turning the
// model's pooled vector into what the browser gets. No llama.cpp, so they
// are unit-tested standalone (embedding_utils_unittest.cc).

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_EMBEDDING_UTILS_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_EMBEDDING_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace molt_ai {

// The tokens to embed for one text, at most |max_tokens| (>= 2) of them:
//  - a text that is too long keeps its first tokens; when |keep_last| (the
//    tokenizer ended it with a marker such as BERT's [SEP] or an EOS) the
//    last token is kept as well;
//  - |terminator| (a token id, or -1 for none) ends the result unless the
//    tokens already end with it; it is never cut off.
// Empty only for empty |tokens| without a terminator.
std::vector<int32_t> FitEmbeddingTokens(std::vector<int32_t> tokens,
                                        size_t max_tokens,
                                        bool keep_last,
                                        int32_t terminator);

// Writes the first |dimensions| of the |native| values to |out|, scaled to
// an L2 norm of 1 (Matryoshka truncation: the leading components of such a
// model's vector are a vector of their own). A vector whose norm is 0 stays
// all zeros. Returns false, writing nothing, when |dimensions| is 0 or more
// than |native|, or a value used isn't finite (or squares to infinity).
bool TruncateAndNormalize(const float* values,
                          size_t native,
                          size_t dimensions,
                          float* out);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_EMBEDDING_UTILS_H_
