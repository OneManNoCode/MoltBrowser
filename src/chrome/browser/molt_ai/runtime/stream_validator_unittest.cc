// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/stream_validator.h"

#include <cstdint>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

StreamLimits Limits(uint32_t ctx = 4096,
                    uint32_t visible = 256,
                    uint32_t hidden = 256) {
  StreamLimits l;
  l.context_tokens = ctx;
  l.max_visible_tokens = visible;
  l.max_reasoning_tokens = hidden;
  return l;
}

TEST(StreamValidatorTest, PassesValidChunksThrough) {
  StreamValidator v(Limits(), 0);
  StreamValidator::ChunkResult r = v.OnChunk("Hello ", 10);
  EXPECT_EQ(r.violation, StreamViolation::kNone);
  EXPECT_EQ(r.visible, "Hello ");
  r = v.OnChunk("w\xC3\xB6rld \xF0\x9F\x98\x80", 20);
  EXPECT_EQ(r.violation, StreamViolation::kNone);
  EXPECT_EQ(r.visible, "w\xC3\xB6rld \xF0\x9F\x98\x80");
  EXPECT_EQ(v.chunks(), 2u);
}

TEST(StreamValidatorTest, RejectsInvalidUtf8) {
  for (const char* bad : {"\x80", "ab\xC3", "\xED\xA0\x80", "\xC0\xAF",
                          "\xF4\x90\x80\x80", "x\xFFy"}) {
    StreamValidator v(Limits(), 0);
    EXPECT_EQ(v.OnChunk(bad, 1).violation, StreamViolation::kInvalidUtf8)
        << bad;
  }
}

TEST(StreamValidatorTest, ReplacesNul) {
  StreamValidator v(Limits(), 0);
  EXPECT_EQ(v.OnChunk(std::string("a\0b", 3), 1).visible,
            "a\xEF\xBF\xBD" "b");
}

TEST(StreamValidatorTest, RejectsEmptyAndOversizedChunks) {
  StreamValidator v(Limits(), 0);
  EXPECT_EQ(v.OnChunk("", 1).violation, StreamViolation::kEmptyChunk);
  StreamValidator w(Limits(), 0);
  EXPECT_EQ(w.OnChunk(std::string(4096, 'a'), 1).violation,
            StreamViolation::kNone);
  EXPECT_EQ(w.OnChunk(std::string(4097, 'a'), 2).violation,
            StreamViolation::kChunkTooLarge);
}

TEST(StreamValidatorTest, CapsTotalTextByBudget) {
  // (256 + 256 + 16) tokens * 512 bytes = 270336 bytes.
  StreamValidator v(Limits(4096, 256, 256), 0);
  EXPECT_EQ(v.max_total_bytes(), 528u * 512u);
  const std::string chunk(4096, 'x');
  StreamViolation last = StreamViolation::kNone;
  int64_t t = 0;
  for (int i = 0; i < 100 && last == StreamViolation::kNone; ++i) {
    t += 100;  // stay under the message rate
    last = v.OnChunk(chunk, t).violation;
  }
  EXPECT_EQ(last, StreamViolation::kTotalTooLarge);
  EXPECT_LE(v.total_bytes(), v.max_total_bytes() + 4096);
}

TEST(StreamValidatorTest, AdaptiveBudgetsAreBoundedByTheContext) {
  StreamValidator v(Limits(8192, 0, 0), 0);
  EXPECT_EQ(v.max_total_bytes(), 2u * 1024 * 1024);  // the 2 MiB cap
  StreamValidator w(Limits(2048, 0, 0), 0);
  EXPECT_EQ(w.max_total_bytes(), (2048u + 16u) * 512u);
}

TEST(StreamValidatorTest, CapsChunkCount) {
  StreamValidator v(Limits(4096, 8, 8), 0);
  // tokens = 32, bytes cap = 32 * 512 = 16384 -> chunks <= 32 + 4 + 16.
  EXPECT_EQ(v.max_chunks(), 52u);
  StreamViolation last = StreamViolation::kNone;
  int n = 0;
  int64_t t = 0;
  while (last == StreamViolation::kNone && n < 100) {
    t += 10;
    last = v.OnChunk("a", t).violation;
    ++n;
  }
  EXPECT_EQ(last, StreamViolation::kTooManyChunks);
  EXPECT_EQ(n, 53);
}

