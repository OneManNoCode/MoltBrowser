// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/molt_translate_driver.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/molt_ai/translate/page_translate_io.h"
#include "chrome/browser/molt_ai/translate/page_translate_status.h"
#include "chrome/browser/molt_ai/translate/page_translation_job.h"
#include "chrome/browser/molt_ai/web_ai/runtime_web_ai_backend.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "components/translate/core/common/translate_errors.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"

namespace molt_ai::page_translate {

namespace {

// Each tab's generations are one "site" to the web AI scheduler, so tabs
// take turns with each other and with web pages' own API calls.
std::string NextScheduleKey() {
  static int next = 0;
  return "molt-page-translate:" + base::NumberToString(++next);
}

// The page a job translates: the document the tab showed when it started,
// reached in Translate's isolated world, never the page's main world.
class DocumentScriptRunner : public PageScriptRunner {
 public:
  explicit DocumentScriptRunner(content::RenderFrameHost& frame)
      : document_(frame.GetWeakDocumentPtr()) {}
  DocumentScriptRunner(const DocumentScriptRunner&) = delete;
  DocumentScriptRunner& operator=(const DocumentScriptRunner&) = delete;
  ~DocumentScriptRunner() override = default;

  // PageScriptRunner:
  bool IsAlive() override { return Frame() != nullptr; }

  bool IsForeground() override {
    content::RenderFrameHost* frame = Frame();
    content::WebContents* contents =
        frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
    return contents &&
           contents->GetVisibility() == content::Visibility::VISIBLE;
  }

  void Run(const std::string& script,
           base::OnceCallback<void(base::Value)> callback) override {
    content::RenderFrameHost* frame = Frame();
    if (!frame) {
      // Navigated, closed, or in the back/forward cache.
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(callback), base::Value()));
      return;
    }
    // Translate's own isolated world: the page's scripts can't see or patch
    // what runs there (docs/design/PAGE_TRANSLATION.md §4.1). If the frame
    // goes before it answers, the job still hears (with no result).
    frame->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(script),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                    base::Value()),
        ISOLATED_WORLD_ID_TRANSLATE);
  }

 private:
  content::RenderFrameHost* Frame() const {
    content::RenderFrameHost* frame = document_.AsRenderFrameHostIfValid();
    return frame && frame->IsActive() ? frame : nullptr;
  }

  const content::WeakDocumentPtr document_;
};

::translate::TranslateErrors ToTranslateErrors(TranslateError error) {
  switch (error) {
    case TranslateError::kNone:
      return ::translate::TranslateErrors::NONE;
    case TranslateError::kInitialization:
      return ::translate::TranslateErrors::INITIALIZATION_ERROR;
    case TranslateError::kUnknownLanguage:
      return ::translate::TranslateErrors::UNKNOWN_LANGUAGE;
    case TranslateError::kUnsupportedLanguage:
      return ::translate::TranslateErrors::UNSUPPORTED_LANGUAGE;
    case TranslateError::kIdenticalLanguages:
      return ::translate::TranslateErrors::IDENTICAL_LANGUAGES;
    case TranslateError::kTranslation:
      return ::translate::TranslateErrors::TRANSLATION_ERROR;
    case TranslateError::kTimeout:
      return ::translate::TranslateErrors::TRANSLATION_TIMEOUT;
  }
  return ::translate::TranslateErrors::TRANSLATION_ERROR;
}

}  // namespace

MoltTranslateDriver::MoltTranslateDriver(
    content::WebContents& web_contents,
    language::UrlLanguageHistogram* url_language_histogram)
    : ContentTranslateDriver(web_contents, url_language_histogram),
      schedule_key_(NextScheduleKey()) {
  PageTranslateStatus::CreateForWebContents(&web_contents);
}

MoltTranslateDriver::~MoltTranslateDriver() = default;

PageTranslateStatus* MoltTranslateDriver::Status() {
  return web_contents() ? PageTranslateStatus::FromWebContents(web_contents())
                        : nullptr;
}

bool MoltTranslateDriver::TranslatesOnDevice() const {
  return true;
}

void MoltTranslateDriver::PrepareToTranslatePage(
    int page_seq_no,
    std::string_view original_source_lang,
    std::string_view target_lang,
    bool triggered_from_menu) {
  prepared_source_ = std::string(original_source_lang);
}

