// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/filter_list_manager.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/task_runner.h"
#include "base/threading/scoped_blocking_call.h"
#include "chrome/browser/molt_ai/shield/filter_list_catalog.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace molt_ai::shield {

namespace {

// First run (nothing cached yet): fetch almost immediately — until then only
// the small built-in list is active.
constexpr base::TimeDelta kFirstRunDelay = base::Seconds(10);
// No profile network context yet.
constexpr base::TimeDelta kNoNetworkContextDelay = base::Minutes(1);
constexpr base::TimeDelta kDownloadTimeout = base::Minutes(2);

net::NetworkTrafficAnnotationTag FilterListAnnotation() {
  return net::DefineNetworkTrafficAnnotation("molt_shield_filter_lists", R"(
      semantics {
        sender: "MoltShield Filter Lists"
        description:
          "Downloads the public ad/tracker filter lists (EasyList, "
          "EasyPrivacy, uBlock Origin's lists) that MoltBrowser's built-in "
          "blocker compiles on-device."
        trigger:
          "Shortly after first run, then about once a day with random "
          "jitter, and when the user clicks 'Update lists now'."
        data:
          "None. Plain GET without cookies or credentials; only the "
          "conditional headers If-None-Match / If-Modified-Since from the "
          "previous download."
        destination: WEBSITE
        internal {
          contacts { email: "security@geneye.ai" }
        }
        user_data { type: NONE }
        last_reviewed: "2026-10-14"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Turn MoltShield off in MoltBrowser settings (molt://ai-settings)."
        policy_exception_justification: "Not yet covered by enterprise policy."
      })");
}

}  // namespace

struct FilterListManager::ProcessResult {
  enum class Outcome { kChanged, kUnchanged, kRejected, kIoError };
  Outcome outcome = Outcome::kIoError;
  std::string sha256;
  int64_t size = 0;
  ListMetadata metadata;
  std::string error;
};

FilterListManager::Pass::Pass() = default;
FilterListManager::Pass::~Pass() = default;

FilterListManager::FilterListManager(
    const base::FilePath& dir,
    scoped_refptr<base::SequencedTaskRunner> file_task_runner,
    LoaderFactoryGetter loader_factory_getter,
    PassDoneCallback pass_done)
    : dir_(dir),
      file_task_runner_(std::move(file_task_runner)),
      loader_factory_getter_(std::move(loader_factory_getter)),
      pass_done_(std::move(pass_done)) {}

FilterListManager::~FilterListManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void FilterListManager::Start(StoreState state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  state_ = std::move(state);
  file_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(base::IgnoreResult(&base::CreateDirectory),
                     ListPath(dir_, "x").DirName()));

  bool any_cached = false;
  for (const ListSpec& spec : EnabledListSpecs()) {
    any_cached |= !spec.expected_sha256.empty();
  }
  ScheduleNext(any_cached
                   ? NextUpdateDelay(state_.settings.last_update_check,
                                     base::Time::Now(),
                                     base::RandTimeDelta(-kMaxUpdateJitter,
                                                         kMaxUpdateJitter))
                   : kFirstRunDelay);
}

void FilterListManager::UpdateNow() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (pass_) {
    return;
  }
  timer_.Stop();
  StartPass(/*force=*/true);
}

void FilterListManager::SaveState() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  file_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(base::IgnoreResult(&molt_ai::shield::SaveState),
                                dir_, state_));
}

void FilterListManager::SetListEnabled(const std::string& id, bool enabled) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const FilterListEntry* entry = FindFilterList(id);
  if (!entry || IsListEnabled(*entry, state_.settings) == enabled) {
    return;
  }
  state_.settings.list_enabled[id] = enabled;
  SaveState();
  auto it = state_.lists.find(id);
  bool downloaded = it != state_.lists.end() && !it->second.sha256_hex.empty();
  if (enabled && !downloaded) {
    UpdateNow();  // Recompiles when the download lands.
  } else {
    pass_done_.Run(/*lists_changed=*/true);
  }
}

std::vector<ListSpec> FilterListManager::EnabledListSpecs() const {
  return molt_ai::shield::EnabledListSpecs(state_);
}

