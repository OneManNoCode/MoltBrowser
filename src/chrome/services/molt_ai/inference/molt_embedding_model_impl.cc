// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/molt_embedding_model_impl.h"

#include <atomic>
#include <functional>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai {

namespace {

// The browser keeps at most one call in flight; a few more are tolerated,
// an unbounded queue is not.
constexpr size_t kMaxPendingCalls = 8;

// Caps from molt_inference.mojom. Returns the reason for a bad message, or
// nullptr.
const char* CheckTexts(const std::vector<std::string>& texts) {
  if (texts.empty() || texts.size() > mojom::kMaxEmbedTexts) {
    return "bad number of texts to embed";
  }
  for (const std::string& text : texts) {
    if (text.empty() || text.size() > mojom::kMaxEmbedTextBytes ||
        !IsValidUtf8(text)) {
      return "bad text to embed";
    }
  }
  return nullptr;
}

}  // namespace

// Set on the main sequence when the pipe closes, read on the engine
// sequence before every text.
class MoltEmbeddingModelImpl::CancelFlag
    : public base::RefCountedThreadSafe<CancelFlag> {
 public:
  CancelFlag() = default;
  CancelFlag(const CancelFlag&) = delete;
  CancelFlag& operator=(const CancelFlag&) = delete;

  void Set() { cancelled_.store(true, std::memory_order_relaxed); }
  bool IsSet() const { return cancelled_.load(std::memory_order_relaxed); }

 private:
  friend class base::RefCountedThreadSafe<CancelFlag>;
  ~CancelFlag() = default;

  std::atomic<bool> cancelled_{false};
};

MoltEmbeddingModelImpl::MoltEmbeddingModelImpl(
    base::SequenceBound<LlamaEmbeddingEngine> engine,
    mojo::PendingReceiver<mojom::EmbeddingModel> receiver,
    base::OnceClosure on_disconnect)
    : engine_(std::move(engine)),
      receiver_(this, std::move(receiver)),
      cancelled_(base::MakeRefCounted<CancelFlag>()) {
  receiver_.set_disconnect_handler(std::move(on_disconnect));
}

MoltEmbeddingModelImpl::~MoltEmbeddingModelImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Stop a running Embed() before its next text; |engine_| is destroyed on
  // its sequence after that task returns.
  cancelled_->Set();
}

void MoltEmbeddingModelImpl::Embed(const std::vector<std::string>& texts,
                                   EmbedCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const char* error = CheckTexts(texts)) {
    receiver_.ReportBadMessage(error);
    return;
  }
  if (pending_ >= kMaxPendingCalls) {
    receiver_.ReportBadMessage("too many embedding calls in flight");
    return;
  }
  ++pending_;
  engine_.AsyncCall(&LlamaEmbeddingEngine::Embed)
      .WithArgs(texts, std::function<bool()>([flag = cancelled_]() {
                  return flag->IsSet();
                }))
      .Then(base::BindOnce(&MoltEmbeddingModelImpl::OnEmbedded,
                           weak_factory_.GetWeakPtr(), std::move(callback)));
}

void MoltEmbeddingModelImpl::OnEmbedded(
    EmbedCallback callback,
    std::optional<std::vector<float>> vectors) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (pending_ > 0) --pending_;
  std::move(callback).Run(vectors);
}

}  // namespace molt_ai
