// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/stream_validator.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace molt_ai {

namespace {

// Forced-answer markers and flushes can add a few tokens/chunks beyond the
// budgets.
constexpr uint32_t kTokenSlack = 16;
constexpr uint32_t kChunkSlack = 16;
constexpr int64_t kWindowMs = 1000;

constexpr int64_t kHourMs = 60 * 60 * 1000;
constexpr int64_t kLoadCrashWindowMs = kHourMs;
constexpr size_t kLoadCrashesToRefuse = 2;
constexpr int64_t kCrashWindowMs = 10 * 60 * 1000;
constexpr size_t kCrashesToRefuse = 3;
constexpr int64_t kBackoffMs[] = {1000, 5000, 30000};
constexpr size_t kMaxGuardEntries = 32;

uint32_t Clamp(uint32_t v, uint32_t hi) {
  return std::min(v, hi);
}

}  // namespace

const char* StreamViolationName(StreamViolation violation) {
  switch (violation) {
    case StreamViolation::kNone:
      return "none";
    case StreamViolation::kInvalidUtf8:
      return "chunk is not valid UTF-8";
    case StreamViolation::kEmptyChunk:
      return "empty chunk";
    case StreamViolation::kChunkTooLarge:
      return "chunk too large";
    case StreamViolation::kTotalTooLarge:
      return "too much text";
    case StreamViolation::kTooManyChunks:
      return "too many chunks";
    case StreamViolation::kProgressFlood:
      return "too many progress messages";
    case StreamViolation::kMessageFlood:
      return "too many messages";
    case StreamViolation::kMessageAfterComplete:
      return "message after OnComplete";
    case StreamViolation::kStalled:
      return "no message within the liveness timeout";
    case StreamViolation::kDeadlineExceeded:
      return "generation deadline exceeded";
    case StreamViolation::kGpuFailureOffGpu:
      return "GPU failure reported for a model not on the GPU";
  }
  return "unknown";
}

StreamValidator::StreamValidator(StreamLimits limits, int64_t now_ms)
    : limits_(std::move(limits)),
      start_ms_(now_ms),
      stop_(limits_.stop_sequences),
      last_message_ms_(now_ms),
      window_start_ms_(now_ms) {
  const uint64_t ctx = limits_.context_tokens;
  const uint64_t visible =
      limits_.max_visible_tokens ? limits_.max_visible_tokens : ctx;
  const uint64_t hidden =
      limits_.max_reasoning_tokens ? limits_.max_reasoning_tokens : ctx;
  // Nothing can generate more tokens than fit the context.
  const uint64_t tokens = std::min(ctx, visible + hidden) + kTokenSlack;
  max_total_bytes_ = static_cast<size_t>(
      std::min<uint64_t>(limits_.max_total_bytes_cap,
                         tokens * limits_.bytes_per_token));
  const uint64_t chunk_bytes = std::max<size_t>(limits_.max_chunk_bytes, 1);
  max_chunks_ = static_cast<uint32_t>(std::min<uint64_t>(
      UINT32_MAX, tokens + max_total_bytes_ / chunk_bytes + kChunkSlack));
}

StreamValidator::~StreamValidator() = default;

StreamViolation StreamValidator::CountMessage(int64_t now_ms,
                                              bool is_progress) {
  if (completed_) return StreamViolation::kMessageAfterComplete;
  if (now_ms - window_start_ms_ >= kWindowMs || now_ms < window_start_ms_) {
    window_start_ms_ = now_ms;
    messages_in_window_ = 0;
    progress_in_window_ = 0;
  }
  last_message_ms_ = now_ms;
  if (++messages_in_window_ > limits_.max_messages_per_second) {
    return StreamViolation::kMessageFlood;
  }
  if (is_progress && ++progress_in_window_ > limits_.max_progress_per_second) {
    return StreamViolation::kProgressFlood;
  }
  return StreamViolation::kNone;
}