std::vector<ListStatus> FilterListManager::GetListStatus() const {
  std::vector<ListStatus> out;
  for (const FilterListEntry& entry : GetFilterListCatalog()) {
    ListStatus status;
    status.id = entry.id;
    status.title = entry.title;
    status.enabled = IsListEnabled(entry, state_.settings);
    auto it = state_.lists.find(entry.id);
    if (it != state_.lists.end()) {
      const ListState& s = it->second;
      status.cached = !s.sha256_hex.empty();
      status.rule_lines = s.rule_lines;
      status.version = s.version;
      status.sha256_hex = s.sha256_hex;
      status.fetched_at = s.fetched_at;
      status.checked_at = s.checked_at;
      status.last_error = s.last_error;
      if (!s.title.empty()) {
        status.title = s.title;
      }
    }
    out.push_back(std::move(status));
  }
  return out;
}

void FilterListManager::SetRuleLines(
    const std::map<std::string, uint32_t>& rule_lines) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (const auto& [id, lines] : rule_lines) {
    state_.lists[id].rule_lines = lines;
  }
  SaveState();
}

void FilterListManager::SetSignatureVerifier(
    std::unique_ptr<ListSignatureVerifier> verifier) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  signature_verifier_ = std::move(verifier);
}

void FilterListManager::ScheduleNext(base::TimeDelta delay) {
  timer_.Start(FROM_HERE, delay,
               base::BindOnce(&FilterListManager::StartPass,
                              weak_factory_.GetWeakPtr(), /*force=*/false));
}

void FilterListManager::StartPass(bool force) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (pass_) {
    return;
  }
  // TODO(molt-update): when kSignedBundleUrl is set and a verifier is
  // installed, fetch the signed GenEye bundle first and only fall back to the
  // per-publisher URLs below if it is unavailable or fails verification.
  pass_.emplace();
  pass_->force = force;
  for (const FilterListEntry& entry : GetFilterListCatalog()) {
    if (IsListEnabled(entry, state_.settings)) {
      pass_->pending.push_back(entry.id);
    }
  }
  FetchNext();
}

void FilterListManager::FetchNext() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!pass_) {
    return;
  }
  if (pass_->pending.empty()) {
    FinishPass();
    return;
  }
  std::string id = std::move(pass_->pending.front());
  pass_->pending.erase(pass_->pending.begin());
  const FilterListEntry* entry = FindFilterList(id);
  if (!entry) {
    FetchNext();
    return;
  }
  Fetch(*entry, /*use_fallback=*/false);
}

void FilterListManager::Fetch(const FilterListEntry& entry,
                              bool use_fallback) {
  scoped_refptr<network::SharedURLLoaderFactory> factory =
      loader_factory_getter_.Run();
  if (!factory) {
    // No profile network context yet. Never fall back to a direct/system
    // context: retry the whole pass later.
    pass_.reset();
    ScheduleNext(kNoNetworkContextDelay);
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(use_fallback ? entry.fallback_url : entry.url);
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_DISABLE_CACHE;
  auto it = state_.lists.find(entry.id);
  if (it != state_.lists.end() && !it->second.sha256_hex.empty()) {
    // Only revalidate when we still hold the copy the validators describe.
    if (!it->second.etag.empty()) {
      request->headers.SetHeader("If-None-Match", it->second.etag);
    }
    if (!it->second.last_modified.empty()) {
      request->headers.SetHeader("If-Modified-Since",
                                 it->second.last_modified);
    }
  }

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             FilterListAnnotation());
  loader_->SetAllowHttpErrorResults(true);  // To see 304 and keep the code.
  loader_->SetRetryOptions(
      2, network::SimpleURLLoader::RETRY_ON_NETWORK_CHANGE |
             network::SimpleURLLoader::RETRY_ON_5XX);
  loader_->SetTimeoutDuration(kDownloadTimeout);
  base::FilePath temp_path =
      ListPath(dir_, entry.id).AddExtensionASCII("download");
  loader_->DownloadToFile(
      factory.get(),
      base::BindOnce(
          [](base::WeakPtr<FilterListManager> self, std::string id,
             bool used_fallback, base::FilePath path) {
            if (self) {
              self->OnDownloaded(std::move(id), used_fallback,
                                 std::move(self->loader_), std::move(path));
            }
          },
          weak_factory_.GetWeakPtr(), std::string(entry.id), use_fallback),
      temp_path, kMaxListBytes);
}

