// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_status.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/web_ai/runtime_web_ai_backend.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai::page_translate {

namespace {

// The last answer about this device (process-wide, UI thread).
std::optional<web_ai::DeviceFacts>& LastDeviceFacts() {
  static base::NoDestructor<std::optional<web_ai::DeviceFacts>> facts;
  return *facts;
}

void RememberDeviceFacts(web_ai::DeviceFacts facts) {
  LastDeviceFacts() = facts;
}

}  // namespace

PageTranslateStatus::PageTranslateStatus(content::WebContents* web_contents)
    : content::WebContentsUserData<PageTranslateStatus>(*web_contents) {
  snapshot_.moltnet = molt_ai::tor::IsMoltNetProfile(
      Profile::FromBrowserContext(web_contents->GetBrowserContext()));
  if (const std::optional<web_ai::DeviceFacts>& facts = LastDeviceFacts()) {
    snapshot_.model_installed = facts->model_installed;
    snapshot_.device_supported = facts->inference_supported;
  }
}

PageTranslateStatus::~PageTranslateStatus() {
  for (Observer& observer : observers_) {
    observer.OnPageTranslateStatusDestroyed();
  }
}

void PageTranslateStatus::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void PageTranslateStatus::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void PageTranslateStatus::Changed() {
  for (Observer& observer : observers_) {
    observer.OnPageTranslateStatusChanged();
  }
}

void PageTranslateStatus::SetIdle() {
  stop_.Reset();
  snapshot_.phase = Phase::kIdle;
  snapshot_.report = Report();
  snapshot_.settled = 0;
  snapshot_.total = 0;
  Changed();
}

void PageTranslateStatus::SetPreparing(base::RepeatingClosure stop) {
  stop_ = std::move(stop);
  snapshot_.phase = Phase::kPreparing;
  snapshot_.report = Report();
  snapshot_.settled = 0;
  snapshot_.total = 0;
  Changed();
}

void PageTranslateStatus::SetProgress(size_t settled, size_t total) {
  snapshot_.phase = Phase::kTranslating;
  snapshot_.settled = settled;
  snapshot_.total = total;
  Changed();
}

void PageTranslateStatus::SetResult(const Report& report,
                                    size_t failed_units,
                                    bool truncated) {
  stop_.Reset();
  snapshot_.phase =
      report.error == TranslateError::kNone ? Phase::kDone : Phase::kFailed;
  snapshot_.report = report;
  snapshot_.failed_units = failed_units;
  snapshot_.truncated = truncated;
  if (report.problem == Problem::kNoModel) {
    snapshot_.model_installed = false;
  }
  Changed();
}

void PageTranslateStatus::Stop() {
  if (stop_) {
    // The driver ends the job, which sets the result.
    stop_.Run();
  }
}

void PageTranslateStatus::RefreshDeviceFacts() {
  web_ai::RuntimeWebAIBackend::GetInstance()->GetDeviceFacts(base::BindOnce(
      [](base::WeakPtr<PageTranslateStatus> status,
         web_ai::DeviceFacts facts) {
        RememberDeviceFacts(facts);
        if (!status) {
          return;
        }
        status->snapshot_.model_installed = facts.model_installed;
        status->snapshot_.device_supported = facts.inference_supported;
        status->Changed();
      },
      weak_factory_.GetWeakPtr()));
}

bool ShouldOfferTranslationAutomatically() {
  // The runtime caches its answer for half a minute, so this is cheap.
  web_ai::RuntimeWebAIBackend::GetInstance()->GetDeviceFacts(
      base::BindOnce(&RememberDeviceFacts));
  const std::optional<web_ai::DeviceFacts>& facts = LastDeviceFacts();
  return facts && facts->inference_supported && facts->model_installed;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(PageTranslateStatus);

}  // namespace molt_ai::page_translate