TEST(StreamValidatorTest, RateLimitsMessagesAndProgress) {
  StreamValidator v(Limits(1 << 20, 0, 0), 0);
  StreamViolation last = StreamViolation::kNone;
  int n = 0;
  while (last == StreamViolation::kNone && n < 5000) {
    last = v.OnChunk("a", 500).violation;  // all in one second
    ++n;
  }
  EXPECT_EQ(last, StreamViolation::kMessageFlood);
  EXPECT_EQ(n, 2001);

  StreamValidator p(Limits(), 0);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(p.OnProgress(1, 2, 100 + i).violation, StreamViolation::kNone);
  }
  EXPECT_EQ(p.OnProgress(1, 2, 200).violation, StreamViolation::kProgressFlood);
  // A new second, a new window.
  StreamValidator q(Limits(), 0);
  for (int s = 0; s < 10; ++s) {
    for (int i = 0; i < 2; ++i) {
      EXPECT_EQ(q.OnProgress(1, 2, s * 1000 + i * 500).violation,
                StreamViolation::kNone);
    }
  }
}

TEST(StreamValidatorTest, ClampsProgressNumbers) {
  StreamValidator v(Limits(4096), 0);
  StreamValidator::ProgressResult r = v.OnProgress(9999999, 8888888, 1);
  EXPECT_EQ(r.violation, StreamViolation::kNone);
  EXPECT_EQ(r.total_tokens, 4096u);
  EXPECT_EQ(r.done_tokens, 4096u);
  r = v.OnProgress(50, 10, 600);
  EXPECT_EQ(r.done_tokens, 10u);
}

TEST(StreamValidatorTest, CompleteExactlyOnceAndLast) {
  StreamValidator v(Limits(), 0);
  StreamSummary s;
  s.status = 0;
  s.prompt_tokens = 10;
  EXPECT_EQ(v.OnComplete(s, 5).violation, StreamViolation::kNone);
  EXPECT_TRUE(v.completed());
  EXPECT_EQ(v.OnChunk("late", 6).violation,
            StreamViolation::kMessageAfterComplete);
  EXPECT_EQ(v.OnProgress(1, 1, 7).violation,
            StreamViolation::kMessageAfterComplete);
  EXPECT_EQ(v.OnComplete(s, 8).violation,
            StreamViolation::kMessageAfterComplete);
}

TEST(StreamValidatorTest, ClampsSummaryCounts) {
  StreamValidator v(Limits(2048), 0);
  StreamSummary s;
  s.prompt_tokens = 0xFFFFFFFFu;
  s.reused_tokens = 0xFFFFFFFFu;
  s.generated_tokens = 0x7FFFFFFFu;
  s.history_dropped = 100000;
  StreamValidator::CompleteResult r = v.OnComplete(s, 1);
  EXPECT_EQ(r.summary.prompt_tokens, 2048u);
  EXPECT_EQ(r.summary.reused_tokens, 2048u);
  EXPECT_EQ(r.summary.generated_tokens, 2048u);
  EXPECT_EQ(r.summary.history_dropped, 512u);
  // AutomationRunner adds prompt + generated into an int.
  EXPECT_LT(static_cast<int64_t>(r.summary.prompt_tokens) +
                r.summary.generated_tokens,
            static_cast<int64_t>(INT32_MAX));
}

TEST(StreamValidatorTest, StopSequencesAreReappliedAndEndTheStream) {
  StreamLimits l = Limits();
  l.stop_sequences = {"\nUser:"};
  StreamValidator v(l, 0);
  StreamValidator::ChunkResult a = v.OnChunk("Answer.\nUs", 1);
  EXPECT_EQ(a.visible, "Answer.");
  EXPECT_FALSE(a.stopped);
  StreamValidator::ChunkResult b = v.OnChunk("er: injected", 2);
  EXPECT_EQ(b.visible, "");
  EXPECT_TRUE(b.stopped);
  EXPECT_TRUE(v.stopped());
  // Anything after the stop is still validated but not shown.
  EXPECT_EQ(v.OnChunk("more", 3).visible, "");
  EXPECT_EQ(v.OnChunk("\xFF", 4).violation, StreamViolation::kInvalidUtf8);
}

TEST(StreamValidatorTest, HeldBackTextIsReleasedOnComplete) {
  StreamLimits l = Limits();
  l.stop_sequences = {"STOP"};
  StreamValidator v(l, 0);
  EXPECT_EQ(v.OnChunk("end ST", 1).visible, "end ");
  EXPECT_EQ(v.OnComplete(StreamSummary(), 2).visible_tail, "ST");
}

TEST(StreamValidatorTest, LivenessAndDeadline) {
  StreamLimits l = Limits();
  l.stall_timeout_ms = 120000;
  l.deadline_ms = 900000;
  StreamValidator v(l, 1000);
  EXPECT_EQ(v.CheckTime(100000), StreamViolation::kNone);
  EXPECT_EQ(v.CheckTime(121001), StreamViolation::kStalled);
  // Messages keep it alive...
  StreamValidator w(l, 0);
  for (int64_t t = 0; t < 900000; t += 60000) {
    ASSERT_EQ(w.OnProgress(1, 2, t).violation, StreamViolation::kNone);
    EXPECT_EQ(w.CheckTime(t + 1), StreamViolation::kNone);
  }
  // ... but not past the deadline.
  EXPECT_EQ(w.CheckTime(900001), StreamViolation::kDeadlineExceeded);
  // Done is done.
  w.OnComplete(StreamSummary(), 900002);
  EXPECT_EQ(w.CheckTime(5000000), StreamViolation::kNone);
}

