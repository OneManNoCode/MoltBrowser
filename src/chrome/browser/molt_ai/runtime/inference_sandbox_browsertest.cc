// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A real model, loaded and run by the real sandboxed inference service
// (InferenceProcessHost's own launcher: a utility process under the
// kOnDeviceModelExecution sandbox; docs/design/SANDBOXED_INFERENCE.md):
// a tiny random-weight llama GGUF the test writes (tiny_gguf_for_testing.h)
// loads, a raw prompt generates, and the process is never lost. Every other
// test of the host uses a fake service; the spoken suite's first run with a
// real model found the sandbox killing the load (llama.cpp's posix_fadvise:
// seccomp's SIGSYS), which nothing before it ran into.

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/molt_ai/runtime/inference_process_host.h"
#include "chrome/browser/molt_ai/runtime/stream_validator.h"
#include "chrome/browser/molt_ai/runtime/tiny_gguf_for_testing.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

class LostRecorder : public InferenceProcessHost::Delegate {
 public:
  void OnModelProcessLost(
      const std::string& model_id,
      const std::string& crash_key,
      CrashLoopGuard::Phase phase,
      bool gpu_failure,
      const std::optional<GpuDeviceReport>& gpu_device) override {
    lost.push_back(phase);
  }
  void OnGpuDeviceChosen(
      const GpuDeviceReport& device,
      base::OnceCallback<void(bool, base::File)> reply) override {
    std::move(reply).Run(false, base::File());
  }
  void OnPipelineCacheData(const GpuDeviceReport& device,
                           mojo_base::BigBuffer cache) override {}
  void OnPipelineCacheDropped(const GpuDeviceReport& device) override {}

  std::vector<CrashLoopGuard::Phase> lost;
};

// True once |mailbox| has its outcome; appends its text to |text|.
bool Finished(InferenceMailbox* mailbox, std::string* text) {
  std::string taken;
  const bool finished = mailbox->Wait(std::chrono::milliseconds(0), &taken);
  text->append(taken);
  return finished;
}

using Kind = InferenceOutcome::Kind;

class InferenceSandboxBrowserTest : public InProcessBrowserTest {};

IN_PROC_BROWSER_TEST_F(InferenceSandboxBrowserTest,
                       ATinyModelLoadsAndGeneratesInTheSandbox) {
  // The file is written, opened and (with |dir|) deleted here, on the UI
  // thread: the allowance outlives |dir|.
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("tiny.gguf");
  ASSERT_TRUE(base::WriteFile(path, TinyLlamaGgufForTesting()));
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  ASSERT_TRUE(file.IsValid());
  const uint64_t size = static_cast<uint64_t>(file.GetLength());

  LostRecorder delegate;
  InferenceProcessHost host(&delegate);

  InferenceProcessHost::LoadRequest request;
  request.model_id = "tiny";
  request.crash_key = "tiny:" + std::to_string(size);
  request.file = std::move(file);
  request.file_size = size;
  request.total_ram_bytes = uint64_t{4} << 30;
  request.cpu_threads = 1;
  request.max_context_tokens = kTinyGgufContextLength;
  auto load = base::MakeRefCounted<InferenceMailbox>();
  host.Load(std::move(request), load);
  std::string text;
  ASSERT_TRUE(
      base::test::RunUntil([&] { return Finished(load.get(), &text); }));
  const InferenceOutcome loaded = load->outcome();
  ASSERT_EQ(static_cast<int>(loaded.kind), static_cast<int>(Kind::kOk))
      << "load status " << static_cast<int>(loaded.load_status);
  EXPECT_EQ(loaded.architecture, "llama");
  EXPECT_EQ(loaded.vocab_size, kTinyGgufVocabSize);
  EXPECT_GT(loaded.context_tokens, 0u);
  EXPECT_TRUE(host.HasCurrentModelForTesting());

  // A raw prompt (no chat template): its tokens are random, its outcome is
  // the point.
  auto prompt = mojom::RawPromptRequest::New();
  prompt->prompt = "hello the page";
  auto params = mojom::GenerationParams::New();
  params->max_visible_tokens = 4;
  StreamLimits limits;
  limits.max_visible_tokens = 4;
  auto generation = base::MakeRefCounted<InferenceMailbox>();
  host.StartPrompt(1, std::move(prompt), std::move(params), std::move(limits),
                   generation);
  text.clear();
  ASSERT_TRUE(
      base::test::RunUntil([&] { return Finished(generation.get(), &text); }));
  const InferenceOutcome generated = generation->outcome();
  EXPECT_EQ(static_cast<int>(generated.kind), static_cast<int>(Kind::kOk));
  EXPECT_TRUE(generated.has_summary);

  // Never lost: no SIGSYS, no crash, nothing the host killed it for.
  EXPECT_TRUE(delegate.lost.empty());
  host.Shutdown();
}

}  // namespace
}  // namespace molt_ai
