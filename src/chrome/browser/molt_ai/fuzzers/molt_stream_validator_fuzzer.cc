// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's checks on what the sandboxed inference service streams back
// (runtime/stream_validator.h). The service is treated as hostile: any
// chunks, progress and completion messages in any order, at any rate.
//
// Input: limits and stop strings, then a sequence of messages with their
// (browser clock) times.
//
// Checks, until the first violation (where the browser drops the request):
// what callers get is valid UTF-8 without NUL and never contains a stop
// string; it is never more than the byte cap, nor more chunks than the
// chunk cap; progress and summary numbers are clamped to the window;
// nothing is accepted after OnComplete(). Also the model description and
// tokenization checks, and CrashLoopGuard.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/runtime/stream_validator.h"

namespace molt_ai {
namespace {

// Browser clock values (base::TimeTicks in ms): far from overflow.
constexpr int64_t kMaxClockMs = int64_t{1} << 50;

int64_t NextTime(FuzzedDataProvider* p, int64_t now) {
  switch (p->ConsumeIntegralInRange<int>(0, 3)) {
    case 0:
      return now;
    case 1:
      return std::min(kMaxClockMs, now + p->ConsumeIntegralInRange<int64_t>(
                                               0, 2000));
    case 2:
      // The browser's clock is monotonic; an earlier time checks that the
      // validator copes with one that went back anyway.
      return std::max<int64_t>(0, now - p->ConsumeIntegralInRange<int64_t>(
                                            0, 5000));
    default:
      return p->ConsumeIntegralInRange<int64_t>(0, kMaxClockMs);
  }
}

void CheckGuard(FuzzedDataProvider* p) {
  CrashLoopGuard guard;
  int64_t now = p->ConsumeIntegralInRange<int64_t>(0, kMaxClockMs);
  for (int i = 0; i < 64 && p->remaining_bytes() > 0; ++i) {
    const std::string key(1, static_cast<char>('a' + p->ConsumeIntegralInRange<int>(0, 40)));
    now = NextTime(p, now);
    switch (p->ConsumeIntegralInRange<int>(0, 2)) {
      case 0:
        guard.RecordCrash(key,
                          static_cast<CrashLoopGuard::Phase>(
                              p->ConsumeIntegralInRange<int>(0, 2)),
                          now);
        break;
      case 1:
        guard.Forget(key);
        CHECK(guard.Check(key, now) == CrashLoopGuard::Verdict::kAllowed);
        break;
      default: {
        const CrashLoopGuard::Verdict v = guard.Check(key, now);
        CHECK(CrashLoopGuard::Explain(v).empty() ==
              (v == CrashLoopGuard::Verdict::kAllowed));
        break;
      }
    }
  }
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider p(data, size);

  StreamLimits limits;
  // The loaded model's n_ctx, as IsValidModelDescription() let it through,
  // and the request's budgets.
  limits.context_tokens = p.ConsumeIntegralInRange<uint32_t>(0, 1 << 20);
  limits.max_visible_tokens = p.ConsumeIntegral<uint32_t>();
  limits.max_reasoning_tokens = p.ConsumeIntegral<uint32_t>();
  if (p.ConsumeBool()) {
    // Small caps, so every limit is reached.
    limits.max_chunk_bytes = p.ConsumeIntegralInRange<size_t>(0, 64);
    limits.max_total_bytes_cap = p.ConsumeIntegralInRange<size_t>(0, 4096);
    limits.bytes_per_token = p.ConsumeIntegralInRange<size_t>(0, 16);
    limits.max_progress_per_second = p.ConsumeIntegralInRange<int>(0, 8);
    limits.max_messages_per_second = p.ConsumeIntegralInRange<int>(0, 64);
    limits.stall_timeout_ms = p.ConsumeIntegralInRange<int64_t>(0, 10000);
    limits.deadline_ms = p.ConsumeIntegralInRange<int64_t>(0, 100000);
  }
  // As BrowserAIRuntime passes them (StopSequencesForService): non-empty,
  // valid UTF-8, at most 8 of at most 64 bytes.
  const size_t stop_count = p.ConsumeIntegralInRange<size_t>(0, 8);
  for (size_t i = 0; i < stop_count; ++i) {
    std::string s = p.ConsumeRandomLengthString(64);
    if (!s.empty() && IsValidUtf8(s)) limits.stop_sequences.push_back(s);
  }
  const std::vector<std::string> stops = limits.stop_sequences;
  const uint32_t ctx = limits.context_tokens;

  if (p.ConsumeBool()) {
    CheckGuard(&p);
  }

  int64_t now = p.ConsumeIntegralInRange<int64_t>(0, kMaxClockMs);
  StreamValidator validator(std::move(limits), now);
  if (p.ConsumeBool()) validator.SetQueued();
  std::string shown;
  bool completed = false;
  for (int step = 0; step < 4096 && p.remaining_bytes() > 0; ++step) {
    now = NextTime(&p, now);
    StreamViolation violation = StreamViolation::kNone;
    switch (p.ConsumeIntegralInRange<int>(0, 9)) {
      case 0: {
        const uint32_t done = p.ConsumeIntegral<uint32_t>();
        const uint32_t total = p.ConsumeIntegral<uint32_t>();
        const StreamValidator::ProgressResult r =
            validator.OnProgress(done, total, now);
        violation = r.violation;
        CHECK(r.total_tokens <= ctx && r.done_tokens <= r.total_tokens);
        break;
      }
      case 1: {
        StreamSummary summary;
        summary.status = p.ConsumeIntegral<int32_t>();
        summary.prompt_tokens = p.ConsumeIntegral<uint32_t>();
        summary.reused_tokens = p.ConsumeIntegral<uint32_t>();
        summary.generated_tokens = p.ConsumeIntegral<uint32_t>();
        summary.history_dropped = p.ConsumeIntegral<uint32_t>();
        const StreamValidator::CompleteResult r =
            validator.OnComplete(summary, now);
        violation = r.violation;
        if (violation == StreamViolation::kNone) {
          CHECK(!completed);
          completed = true;
          shown += r.visible_tail;
          CHECK(r.summary.prompt_tokens <= ctx &&
                r.summary.reused_tokens <= r.summary.prompt_tokens &&
                r.summary.generated_tokens <= ctx &&
                r.summary.history_dropped <= 512);
        } else if (completed) {
          CHECK(violation == StreamViolation::kMessageAfterComplete);
        }
        break;
      }
      case 2:
        violation = validator.CheckTime(now);
        break;
      case 3:
        validator.Activate(now);
        break;
      default: {
        const std::string chunk = p.ConsumeRandomLengthString(8192);
        const StreamValidator::ChunkResult r = validator.OnChunk(chunk, now);
        violation = r.violation;
        if (violation == StreamViolation::kNone) {
          CHECK(!completed);
          CHECK(IsValidUtf8(chunk));
          shown += r.visible;
          if (r.stopped) CHECK(validator.stopped());
        } else {
          CHECK(r.visible.empty());
          if (completed) {
            CHECK(violation == StreamViolation::kMessageAfterComplete);
          }
        }
        break;
      }
    }
    // What the caller showed so far is safe to show.
    CHECK(IsValidUtf8(shown) && shown.find('\0') == std::string::npos);
    for (const std::string& stop : stops) {
      CHECK(shown.find(stop) == std::string::npos);
    }
    CHECK(validator.total_bytes() <= validator.max_total_bytes() ||
          violation == StreamViolation::kTotalTooLarge);
    CHECK(validator.chunks() <= validator.max_chunks() ||
          violation == StreamViolation::kTooManyChunks);
    CHECK(StreamViolationName(violation) != nullptr);
    if (violation != StreamViolation::kNone) break;  // the request is dropped
  }

  // The service's model description and tokenizations.
  const std::string arch = p.ConsumeRandomLengthString(80);
  const std::string backend = p.ConsumeRandomLengthString(16);
  const uint32_t n_ctx = p.ConsumeIntegral<uint32_t>();
  const uint32_t vocab = p.ConsumeIntegral<uint32_t>();
  if (IsValidModelDescription(arch, backend, n_ctx, vocab, 256, 1 << 20)) {
    CHECK(arch.size() <= 64 && n_ctx >= 256 && n_ctx <= (1u << 20));
    CHECK(vocab > 0 && vocab <= (1u << 22));
  }
  std::vector<int32_t> tokens(p.ConsumeIntegralInRange<size_t>(0, 64));
  for (int32_t& t : tokens) t = p.ConsumeIntegral<int32_t>();
  const size_t text_bytes = p.ConsumeIntegralInRange<size_t>(0, 64);
  if (IsValidTokenization(tokens, text_bytes, vocab)) {
    for (int32_t t : tokens) CHECK(t >= 0 && static_cast<uint32_t>(t) < vocab);
  }
  return 0;
}