void MoltTranslateDriver::TranslatePage(int page_seq_no,
                                        std::string_view translate_script,
                                        std::string_view source_lang,
                                        std::string_view target_lang) {
  // |translate_script| is empty: TranslateManager fetches no script for
  // this driver. A new translation replaces one in progress (another
  // target language); the new one restores what the old one wrote before
  // it reads the page.
  job_.reset();
  if (!web_contents()) {
    return;
  }
  const std::string source = prepared_source_.empty()
                                 ? std::string(source_lang)
                                 : std::move(prepared_source_);
  prepared_source_.clear();
  const std::string target(target_lang);
  const PagePair pair = ResolvePagePair(source, target);
  if (pair.problem != PairProblem::kNone) {
    const Report report = ReportForPair(pair.problem);
    if (PageTranslateStatus* status = Status()) {
      status->SetResult(report, /*failed_units=*/0, /*truncated=*/false);
    }
    PostReport(source, target, report.error);
    return;
  }
  if (PageTranslateStatus* status = Status()) {
    status->SetPreparing(base::BindRepeating(&MoltTranslateDriver::StopJob,
                                             weak_factory_.GetWeakPtr()));
  }
  job_ = std::make_unique<PageTranslationJob>(
      std::make_unique<DocumentScriptRunner>(
          *web_contents()->GetPrimaryMainFrame()),
      pair.pair,
      *web_ai::RuntimeWebAIBackend::GetInstance(), schedule_key_,
      base::BindRepeating(&MoltTranslateDriver::OnProgress,
                          weak_factory_.GetWeakPtr()),
      // The page's language in Translate's list, so TranslateManager takes
      // it as supported ("pt" for "pt-BR").
      base::BindOnce(&MoltTranslateDriver::OnJobDone,
                     weak_factory_.GetWeakPtr(),
                     PageTranslateCode(*pair.pair.source), target));
  job_->Start();
}

void MoltTranslateDriver::RevertTranslation(int page_seq_no) {
  job_.reset();
  if (!web_contents()) {
    return;
  }
  if (PageTranslateStatus* status = Status()) {
    status->SetIdle();
  }
  // Puts back every text that still holds its translation, exactly as it
  // was read; text the page changed since stays the page's.
  web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(BuildRevertScript()), base::NullCallback(),
      ISOLATED_WORLD_ID_TRANSLATE);
}

void MoltTranslateDriver::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  ContentTranslateDriver::DidFinishNavigation(navigation_handle);
  if (!navigation_handle->HasCommitted() ||
      !navigation_handle->IsInPrimaryMainFrame() ||
      navigation_handle->IsSameDocument()) {
    return;
  }
  // A new page: the old one's translation ends without a report, as
  // upstream's does when the page's agent goes away.
  job_.reset();
  prepared_source_.clear();
  if (PageTranslateStatus* status = Status()) {
    status->SetIdle();
  }
}

void MoltTranslateDriver::WebContentsDestroyed() {
  job_.reset();
  weak_factory_.InvalidateWeakPtrs();
}

void MoltTranslateDriver::StopJob() {
  if (job_) {
    job_->Stop();
  }
}

void MoltTranslateDriver::OnProgress(size_t settled, size_t total) {
  if (PageTranslateStatus* status = Status()) {
    status->SetProgress(settled, total);
  }
}

void MoltTranslateDriver::OnJobDone(std::string source,
                                    std::string target,
                                    JobResult result) {
  job_.reset();
  const Report report = ReportForJob(result);
  if (PageTranslateStatus* status = Status()) {
    status->SetResult(report, result.failed_units, result.truncated);
  }
  ReportToManager(std::move(source), std::move(target), report.error);
}

void MoltTranslateDriver::PostReport(std::string source,
                                     std::string target,
                                     TranslateError error) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&MoltTranslateDriver::ReportToManager,
                     weak_factory_.GetWeakPtr(), std::move(source),
                     std::move(target), error));
}

void MoltTranslateDriver::ReportToManager(std::string source,
                                          std::string target,
                                          TranslateError error) {
  if (!web_contents()) {
    return;
  }
  // What the renderer's agent would have answered: TranslateManager
  // records the languages and shows the "after" or error step.
  OnPageTranslated(/*cancelled=*/false, source, target,
                   ToTranslateErrors(error));
}

}  // namespace molt_ai::page_translate