TEST(StreamValidatorTest, QueuedRequestsHaveNoClockUntilActivated) {
  StreamLimits l = Limits();
  l.stall_timeout_ms = 120000;
  l.deadline_ms = 900000;
  StreamValidator v(l, 0);
  v.SetQueued();
  EXPECT_TRUE(v.queued());
  // Waiting behind a 20-minute request is fine...
  EXPECT_EQ(v.CheckTime(20 * 60 * 1000), StreamViolation::kNone);
  // ... and the clocks start when it runs.
  v.Activate(20 * 60 * 1000);
  EXPECT_FALSE(v.queued());
  EXPECT_EQ(v.CheckTime(20 * 60 * 1000 + 119000), StreamViolation::kNone);
  EXPECT_EQ(v.CheckTime(20 * 60 * 1000 + 121000), StreamViolation::kStalled);
  // Activate() on a running request changes nothing.
  StreamValidator w(l, 0);
  w.Activate(500000);
  EXPECT_EQ(w.CheckTime(121000), StreamViolation::kStalled);
}

TEST(ModelDescriptionTest, Validation) {
  EXPECT_TRUE(IsValidModelDescription("llama", "cpu", 4096, 32000, 256, 8192));
  EXPECT_TRUE(IsValidModelDescription("gpt-oss", "metal", 256, 1, 256, 256));
  EXPECT_TRUE(IsValidModelDescription("qwen3moe", "vulkan", 8192, 151936, 256,
                                      32768));
  EXPECT_TRUE(IsValidModelDescription("", "cpu", 2048, 10, 256, 4096));
  EXPECT_FALSE(IsValidModelDescription("Llama", "cpu", 4096, 10, 256, 8192));
  EXPECT_FALSE(IsValidModelDescription("lla ma", "cpu", 4096, 10, 256, 8192));
  EXPECT_FALSE(IsValidModelDescription("<script>", "cpu", 4096, 10, 256,
                                       8192));
  EXPECT_FALSE(IsValidModelDescription(std::string(65, 'a'), "cpu", 4096, 10,
                                       256, 8192));
  EXPECT_FALSE(IsValidModelDescription("llama", "cuda", 4096, 10, 256, 8192));
  EXPECT_FALSE(IsValidModelDescription("llama", "cpu", 255, 10, 256, 8192));
  EXPECT_FALSE(IsValidModelDescription("llama", "cpu", 8193, 10, 256, 8192));
  EXPECT_FALSE(IsValidModelDescription("llama", "cpu", 4096, 0, 256, 8192));
  EXPECT_FALSE(IsValidModelDescription("llama", "cpu", 4096, (1u << 22) + 1,
                                       256, 8192));
}

TEST(TokenizationTest, Validation) {
  EXPECT_TRUE(IsValidTokenization({0, 5, 9}, 3, 10));
  EXPECT_TRUE(IsValidTokenization({}, 0, 10));
  EXPECT_FALSE(IsValidTokenization({10}, 3, 10));
  EXPECT_FALSE(IsValidTokenization({-1}, 3, 10));
  EXPECT_FALSE(IsValidTokenization(std::vector<int32_t>(20, 1), 3, 10));
}

TEST(CrashLoopGuardTest, TwoLoadCrashesWithinAnHourRefuseTheFile) {
  CrashLoopGuard g;
  const std::string key = "qwen3-8b:5027783488:1700000000";
  EXPECT_EQ(g.Check(key, 0), CrashLoopGuard::Verdict::kAllowed);
  g.RecordCrash(key, CrashLoopGuard::Phase::kLoading, 0);
  EXPECT_EQ(g.Check(key, 10), CrashLoopGuard::Verdict::kAllowed);
  g.RecordCrash(key, CrashLoopGuard::Phase::kLoading, 30 * 60 * 1000);
  EXPECT_EQ(g.Check(key, 31 * 60 * 1000),
            CrashLoopGuard::Verdict::kRefusedAfterLoadCrashes);
  EXPECT_NE(CrashLoopGuard::Explain(
                CrashLoopGuard::Verdict::kRefusedAfterLoadCrashes)
                .find("re-download"),
            std::string::npos);
  // An hour after the first crash, only one is recent.
  EXPECT_EQ(g.Check(key, 61 * 60 * 1000), CrashLoopGuard::Verdict::kAllowed);
  // Other files are unaffected; a re-downloaded file has a new key.
  EXPECT_EQ(g.Check("qwen3-8b:5027783488:1800000000", 31 * 60 * 1000),
            CrashLoopGuard::Verdict::kAllowed);
  g.Forget(key);
  EXPECT_EQ(g.Check(key, 31 * 60 * 1000), CrashLoopGuard::Verdict::kAllowed);
}

