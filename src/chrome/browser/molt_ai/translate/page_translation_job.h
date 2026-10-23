// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// One on-device translation of one document, started by the user's click
// (or "always translate"): checks that a model is downloaded (it never
// downloads one), reads the page's text in Translate's isolated world,
// loads the model, translates batch by batch on the web AI backend (shared
// with the chat and the web AI APIs, one generation at a time), and writes
// each settled text back as text. Ends on its own, on Stop() or after
// kMaxTranslateMinutes, and reports how far it got; it never reports
// after it is destroyed. No content/: the page is a PageScriptRunner, so
// molt_ai_unittests drives a job with fakes.
// Design: docs/design/PAGE_TRANSLATION.md §4.

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATION_JOB_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATION_JOB_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/functional/function_ref.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/translate/page_translate_plan.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_backend.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"

namespace molt_ai::page_translate {

// What a job needs from its page. The browser's runs scripts in the
// document's Translate isolated world (molt_translate_driver.cc); tests use
// a fake.
class PageScriptRunner {
 public:
  virtual ~PageScriptRunner() = default;
  // The document the job started on is still the tab's page.
  virtual bool IsAlive() = 0;
  // The tab is visible, so its generations go first.
  virtual bool IsForeground() = 0;
  // Runs |script| in that document's Translate isolated world. |callback|
  // gets the script's result, or none (base::Value()) if the document went
  // away. Never calls back synchronously.
  virtual void Run(const std::string& script,
                   base::OnceCallback<void(base::Value)> callback) = 0;
};

class PageTranslationJob {
 public:
  using ProgressCallback =
      base::RepeatingCallback<void(size_t settled, size_t total)>;
  using DoneCallback = base::OnceCallback<void(JobResult)>;

  // |schedule_key| groups the job's generations in the backend's
  // scheduler. Callbacks never run synchronously.
  PageTranslationJob(std::unique_ptr<PageScriptRunner> page,
                     web_ai::TranslatorPair pair,
                     web_ai::WebAIBackend& backend,
                     std::string schedule_key,
                     ProgressCallback on_progress,
                     DoneCallback on_done);
  PageTranslationJob(const PageTranslationJob&) = delete;
  PageTranslationJob& operator=(const PageTranslationJob&) = delete;
  // Cancels the running generation; no callback runs afterwards, and a
  // result not yet delivered is dropped.
  ~PageTranslationJob();

  void Start();
  // Ends the job as soon as what is in flight lands: the running
  // generation is cancelled, a write already sent to the page is waited
  // for (so the count of what was written is exact).
  void Stop();

  // The id the job's state has in the page's isolated world.
  const std::string& request_id() const { return request_id_; }

 private:
  void OnDeviceFacts(web_ai::DeviceFacts facts);
  void OnExtracted(base::Value result);
  void OnModelPrepared(bool ok, uint32_t context_tokens, bool sees_images);
  void SendBatch();
  void OnChunk(const std::string& text);
  void OnGenerationDone(web_ai::GenerationStatus status);
  void Write(std::vector<SlotWrite> writes);
  void OnWritten(size_t asked, base::Value result);
  void Progress();
  // Ends now, or once the write in flight lands.
  void RequestEnd(JobEnd end);
  void Finish(JobEnd end);
  void ReportDone(JobResult result);

  const std::unique_ptr<PageScriptRunner> page_;
  const web_ai::TranslatorPair pair_;
  const raw_ref<web_ai::WebAIBackend> backend_;
  const std::string schedule_key_;
  const std::string request_id_;
  ProgressCallback on_progress_;
  DoneCallback on_done_;

  std::unique_ptr<TranslationPlan> plan_;
  bool truncated_ = false;
  // The batch being translated, and its reply so far.
  std::optional<Batch> batch_;
  std::string reply_;
  bool reply_overflowed_ = false;
  // The backend's id of the running generation (0: none).
  uint64_t generation_ = 0;
  bool writing_ = false;
  std::optional<JobEnd> pending_end_;
  int failed_in_a_row_ = 0;
  int throttled_ = 0;
  size_t written_slots_ = 0;
  bool finished_ = false;
  base::OneShotTimer deadline_;
  base::OneShotTimer retry_;

  base::WeakPtrFactory<PageTranslationJob> weak_factory_{this};
};

// How the driver starts a translation TranslateManager asked for:
// DecideStart() first, so nothing starts in a MoltNet window (whatever the
// manager asked) or for a pair the table doesn't cover. Then |make_job|
// isn't called (the page isn't read, the backend isn't asked anything),
// |job| is null and |decision.report| says why. Otherwise |job| is the one
// |make_job| made for the pair, started.
struct PageTranslationStart {
  PageTranslationStart();
  PageTranslationStart(PageTranslationStart&&);
  PageTranslationStart& operator=(PageTranslationStart&&);
  ~PageTranslationStart();

  StartDecision decision;
  std::unique_ptr<PageTranslationJob> job;
};
PageTranslationStart StartPageTranslation(
    bool moltnet,
    const PagePair& pair,
    base::FunctionRef<std::unique_ptr<PageTranslationJob>(
        const web_ai::TranslatorPair&)> make_job);

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATION_JOB_H_
