// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Browser-side checks on what the sandboxed inference service sends
// (docs/design/SANDBOXED_INFERENCE.md section 5.3). The service is treated as
// hostile: a compromised process can send anything mojo's validation lets
// through, and mojo doesn't validate UTF-8 in C++ strings.
//
// Std-only (time is passed in as milliseconds) so it is unit-tested
// standalone, like the engine utils.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_STREAM_VALIDATOR_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_STREAM_VALIDATOR_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai {

// A reason to stop trusting the stream: the request fails and the process
// is killed.
enum class StreamViolation {
  kNone,
  kInvalidUtf8,
  kEmptyChunk,
  kChunkTooLarge,
  kTotalTooLarge,
  kTooManyChunks,
  kProgressFlood,
  kMessageFlood,
  kMessageAfterComplete,
  kStalled,
  kDeadlineExceeded,
};

const char* StreamViolationName(StreamViolation violation);

struct StreamLimits {
  // n_ctx of the loaded model (from its validated ModelDescription). No
  // count the service reports can exceed it.
  uint32_t context_tokens = 0;
  // As sent to the service; 0 = adaptive (bounded by context_tokens).
  uint32_t max_visible_tokens = 0;
  uint32_t max_reasoning_tokens = 0;
  // Re-applied here; this copy decides what callers see.
  std::vector<std::string> stop_sequences;

  size_t max_chunk_bytes = 4096;
  size_t max_total_bytes_cap = 2 * 1024 * 1024;
  size_t bytes_per_token = 512;
  int max_progress_per_second = 4;
  int max_messages_per_second = 2000;
  int64_t stall_timeout_ms = 120 * 1000;
  int64_t deadline_ms = 15 * 60 * 1000;
};

// Mirror of mojom::GenerationSummary (the validator is std-only).
struct StreamSummary {
  int32_t status = 0;
  uint32_t prompt_tokens = 0;
  uint32_t reused_tokens = 0;
  uint32_t generated_tokens = 0;
  uint32_t history_dropped = 0;
  bool context_trimmed = false;
  bool user_message_trimmed = false;
};

// One generation's stream. Every message from the service goes through it,
// in order:
//  - OnChunk: valid UTF-8, non-empty, at most max_chunk_bytes; NUL becomes
//    U+FFFD; the total and the count are capped; then the stop filter.
//  - OnProgress: rate-limited; numbers clamped.
//  - OnComplete: exactly once, last; counts clamped to [0, context_tokens].
//  - Any message: at most max_messages_per_second; nothing after OnComplete.
//  - CheckTime (from a timer): no message for stall_timeout_ms, or longer
//    than deadline_ms in total.
// Memory is bounded: text is returned, never accumulated, and the stop
// filter holds back less than the longest stop string.
class StreamValidator {
 public:
  StreamValidator(StreamLimits limits, int64_t now_ms);
  ~StreamValidator();
  StreamValidator(const StreamValidator&) = delete;
  StreamValidator& operator=(const StreamValidator&) = delete;

  struct ChunkResult {
    StreamViolation violation = StreamViolation::kNone;
    // Text for the caller (may be empty while the stop filter holds back).
    std::string visible;
    // A stop sequence matched: the caller ends the request (and closes the
    // streamer, which cancels it in the service).
    bool stopped = false;
  };
  ChunkResult OnChunk(std::string_view text, int64_t now_ms);

  struct ProgressResult {
    StreamViolation violation = StreamViolation::kNone;
    uint32_t done_tokens = 0;
    uint32_t total_tokens = 0;
  };
  ProgressResult OnProgress(uint32_t done_tokens,
                            uint32_t total_tokens,
                            int64_t now_ms);

  struct CompleteResult {
    StreamViolation violation = StreamViolation::kNone;
    // Held back by the stop filter, released at the end.
    std::string visible_tail;
    StreamSummary summary;
  };
  CompleteResult OnComplete(const StreamSummary& summary, int64_t now_ms);

  StreamViolation CheckTime(int64_t now_ms) const;

  bool completed() const { return completed_; }
  bool stopped() const { return stop_.stopped(); }
  size_t total_bytes() const { return total_bytes_; }
  uint32_t chunks() const { return chunks_; }
  size_t max_total_bytes() const { return max_total_bytes_; }
  uint32_t max_chunks() const { return max_chunks_; }

 private:
  // Counts one message against the per-second limits.
  StreamViolation CountMessage(int64_t now_ms, bool is_progress);

  const StreamLimits limits_;
  const int64_t start_ms_;
  size_t max_total_bytes_ = 0;
  uint32_t max_chunks_ = 0;

  StopSequenceFilter stop_;
  int64_t last_message_ms_;
  int64_t window_start_ms_;
  int messages_in_window_ = 0;
  int progress_in_window_ = 0;
  size_t total_bytes_ = 0;
  uint32_t chunks_ = 0;
  bool completed_ = false;
};

// Validation of a ModelDescription (section 5.3): architecture at most 64
// bytes of [a-z0-9_.-], a known backend, context_tokens within
// [min_context_tokens, max_context_tokens], a vocab size in (0, 2^22].
bool IsValidModelDescription(std::string_view architecture,
                             std::string_view backend,
                             uint32_t context_tokens,
                             uint32_t vocab_size,
                             uint32_t min_context_tokens,
                             uint32_t max_context_tokens);

// Tokenize results: at most text_bytes + 16 ids, each in [0, vocab_size).
bool IsValidTokenization(const std::vector<int32_t>& tokens,
                         size_t text_bytes,
                         uint32_t vocab_size);

// Crash-loop guard (section 4.3), per model file (id + size + mtime):
//  - two crashes while loading within an hour: the file is refused (it
//    crashed the process; delete and re-download it);
//  - crashes while generating back relaunches off (1 s, 5 s, 30 s), and
//    after three within ten minutes the model is refused for the rest of
//    those ten minutes, so page text that deterministically crashes the
//    tokenizer can't make automation hammer the process.
// The runtime can't tell an explicit user pick from an auto-load (both are
// BrowserAIRuntime::LoadModel), so the refusals expire with time instead of
// "until the user picks the model again"; a re-downloaded file is a new key.
class CrashLoopGuard {
 public:
  enum class Phase { kLoading, kGenerating, kIdle };
  enum class Verdict { kAllowed, kBackingOff, kRefusedAfterLoadCrashes,
                       kRefusedAfterCrashes };

  CrashLoopGuard();
  ~CrashLoopGuard();

  void RecordCrash(const std::string& key, Phase phase, int64_t now_ms);
  Verdict Check(const std::string& key, int64_t now_ms) const;
  // A user-facing explanation for a refusal ("" when allowed).
  static std::string Explain(Verdict verdict);
  // The file was deleted or replaced.
  void Forget(const std::string& key);

 private:
  struct History {
    std::deque<int64_t> load_crashes;
    std::deque<int64_t> other_crashes;
  };
  struct Entry {
    std::string key;
    History history;
  };
  const History* Find(const std::string& key) const;
  History* FindOrAdd(const std::string& key);

  // A handful of model files at most; old entries are dropped beyond this.
  std::vector<Entry> entries_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_STREAM_VALIDATOR_H_
