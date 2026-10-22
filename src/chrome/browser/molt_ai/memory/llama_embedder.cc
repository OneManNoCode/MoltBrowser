// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/llama_embedder.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

namespace molt_ai {
namespace memory {

namespace {

// Texts per background call: a few seconds of CPU at most for a 0.6B model
// over 512-token chunks.
constexpr size_t kBatchTexts = 8;

mojom::EmbeddingPooling ToMojo(EmbeddingPoolingKind pooling) {
  switch (pooling) {
    case EmbeddingPoolingKind::kMean:
      return mojom::EmbeddingPooling::kMean;
    case EmbeddingPoolingKind::kCls:
      return mojom::EmbeddingPooling::kCls;
    case EmbeddingPoolingKind::kLast:
      return mojom::EmbeddingPooling::kLast;
  }
  return mojom::EmbeddingPooling::kMean;
}

}  // namespace

EmbedOutcome OutcomeForStatus(EmbeddingStatus status) {
  switch (status) {
    case EmbeddingStatus::kOk:
      return EmbedOutcome::kOk;
    case EmbeddingStatus::kUnavailable:
      return EmbedOutcome::kUnavailable;
    case EmbeddingStatus::kFailed:
      return EmbedOutcome::kFailed;
    case EmbeddingStatus::kNotEnoughMemory:
    case EmbeddingStatus::kBackingOff:
    case EmbeddingStatus::kShuttingDown:
    case EmbeddingStatus::kNotLoaded:
      return EmbedOutcome::kRetryLater;
  }
  return EmbedOutcome::kRetryLater;
}

LlamaEmbedder::LlamaEmbedder(const EmbeddingModelSpec& spec,
                             const base::FilePath& model_path)
    : space_(SpaceOf(spec)),
      query_prefix_(spec.query_prefix ? spec.query_prefix : ""),
      document_prefix_(spec.document_prefix ? spec.document_prefix : "") {
  config_.model_id = spec.model_id;
  config_.path = model_path;
  config_.file_size = spec.file_size_bytes;
  config_.ram_required_bytes = spec.ram_required_bytes;
  config_.pooling = ToMojo(spec.pooling);
  config_.max_tokens_per_text = spec.max_tokens_per_text;
  config_.terminator = spec.terminator ? spec.terminator : "";
  config_.dimensions = spec.dimensions;
  config_.native_dimensions = spec.native_dimensions;
  config_.architecture = spec.architecture ? spec.architecture : "";
}

LlamaEmbedder::~LlamaEmbedder() = default;

EmbedderSpace LlamaEmbedder::space() const {
  return space_;
}

bool LlamaEmbedder::is_synchronous() const {
  return false;
}

std::vector<float> LlamaEmbedder::Embed(const std::string& text) const {
  return std::vector<float>();  // asynchronous only
}

size_t LlamaEmbedder::max_batch_size() const {
  return kBatchTexts;
}

void LlamaEmbedder::EmbedBatch(std::vector<std::string> texts,
                               EmbedPurpose purpose,
                               BatchCallback done) const {
  const bool query = purpose == EmbedPurpose::kQuery;
  const std::string& prefix = query ? query_prefix_ : document_prefix_;
  if (!prefix.empty()) {
    for (std::string& text : texts) {
      if (!text.empty()) text = prefix + text;
    }
  }
  // The answer comes back to this sequence (the memory worker).
  scoped_refptr<base::SequencedTaskRunner> reply_runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  auto reply = [](scoped_refptr<base::SequencedTaskRunner> runner,
                  BatchCallback cb, EmbeddingStatus status,
                  std::vector<std::vector<float>> vectors) {
    EmbedBatchResult result;
    result.outcome = OutcomeForStatus(status);
    result.vectors = std::move(vectors);
    runner->PostTask(
        FROM_HERE,
        base::BindOnce([](BatchCallback cb,
                          EmbedBatchResult r) { cb(std::move(r)); },
                       std::move(cb), std::move(result)));
  };
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](EmbeddingModelConfig config, std::vector<std::string> texts,
             bool urgent, base::OnceCallback<void(
                              EmbeddingStatus, std::vector<std::vector<float>>)>
                              on_done) {
            EmbeddingProcessHost::GetInstance()->Embed(
                config, std::move(texts), std::move(on_done), urgent);
          },
          config_, std::move(texts), query,
          base::BindOnce(reply, std::move(reply_runner), std::move(done))));
}

void LlamaEmbedder::EmbedQueryIfReady(std::string text,
                                      BatchCallback done) const {
  if (!text.empty() && !query_prefix_.empty()) {
    text = query_prefix_ + text;
  }
  scoped_refptr<base::SequencedTaskRunner> reply_runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  auto reply = [](scoped_refptr<base::SequencedTaskRunner> runner,
                  BatchCallback cb, EmbeddingStatus status,
                  std::vector<std::vector<float>> vectors) {
    EmbedBatchResult result;
    result.outcome = OutcomeForStatus(status);
    result.vectors = std::move(vectors);
    runner->PostTask(
        FROM_HERE,
        base::BindOnce([](BatchCallback cb,
                          EmbedBatchResult r) { cb(std::move(r)); },
                       std::move(cb), std::move(result)));
  };
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](EmbeddingModelConfig config, std::string text,
             base::OnceCallback<void(EmbeddingStatus,
                                     std::vector<std::vector<float>>)>
                 on_done) {
            // Never starts the model process: typing in the omnibox only
            // uses it when it is already loaded.
            EmbeddingProcessHost::GetInstance()->EmbedIfLoaded(
                config, {std::move(text)}, std::move(on_done));
          },
          config_, std::move(text),
          base::BindOnce(reply, std::move(reply_runner), std::move(done))));
}

}  // namespace memory
}  // namespace molt_ai