TEST(CrashLoopGuardTest, GenerationCrashesBackOffThenRefuse) {
  CrashLoopGuard g;
  const std::string key = "m";
  const auto gen = CrashLoopGuard::Phase::kGenerating;
  g.RecordCrash(key, gen, 0);
  EXPECT_EQ(g.Check(key, 500), CrashLoopGuard::Verdict::kBackingOff);
  EXPECT_EQ(g.Check(key, 1000), CrashLoopGuard::Verdict::kAllowed);
  g.RecordCrash(key, gen, 2000);
  EXPECT_EQ(g.Check(key, 6999), CrashLoopGuard::Verdict::kBackingOff);
  EXPECT_EQ(g.Check(key, 7000), CrashLoopGuard::Verdict::kAllowed);
  g.RecordCrash(key, gen, 10000);
  EXPECT_EQ(g.Check(key, 10000 + 9 * 60 * 1000),
            CrashLoopGuard::Verdict::kRefusedAfterCrashes);
  // Ten minutes after the first crash two remain: 5 s backoff, long over.
  EXPECT_EQ(g.Check(key, 10 * 60 * 1000 + 1),
            CrashLoopGuard::Verdict::kAllowed);
}

TEST(CrashLoopGuardTest, IdleDeathsDontCount) {
  CrashLoopGuard g;
  for (int i = 0; i < 10; ++i) {
    g.RecordCrash("m", CrashLoopGuard::Phase::kIdle, i);
  }
  EXPECT_EQ(g.Check("m", 11), CrashLoopGuard::Verdict::kAllowed);
}

TEST(GpuDescriptionTest, Validation) {
  // On the GPU: requested, "vulkan", kOn, 1..max layers.
  EXPECT_TRUE(IsValidGpuDescription(true, "vulkan", true, 1, 4097));
  EXPECT_TRUE(IsValidGpuDescription(true, "vulkan", true, 4097, 4097));
  EXPECT_FALSE(IsValidGpuDescription(true, "vulkan", true, 0, 4097));
  EXPECT_FALSE(IsValidGpuDescription(true, "vulkan", true, 4098, 4097));
  // Not asked for the GPU, yet on it: a lie.
  EXPECT_FALSE(IsValidGpuDescription(false, "vulkan", true, 33, 4097));
  // "vulkan" without the GPU on, or the GPU on without "vulkan".
  EXPECT_FALSE(IsValidGpuDescription(true, "vulkan", false, 33, 4097));
  EXPECT_FALSE(IsValidGpuDescription(true, "cpu", true, 33, 4097));
  // On the CPU (a fallback included): no layers on the GPU.
  EXPECT_TRUE(IsValidGpuDescription(true, "cpu", false, 0, 4097));
  EXPECT_TRUE(IsValidGpuDescription(false, "cpu", false, 0, 4097));
  EXPECT_FALSE(IsValidGpuDescription(true, "cpu", false, 1, 4097));
  // Metal (macOS) never reports Vulkan layers.
  EXPECT_TRUE(IsValidGpuDescription(false, "metal", false, 0, 4097));
  EXPECT_FALSE(IsValidGpuDescription(false, "metal", false, 5, 4097));
}

TEST(GpuFallbackGuardTest, OneFailureTurnsTheGpuOffForThatModel) {
  GpuFallbackGuard g;
  EXPECT_TRUE(g.AllowGpu("a"));
  EXPECT_TRUE(g.AllowGpu("b"));
  g.RecordGpuFailure("a");
  EXPECT_FALSE(g.AllowGpu("a"));
  EXPECT_TRUE(g.AllowGpu("b"));
  EXPECT_FALSE(g.gpu_disabled());
  // A second failure, of the same model or another, turns it off for all.
  g.RecordGpuFailure("a");
  EXPECT_TRUE(g.gpu_disabled());
  EXPECT_FALSE(g.AllowGpu("b"));
}

TEST(GpuFallbackGuardTest, TwoFailuresTurnTheGpuOffForEveryModel) {
  GpuFallbackGuard g;
  g.RecordGpuFailure("a");
  g.RecordGpuFailure("b");
  EXPECT_TRUE(g.gpu_disabled());
  EXPECT_FALSE(g.AllowGpu("a"));
  EXPECT_FALSE(g.AllowGpu("b"));
  EXPECT_FALSE(g.AllowGpu("c"));
  // Bounded: further failures change nothing.
  for (int i = 0; i < 100; ++i) {
    g.RecordGpuFailure("x" + std::to_string(i));
  }
  EXPECT_FALSE(g.AllowGpu("c"));
}

}  // namespace
}  // namespace molt_ai
