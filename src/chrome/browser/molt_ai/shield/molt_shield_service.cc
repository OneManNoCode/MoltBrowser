// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_service.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/shield/adblock/molt_adblock.rs.h"
#include "chrome/browser/molt_ai/shield/filter_list_manager.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "content/public/browser/browser_thread.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace molt_ai::shield {

namespace {

constexpr char kShieldDirName[] = "MoltShield";

std::atomic<MoltShieldService*> g_instance{nullptr};

bool IsAdType(std::string_view type) {
  return type == kTypeSubdocument || type == "image" || type == "media" ||
         type == "script" || type == "object";
}

}  // namespace

// static
MoltShieldService* MoltShieldService::Get() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  static base::NoDestructor<MoltShieldService> instance;
  if (!g_instance.load(std::memory_order_acquire)) {
    g_instance.store(instance.get(), std::memory_order_release);
    instance->Initialize();
  }
  return instance.get();
}

// static
MoltShieldService* MoltShieldService::GetIfExists() {
  return g_instance.load(std::memory_order_acquire);
}

MoltShieldService::MoltShieldService() = default;

// Never runs in production (NoDestructor).
MoltShieldService::~MoltShieldService() = default;

void MoltShieldService::Initialize() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  base::FilePath user_data_dir;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
    LOG(ERROR) << "[MoltShield] no user data dir; lists won't be cached";
  }
  dir_ = user_data_dir.AppendASCII(kShieldDirName);

  // Matching is on the critical path of every page load.
  engine_task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN});
  file_task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN});
  engine_.emplace(engine_task_runner_);

  lists_ = std::make_unique<FilterListManager>(
      dir_, file_task_runner_,
      base::BindRepeating(&MoltShieldService::GetLoaderFactory,
                          base::Unretained(this)),
      base::BindRepeating(&MoltShieldService::OnListPassDone,
                          weak_factory_.GetWeakPtr()));

  // --disable-molt-shield: the hooks are already bypassed; also never load
  // the engine or start the list updater (no disk or network activity). The
  // settings page still works and says why everything is off.
  if (molt_shield::IsMoltShieldDisabledByCommandLine()) {
    disabled_by_command_line_ = true;
    ApplySettingsToAtomics();
    LOG(WARNING) << "[MoltShield] disabled by --"
                 << molt_shield::switches::kDisableMoltShield;
    return;
  }

  // First task on the engine sequence: every Check() posted after this
  // waits for the engine instead of being answered "allow".
  engine_.AsyncCall(&MoltShieldEngine::Startup)
      .WithArgs(dir_)
      .Then(base::BindOnce(&MoltShieldService::OnStartupLoaded,
                           weak_factory_.GetWeakPtr()));
}

void MoltShieldService::OnStartupLoaded(
    MoltShieldEngine::StartupResult result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  engine_ready_ = true;
  LOG(INFO) << "[MoltShield] engine ready ("
            << (result.summary.from_cache ? "from cache" : "compiled")
            << (result.summary.used_builtin_list ? ", built-in list only" : "")
            << ")";
  lists_->Start(std::move(result.state));
  if (!result.summary.rule_lines.empty()) {
    lists_->SetRuleLines(result.summary.rule_lines);
  }
  ApplySettingsToAtomics();
}

void MoltShieldService::ApplySettingsToAtomics() {
  const ShieldSettings& s = lists_->state().settings;
  enabled_.store(s.enabled && !disabled_by_command_line_,
                 std::memory_order_relaxed);
  cosmetic_enabled_.store(s.cosmetic, std::memory_order_relaxed);
  consent_killer_enabled_.store(s.consent_killer, std::memory_order_relaxed);
}

void MoltShieldService::Check(CheckInput input,
                              base::OnceCallback<void(CheckResult)> reply) {
  engine_.AsyncCall(&MoltShieldEngine::Check)
      .WithArgs(std::move(input))
      .Then(std::move(reply));
}

void MoltShieldService::GetCosmetic(
    const GURL& document_url,
    base::OnceCallback<void(CosmeticData)> reply) {
  engine_.AsyncCall(&MoltShieldEngine::Cosmetic)
      .WithArgs(document_url)
      .Then(std::move(reply));
}

void MoltShieldService::GetHiddenClassIdCss(
    const GURL& document_url,
    std::vector<std::string> classes,
    std::vector<std::string> ids,
    base::OnceCallback<void(std::string)> reply) {
  engine_.AsyncCall(&MoltShieldEngine::HiddenClassIdCss)
      .WithArgs(document_url, std::move(classes), std::move(ids))
      .Then(std::move(reply));
}