StreamValidator::ChunkResult StreamValidator::OnChunk(std::string_view text,
                                                      int64_t now_ms) {
  ChunkResult result;
  result.violation = CountMessage(now_ms, /*is_progress=*/false);
  if (result.violation != StreamViolation::kNone) return result;
  if (text.empty()) {
    result.violation = StreamViolation::kEmptyChunk;
    return result;
  }
  if (text.size() > limits_.max_chunk_bytes) {
    result.violation = StreamViolation::kChunkTooLarge;
    return result;
  }
  if (++chunks_ > max_chunks_) {
    result.violation = StreamViolation::kTooManyChunks;
    return result;
  }
  total_bytes_ += text.size();
  if (total_bytes_ > max_total_bytes_) {
    result.violation = StreamViolation::kTotalTooLarge;
    return result;
  }
  if (!IsValidUtf8(text)) {
    result.violation = StreamViolation::kInvalidUtf8;
    return result;
  }
  if (stop_.stopped()) return result;  // already ended for the caller
  result.visible = stop_.Feed(ReplaceNul(text));
  result.stopped = stop_.stopped();
  return result;
}

StreamValidator::ProgressResult StreamValidator::OnProgress(
    uint32_t done_tokens,
    uint32_t total_tokens,
    int64_t now_ms) {
  ProgressResult result;
  result.violation = CountMessage(now_ms, /*is_progress=*/true);
  // The numbers are only shown, never used for a decision: clamp them.
  result.total_tokens = Clamp(total_tokens, limits_.context_tokens);
  result.done_tokens = Clamp(done_tokens, result.total_tokens);
  return result;
}

StreamValidator::CompleteResult StreamValidator::OnComplete(
    const StreamSummary& summary,
    int64_t now_ms) {
  CompleteResult result;
  result.violation = CountMessage(now_ms, /*is_progress=*/false);
  if (result.violation != StreamViolation::kNone) return result;
  if (summary.backend_failed && !limits_.model_on_gpu) {
    // Counted against the model (CrashLoopGuard), not as a GPU failure.
    result.violation = StreamViolation::kGpuFailureOffGpu;
    return result;
  }
  completed_ = true;
  result.visible_tail = stop_.Flush();
  // Callers add these up (AutomationRunner keeps an int total): never more
  // than the window.
  const uint32_t ctx = limits_.context_tokens;
  result.summary = summary;
  result.summary.prompt_tokens = Clamp(summary.prompt_tokens, ctx);
  result.summary.reused_tokens =
      Clamp(summary.reused_tokens, result.summary.prompt_tokens);
  result.summary.generated_tokens = Clamp(summary.generated_tokens, ctx);
  result.summary.history_dropped =
      Clamp(summary.history_dropped, 512u /* kMaxHistoryTurns */);
  return result;
}

void StreamValidator::SetQueued() {
  queued_ = true;
}

void StreamValidator::Activate(int64_t now_ms) {
  if (!queued_) return;
  queued_ = false;
  start_ms_ = now_ms;
  last_message_ms_ = std::max(last_message_ms_, now_ms);
}

StreamViolation StreamValidator::CheckTime(int64_t now_ms) const {
  if (completed_ || queued_) return StreamViolation::kNone;
  if (now_ms - start_ms_ > limits_.deadline_ms) {
    return StreamViolation::kDeadlineExceeded;
  }
  if (now_ms - last_message_ms_ > limits_.stall_timeout_ms) {
    return StreamViolation::kStalled;
  }
  return StreamViolation::kNone;
}

bool IsValidModelDescription(std::string_view architecture,
                             std::string_view backend,
                             uint32_t context_tokens,
                             uint32_t vocab_size,
                             uint32_t min_context_tokens,
                             uint32_t max_context_tokens) {
  if (architecture.size() > 64) return false;
  for (char c : architecture) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  if (backend != "metal" && backend != "cpu" && backend != "vulkan") {
    return false;
  }
  if (context_tokens < min_context_tokens ||
      context_tokens > max_context_tokens) {
    return false;
  }
  return vocab_size > 0 && vocab_size <= (1u << 22);
}

bool IsValidGpuDescription(bool gpu_requested,
                           std::string_view backend,
                           bool gpu_on,
                           uint32_t gpu_layers,
                           uint32_t max_gpu_layers) {
  if (backend == "vulkan") {
    return gpu_requested && gpu_on && gpu_layers >= 1 &&
           gpu_layers <= max_gpu_layers;
  }
  return !gpu_on && gpu_layers == 0;
}

