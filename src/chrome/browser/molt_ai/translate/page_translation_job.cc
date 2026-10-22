// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translation_job.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/translate/page_translate_io.h"

namespace molt_ai::page_translate {

namespace {

// A batch's reply is at most web_ai::kTranslatorMaxReplyTokens tokens; this
// is far beyond what they can spell. More means something is wrong, and
// the batch is rejected.
constexpr size_t kMaxReplyBytes = 64 * 1024;

// The backend's queue for all pages was full: try again shortly, a few
// times.
constexpr base::TimeDelta kThrottledRetryDelay = base::Seconds(1);
constexpr int kMaxThrottledRetries = 60;

}  // namespace

PageTranslationJob::PageTranslationJob(std::unique_ptr<PageScriptRunner> page,
                                       web_ai::TranslatorPair pair,
                                       web_ai::WebAIBackend& backend,
                                       std::string schedule_key,
                                       ProgressCallback on_progress,
                                       DoneCallback on_done)
    : page_(std::move(page)),
      pair_(pair),
      backend_(backend),
      schedule_key_(std::move(schedule_key)),
      request_id_(base::NumberToString(base::RandUint64())),
      on_progress_(std::move(on_progress)),
      on_done_(std::move(on_done)) {}

PageTranslationJob::~PageTranslationJob() {
  if (generation_ != 0) {
    backend_->Cancel(generation_);
  }
}

void PageTranslationJob::Start() {
  deadline_.Start(FROM_HERE, base::Minutes(kMaxTranslateMinutes),
                  base::BindOnce(&PageTranslationJob::RequestEnd,
                                 weak_factory_.GetWeakPtr(),
                                 JobEnd::kTimedOut));
  // First whether there is a model at all: nothing is read from the page
  // and nothing is downloaded if there isn't.
  backend_->GetDeviceFacts(base::BindOnce(&PageTranslationJob::OnDeviceFacts,
                                          weak_factory_.GetWeakPtr()));
}

void PageTranslationJob::Stop() {
  RequestEnd(JobEnd::kStopped);
}

void PageTranslationJob::OnDeviceFacts(web_ai::DeviceFacts facts) {
  if (!facts.inference_supported) {
    Finish(JobEnd::kDeviceUnsupported);
    return;
  }
  if (!facts.model_installed) {
    Finish(JobEnd::kNoModel);
    return;
  }
  page_->Run(BuildExtractScript(request_id_),
             base::BindOnce(&PageTranslationJob::OnExtracted,
                            weak_factory_.GetWeakPtr()));
}

void PageTranslationJob::OnExtracted(base::Value result) {
  std::optional<PageText> page = ParseExtraction(result, request_id_);
  if (!page) {
    Finish(JobEnd::kPageUnreadable);
    return;
  }
  truncated_ = page->truncated;
  plan_ = std::make_unique<TranslationPlan>(*page);
  if (plan_->finished()) {
    // Nothing for the model: no model is loaded for it.
    Finish(JobEnd::kFinished);
    return;
  }
  Progress();
  backend_->PrepareModel(/*wants_images=*/false,
                         base::BindOnce(&PageTranslationJob::OnModelPrepared,
                                        weak_factory_.GetWeakPtr()));
}

void PageTranslationJob::OnModelPrepared(bool ok,
                                         uint32_t context_tokens,
                                         bool sees_images) {
  if (!ok) {
    Finish(JobEnd::kModelFailed);
    return;
  }
  batch_ = plan_->NextBatch();
  SendBatch();
}

void PageTranslationJob::SendBatch() {
  if (pending_end_) {
    Finish(*pending_end_);
    return;
  }
  if (!batch_) {
    Finish(JobEnd::kFinished);
    return;
  }
  if (!page_->IsAlive()) {
    Finish(JobEnd::kPageUnreadable);
    return;
  }
  // MoltBrowser's fixed instruction; the page's texts only in the user turn
  // (untrusted content to the inference service).
  web_ai::TaskPrompt prompt =
      web_ai::BuildTranslatorBatchPrompt(pair_, batch_->texts);
  web_ai::GenerationJob job;
  job.site_key = schedule_key_;
  job.foreground = page_->IsForeground();
  job.system_prompt = std::move(prompt.system_prompt);
  job.user_message = std::move(prompt.user_message);
  job.max_output_tokens = prompt.max_output_tokens;
  job.sampling.temperature = prompt.temperature;
  job.sampling.top_k = prompt.top_k;
  // The model can only write one string per text (constrained decoding).
  job.json_schema = web_ai::TranslatorBatchResponseSchema(batch_->texts.size());
  reply_.clear();
  reply_overflowed_ = false;
  generation_ = backend_->Generate(
      std::move(job),
      base::BindRepeating(&PageTranslationJob::OnChunk,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&PageTranslationJob::OnGenerationDone,
                     weak_factory_.GetWeakPtr()));
}

void PageTranslationJob::OnChunk(const std::string& text) {
  if (reply_overflowed_ || reply_.size() + text.size() > kMaxReplyBytes) {
    reply_overflowed_ = true;
    return;
  }
  reply_ += text;
}

void PageTranslationJob::OnGenerationDone(web_ai::GenerationStatus status) {
  generation_ = 0;
  if (pending_end_) {
    Finish(*pending_end_);
    return;
  }
  std::vector<SlotWrite> writes;
  switch (status) {
    case web_ai::GenerationStatus::kOk: {
      std::optional<std::vector<std::string>> translations =
          reply_overflowed_
              ? std::nullopt
              : ReadBatchReply(reply_, batch_->texts.size());
      if (translations) {
        failed_in_a_row_ = 0;
        throttled_ = 0;
        writes = plan_->AcceptReply(*batch_, *translations);
      } else {
        ++failed_in_a_row_;
        writes = plan_->RejectBatch(*batch_);
      }
      break;
    }
    case web_ai::GenerationStatus::kInvalidOutput:
    case web_ai::GenerationStatus::kOutputTruncated:
      ++failed_in_a_row_;
      writes = plan_->RejectBatch(*batch_);
      break;
    case web_ai::GenerationStatus::kThrottled:
      // The backend's queue for every page together was full; the batch
      // goes again as it is.
      if (++throttled_ > kMaxThrottledRetries) {
        Finish(JobEnd::kModelErrors);
        return;
      }
      retry_.Start(FROM_HERE, kThrottledRetryDelay,
                   base::BindOnce(&PageTranslationJob::SendBatch,
                                  weak_factory_.GetWeakPtr()));
      return;
    case web_ai::GenerationStatus::kCancelled:
      // The backend's per-generation deadline (or shutdown).
      Finish(JobEnd::kTimedOut);
      return;
    case web_ai::GenerationStatus::kFailed:
    case web_ai::GenerationStatus::kConstraintUnsupported:
      Finish(JobEnd::kModelErrors);
      return;
  }
  reply_.clear();
  if (failed_in_a_row_ >= kMaxFailedBatchesInARow) {
    Finish(JobEnd::kModelErrors);
    return;
  }
  batch_ = plan_->NextBatch();
  Progress();
  if (writes.empty()) {
    SendBatch();
    return;
  }
  Write(std::move(writes));
}

void PageTranslationJob::Write(std::vector<SlotWrite> writes) {
  writing_ = true;
  const size_t asked = writes.size();
  page_->Run(BuildApplyScript(request_id_, writes),
             base::BindOnce(&PageTranslationJob::OnWritten,
                            weak_factory_.GetWeakPtr(), asked));
}

void PageTranslationJob::OnWritten(size_t asked, base::Value result) {
  writing_ = false;
  const std::optional<size_t> written =
      ParseApplyResult(result, request_id_, asked);
  if (!written) {
    // The page lost the job's state, or the frame went away.
    Finish(pending_end_.value_or(JobEnd::kPageUnreadable));
    return;
  }
  written_slots_ += *written;
  SendBatch();
}

void PageTranslationJob::Progress() {
  if (plan_) {
    on_progress_.Run(plan_->settled_units(), plan_->unit_count());
  }
}

void PageTranslationJob::RequestEnd(JobEnd end) {
  if (finished_ || pending_end_) {
    return;
  }
  pending_end_ = end;
  if (writing_) {
    return;  // OnWritten() ends it, with the write counted.
  }
  if (generation_ != 0) {
    backend_->Cancel(generation_);
    generation_ = 0;
  }
  Finish(end);
}

void PageTranslationJob::Finish(JobEnd end) {
  if (finished_) {
    return;
  }
  finished_ = true;
  deadline_.Stop();
  retry_.Stop();
  if (generation_ != 0) {
    backend_->Cancel(generation_);
    generation_ = 0;
  }
  JobResult result;
  result.end = end;
  result.units = plan_ ? plan_->unit_count() : 0;
  result.failed_units = plan_ ? plan_->failed_units() : 0;
  result.written_slots = written_slots_;
  result.truncated = truncated_;
  // Nothing in flight (a script's result, a timer) reaches the job now.
  weak_factory_.InvalidateWeakPtrs();
  // Never synchronously, and not at all once the job is destroyed: the
  // owner may destroy it when it hears.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&PageTranslationJob::ReportDone,
                                weak_factory_.GetWeakPtr(), result));
}

void PageTranslationJob::ReportDone(JobResult result) {
  std::move(on_done_).Run(result);
}

}  // namespace molt_ai::page_translate