void MoltShieldService::RecordCheck(const CheckResult& result,
                                    std::string_view request_type) {
  total_requests_checked_.fetch_add(1, std::memory_order_relaxed);
  if (!result.should_block) {
    return;
  }
  trackers_blocked_.fetch_add(1, std::memory_order_relaxed);
  if (IsAdType(request_type)) {
    ads_blocked_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!result.redirect_data_url.empty()) {
    redirects_served_.fetch_add(1, std::memory_order_relaxed);
  }
  if (request_type == kTypeWebSocket) {
    websockets_blocked_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MoltShieldService::RecordCosmetic(const CosmeticData& data) {
  if (!data.hide_css.empty()) {
    cosmetic_rules_applied_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!data.scriptlet_js.empty()) {
    scriptlets_injected_.fetch_add(1, std::memory_order_relaxed);
  }
}

ShieldStats MoltShieldService::GetStats() const {
  ShieldStats stats;
  stats.total_requests_checked = total_requests_checked_.load();
  stats.trackers_blocked = trackers_blocked_.load();
  stats.ads_blocked = ads_blocked_.load();
  stats.redirects_served = redirects_served_.load();
  stats.websockets_blocked = websockets_blocked_.load();
  stats.cosmetic_rules_applied = cosmetic_rules_applied_.load();
  stats.scriptlets_injected = scriptlets_injected_.load();
  return stats;
}

void MoltShieldService::SetEnabled(bool enabled) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanChangeSettings()) {
    return;
  }
  lists_->mutable_settings().enabled = enabled;
  lists_->SaveState();
  ApplySettingsToAtomics();
}

void MoltShieldService::SetCosmeticEnabled(bool enabled) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanChangeSettings()) {
    return;
  }
  lists_->mutable_settings().cosmetic = enabled;
  lists_->SaveState();
  ApplySettingsToAtomics();
}

void MoltShieldService::SetConsentKillerEnabled(bool enabled) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanChangeSettings()) {
    return;
  }
  lists_->mutable_settings().consent_killer = enabled;
  lists_->SaveState();
  ApplySettingsToAtomics();
}

void MoltShieldService::RegisterProfile(Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (update_profile_ || !profile) {
    return;
  }
  // Never an OTR context: use its original profile (still MoltNet-routed
  // when MoltNet is on — MoltNetRouting applies to both).
  update_profile_ = profile->GetOriginalProfile();
  profile_observation_.Observe(update_profile_.get());
}

void MoltShieldService::OnProfileWillBeDestroyed(Profile* profile) {
  if (profile == update_profile_) {
    profile_observation_.Reset();
    update_profile_ = nullptr;
  }
}

scoped_refptr<network::SharedURLLoaderFactory>
MoltShieldService::GetLoaderFactory() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // The profile's own network context: honors the profile proxy, which is
  // the fail-closed Tor SOCKS proxy while MoltNet is on. Deliberately not
  // g_browser_process->shared_url_loader_factory().
  return update_profile_ ? update_profile_->GetURLLoaderFactory() : nullptr;
}

const ShieldSettings& MoltShieldService::settings() const {
  return lists_->state().settings;
}

std::vector<ListStatus> MoltShieldService::GetListStatus() const {
  return lists_->GetListStatus();
}

void MoltShieldService::SetListEnabled(const std::string& id, bool enabled) {
  if (CanChangeSettings()) {
    lists_->SetListEnabled(id, enabled);
  }
}

void MoltShieldService::UpdateListsNow() {
  if (CanChangeSettings()) {
    lists_->UpdateNow();
  }
}

bool MoltShieldService::CanChangeSettings() const {
  // The persisted state arrives with the engine (OnStartupLoaded), and never
  // under --disable-molt-shield. Saving before that would overwrite it with
  // defaults, and an update pass would hit the network.
  return engine_ready_;
}

bool MoltShieldService::IsUpdating() const {
  return lists_->update_in_progress() || rebuild_in_flight_;
}

base::Time MoltShieldService::last_update_check() const {
  return lists_->state().settings.last_update_check;
}

std::string MoltShieldService::EngineVersion() const {
  return std::string(ffi::engine_format_key());
}

void MoltShieldService::OnListPassDone(bool lists_changed) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (lists_changed) {
    Rebuild();
  }
}

void MoltShieldService::Rebuild() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (rebuild_in_flight_) {
    rebuild_pending_ = true;
    return;
  }
  rebuild_in_flight_ = true;
  // Compile off the engine sequence so matching continues on the current
  // engine meanwhile.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&MoltShieldEngine::BuildAndPersist, dir_,
                     lists_->EnabledListSpecs(), /*is_mobile=*/false),
      base::BindOnce(&MoltShieldService::OnRebuilt,
                     weak_factory_.GetWeakPtr()));
}

void MoltShieldService::OnRebuilt(
    std::unique_ptr<MoltShieldEngine::Compiled> compiled) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  engine_.AsyncCall(&MoltShieldEngine::Swap)
      .WithArgs(std::move(compiled))
      .Then(base::BindOnce(&MoltShieldService::OnEngineSwapped,
                           weak_factory_.GetWeakPtr()));
}

void MoltShieldService::OnEngineSwapped(MoltShieldEngine::Summary summary) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  rebuild_in_flight_ = false;
  lists_->SetRuleLines(summary.rule_lines);
  LOG(INFO) << "[MoltShield] engine rebuilt and swapped in";
  if (rebuild_pending_) {
    rebuild_pending_ = false;
    Rebuild();
  }
}

}  // namespace molt_ai::shield