bool IsValidTokenization(const std::vector<int32_t>& tokens,
                         size_t text_bytes,
                         uint32_t vocab_size) {
  if (tokens.size() > text_bytes + 16) return false;
  for (int32_t id : tokens) {
    if (id < 0 || static_cast<uint32_t>(id) >= vocab_size) return false;
  }
  return true;
}

// ---- CrashLoopGuard ---------------------------------------------------------

CrashLoopGuard::CrashLoopGuard() = default;
CrashLoopGuard::~CrashLoopGuard() = default;

const CrashLoopGuard::History* CrashLoopGuard::Find(
    const std::string& key) const {
  for (const Entry& e : entries_) {
    if (e.key == key) return &e.history;
  }
  return nullptr;
}

CrashLoopGuard::History* CrashLoopGuard::FindOrAdd(const std::string& key) {
  for (Entry& e : entries_) {
    if (e.key == key) return &e.history;
  }
  if (entries_.size() >= kMaxGuardEntries) entries_.erase(entries_.begin());
  entries_.push_back({key, {}});
  return &entries_.back().history;
}

void CrashLoopGuard::RecordCrash(const std::string& key,
                                 Phase phase,
                                 int64_t now_ms) {
  if (phase == Phase::kIdle) return;  // e.g. the OS reclaimed memory
  History* h = FindOrAdd(key);
  std::deque<int64_t>& list =
      phase == Phase::kLoading ? h->load_crashes : h->other_crashes;
  list.push_back(now_ms);
  while (list.size() > 8) list.pop_front();
}

CrashLoopGuard::Verdict CrashLoopGuard::Check(const std::string& key,
                                              int64_t now_ms) const {
  const History* h = Find(key);
  if (!h) return Verdict::kAllowed;
  size_t load_recent = 0;
  for (int64_t t : h->load_crashes) {
    if (now_ms - t < kLoadCrashWindowMs) ++load_recent;
  }
  if (load_recent >= kLoadCrashesToRefuse) {
    return Verdict::kRefusedAfterLoadCrashes;
  }
  size_t recent = 0;
  int64_t last = 0;
  for (int64_t t : h->other_crashes) {
    if (now_ms - t < kCrashWindowMs) {
      ++recent;
      last = std::max(last, t);
    }
  }
  if (recent >= kCrashesToRefuse) return Verdict::kRefusedAfterCrashes;
  if (recent > 0) {
    const int64_t backoff =
        kBackoffMs[std::min<size_t>(recent, std::size(kBackoffMs)) - 1];
    if (now_ms - last < backoff) return Verdict::kBackingOff;
  }
  return Verdict::kAllowed;
}

// static
std::string CrashLoopGuard::Explain(Verdict verdict) {
  switch (verdict) {
    case Verdict::kAllowed:
      return std::string();
    case Verdict::kBackingOff:
      return "The on-device model stopped unexpectedly. Try again in a few "
             "seconds.";
    case Verdict::kRefusedAfterLoadCrashes:
      return "This model file crashed the on-device AI process while "
             "loading. Delete and re-download it.";
    case Verdict::kRefusedAfterCrashes:
      return "The on-device model stopped unexpectedly several times. Try "
             "again in a few minutes, or pick another model.";
  }
  return std::string();
}

void CrashLoopGuard::Forget(const std::string& key) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) { return e.key == key; }),
                 entries_.end());
}

GpuFallbackGuard::GpuFallbackGuard() = default;
GpuFallbackGuard::~GpuFallbackGuard() = default;

bool GpuFallbackGuard::AllowGpu(const std::string& key) const {
  return !gpu_disabled() && std::find(failed_keys_.begin(), failed_keys_.end(),
                                      key) == failed_keys_.end();
}

void GpuFallbackGuard::RecordGpuFailure(const std::string& key) {
  if (failures_ < kMaxGpuFailures) ++failures_;
  if (failed_keys_.size() < kMaxGpuFailures &&
      std::find(failed_keys_.begin(), failed_keys_.end(), key) ==
          failed_keys_.end()) {
    failed_keys_.push_back(key);
  }
}

}  // namespace molt_ai