void FilterListManager::OnDownloaded(
    std::string id,
    bool used_fallback,
    std::unique_ptr<network::SimpleURLLoader> loader,
    base::FilePath temp_path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const int net_error = loader->NetError();
  int code = 0;
  std::string etag;
  std::string last_modified;
  if (const network::mojom::URLResponseHead* head = loader->ResponseInfo();
      head && head->headers) {
    code = head->headers->response_code();
    etag = head->headers->GetNormalizedHeader("ETag").value_or("");
    last_modified =
        head->headers->GetNormalizedHeader("Last-Modified").value_or("");
  }

  if (net_error == net::OK && code == 200 && !temp_path.empty()) {
    auto it = state_.lists.find(id);
    std::string previous_sha =
        it != state_.lists.end() ? it->second.sha256_hex : std::string();
    file_task_runner_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&FilterListManager::ProcessDownload, dir_, id,
                       temp_path, std::move(previous_sha)),
        base::BindOnce(&FilterListManager::OnProcessed,
                       weak_factory_.GetWeakPtr(), id, std::move(etag),
                       std::move(last_modified)));
    return;
  }

  if (!temp_path.empty()) {
    file_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(base::IgnoreResult(&base::DeleteFile),
                                  temp_path));
  }

  if (net_error == net::OK && code == 304) {
    ListState& list = state_.lists[id];
    list.checked_at = base::Time::Now();
    list.last_error.clear();
    pass_->any_success = true;
    FetchNext();
    return;
  }

  const FilterListEntry* entry = FindFilterList(id);
  if (!used_fallback && entry && entry->fallback_url[0] != '\0') {
    Fetch(*entry, /*use_fallback=*/true);
    return;
  }
  RecordFailure(id, net_error != net::OK
                        ? net::ErrorToShortString(net_error)
                        : base::StrCat({"HTTP ", base::NumberToString(code)}));
  FetchNext();
}

// static
FilterListManager::ProcessResult FilterListManager::ProcessDownload(
    base::FilePath dir,
    std::string id,
    base::FilePath temp_path,
    std::string previous_sha256) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  ProcessResult result;
  std::optional<std::string> text = ReadFileCapped(temp_path, kMaxListBytes);
  if (!text) {
    base::DeleteFile(temp_path);
    result.outcome = ProcessResult::Outcome::kIoError;
    result.error = "could not read download";
    return result;
  }
  if (!LooksLikeFilterList(*text)) {
    base::DeleteFile(temp_path);
    result.outcome = ProcessResult::Outcome::kRejected;
    result.error = "content is not a filter list";
    return result;
  }
  result.sha256 = Sha256Hex(*text);
  result.size = static_cast<int64_t>(text->size());
  result.metadata = ParseListMetadata(*text);
  if (result.sha256 == previous_sha256 &&
      base::PathExists(ListPath(dir, id))) {
    base::DeleteFile(temp_path);
    result.outcome = ProcessResult::Outcome::kUnchanged;
    return result;
  }
  if (!base::ReplaceFile(temp_path, ListPath(dir, id), nullptr)) {
    base::DeleteFile(temp_path);
    result.outcome = ProcessResult::Outcome::kIoError;
    result.error = "could not update cache";
    return result;
  }
  result.outcome = ProcessResult::Outcome::kChanged;
  return result;
}

void FilterListManager::OnProcessed(std::string id,
                                    std::string etag,
                                    std::string last_modified,
                                    ProcessResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!pass_) {
    return;
  }
  using Outcome = ProcessResult::Outcome;
  if (result.outcome == Outcome::kRejected ||
      result.outcome == Outcome::kIoError) {
    RecordFailure(id, std::move(result.error));
    FetchNext();
    return;
  }
  ListState& list = state_.lists[id];
  const base::Time now = base::Time::Now();
  list.etag = std::move(etag);
  list.last_modified = std::move(last_modified);
  list.checked_at = now;
  list.last_error.clear();
  if (result.outcome == Outcome::kChanged) {
    list.sha256_hex = result.sha256;
    list.size = result.size;
    list.title = result.metadata.title;
    list.version = result.metadata.version;
    list.fetched_at = now;
    pass_->changed = true;
  }
  pass_->any_success = true;
  FetchNext();
}

void FilterListManager::RecordFailure(const std::string& id,
                                      std::string error) {
  LOG(WARNING) << "[MoltShield] list update failed: " << id << ": " << error;
  state_.lists[id].last_error = std::move(error);
}

void FilterListManager::FinishPass() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const bool changed = pass_->changed;
  const bool any_success = pass_->any_success;
  pass_.reset();

  const base::Time now = base::Time::Now();
  if (any_success) {
    state_.settings.last_update_check = now;
  }
  SaveState();
  ScheduleNext(any_success
                   ? NextUpdateDelay(now, now,
                                     base::RandTimeDelta(-kMaxUpdateJitter,
                                                         kMaxUpdateJitter))
                   : kRetryAfterFailure +
                         base::RandTimeDeltaUpTo(base::Minutes(30)));
  pass_done_.Run(changed);
}

}  // namespace molt_ai::shield
