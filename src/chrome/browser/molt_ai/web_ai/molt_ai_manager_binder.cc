// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/molt_ai_manager_binder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base/callback_list.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/web_ai/molt_ai_manager.h"
#include "chrome/browser/molt_ai/web_ai/runtime_web_ai_backend.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_consent.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions_service.h"
#include "chrome/browser/profiles/profile.h"
#include "components/optimization_guide/core/model_execution/model_execution_prefs.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/common/page_visibility_state.h"
#include "net/base/schemeful_site.h"
#include "services/network/public/mojom/permissions_policy/permissions_policy_feature.mojom.h"
#include "third_party/blink/public/common/features_generated.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace molt_ai::web_ai {

namespace {

namespace prefs = optimization_guide::model_execution::prefs;

bool IsApiFeatureEnabled(WebAIApi api) {
  switch (api) {
    case WebAIApi::kLanguageModel:
      // As upstream's AIManager::IsPromptApiEnabled().
      return base::FeatureList::IsEnabled(blink::features::kAIPromptAPI) ||
             base::FeatureList::IsEnabled(
                 blink::features::kAIPromptAPIMultimodalInput);
    case WebAIApi::kSummarizer:
      return base::FeatureList::IsEnabled(
          blink::features::kAISummarizationAPI);
    case WebAIApi::kWriter:
      return base::FeatureList::IsEnabled(blink::features::kAIWriterAPI);
    case WebAIApi::kRewriter:
      return base::FeatureList::IsEnabled(blink::features::kAIRewriterAPI);
  }
  return false;
}

network::mojom::PermissionsPolicyFeature PolicyFeature(WebAIApi api) {
  switch (api) {
    case WebAIApi::kLanguageModel:
      return network::mojom::PermissionsPolicyFeature::kLanguageModel;
    case WebAIApi::kSummarizer:
      return network::mojom::PermissionsPolicyFeature::kSummarizer;
    case WebAIApi::kWriter:
      return network::mojom::PermissionsPolicyFeature::kWriter;
    case WebAIApi::kRewriter:
      return network::mojom::PermissionsPolicyFeature::kRewriter;
  }
  return network::mojom::PermissionsPolicyFeature::kLanguageModel;
}

// The policies upstream's AIManager honours: BuiltInAIAPIsEnabled (profile)
// and GenAILocalFoundationalModelSettings (local state).
bool EnterpriseAllows(Profile* profile) {
  PrefService* local_state =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  if (local_state &&
      local_state->FindPreference(
          prefs::localstate::
              kGenAILocalFoundationalModelEnterprisePolicySettings) &&
      local_state->GetInteger(
          prefs::localstate::
              kGenAILocalFoundationalModelEnterprisePolicySettings) ==
          static_cast<int>(
              prefs::GenAILocalFoundationalModelEnterprisePolicySettings::
                  kDisallowed)) {
    return false;
  }
  PrefService* profile_prefs = profile ? profile->GetPrefs() : nullptr;
  if (profile_prefs &&
      profile_prefs->FindPreference(policy::policy_prefs::kBuiltInAIAPIsEnabled) &&
      !profile_prefs->GetBoolean(policy::policy_prefs::kBuiltInAIAPIsEnabled)) {
    return false;
  }
  return true;
}

// Chromium's "On-device AI" switch (chrome://settings/system).
bool UserAllows() {
  PrefService* local_state =
      g_browser_process ? g_browser_process->local_state() : nullptr;
  return !local_state ||
         !local_state->FindPreference(
             prefs::localstate::kOnDeviceAiUserSettingsEnabled) ||
         local_state->GetBoolean(
             prefs::localstate::kOnDeviceAiUserSettingsEnabled);
}

// A document's or worker's facts, read when asked (the document can move
// in and out of the back/forward cache; its origin never changes).
class FrameEnvironment : public WebAIEnvironment {
 public:
  FrameEnvironment(content::BrowserContext* browser_context,
                   content::RenderFrameHost* rfh)
      : browser_context_(browser_context),
        is_document_(rfh != nullptr),
        document_(rfh ? rfh->GetWeakDocumentPtr()
                      : content::WeakDocumentPtr()) {}
  FrameEnvironment(const FrameEnvironment&) = delete;
  FrameEnvironment& operator=(const FrameEnvironment&) = delete;
  ~FrameEnvironment() override = default;

  // WebAIEnvironment:
  ContextFacts GetContextFacts(WebAIApi api) override {
    ContextFacts facts;
    facts.feature_enabled = IsApiFeatureEnabled(api);
    Profile* profile = Profile::FromBrowserContext(browser_context_);
    facts.moltnet = tor::IsMoltNetProfile(profile);
    facts.enterprise_allowed = EnterpriseAllows(profile);
    facts.user_enabled = UserAllows();
    content::RenderFrameHost* rfh = document_.AsRenderFrameHostIfValid();
    facts.is_document = is_document_ && rfh && rfh->IsActive();
    if (rfh) {
      const url::Origin& top = TopLevelOrigin(rfh);
      facts.http_top_level = !top.opaque() &&
                             (top.scheme() == url::kHttpsScheme ||
                              top.scheme() == url::kHttpScheme);
    }
    return facts;
  }

  bool IsAllowedByPermissionsPolicy(WebAIApi api) override {
    content::RenderFrameHost* rfh = document_.AsRenderFrameHostIfValid();
    // Workers have no frame policy here (as upstream); they get
    // "unavailable" anyway.
    return !rfh || rfh->IsFeatureEnabled(PolicyFeature(api));
  }

  SiteConsent GetConsent() override {
    content::RenderFrameHost* rfh = document_.AsRenderFrameHostIfValid();
    if (!rfh) {
      return SiteConsent::kAsk;
    }
    WebAISitePermissions* answers =
        WebAISitePermissions::Get(browser_context_);
    return answers ? answers->GetConsent(TopLevelOrigin(rfh))
                   : SiteConsent::kAsk;
  }

  void ObserveConsent(base::RepeatingClosure on_change) override {
    // Any site's change: the manager reads its own answer again.
    if (WebAISitePermissions* answers =
            WebAISitePermissions::Get(browser_context_)) {
      consent_subscription_ = answers->AddObserver(std::move(on_change));
    }
  }

  void RequestConsent(bool needs_model,
                      base::OnceCallback<void(SiteConsent)> callback) override {
    content::RenderFrameHost* rfh = document_.AsRenderFrameHostIfValid();
    if (!rfh) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(callback), SiteConsent::kAsk));
      return;
    }
    ShowWebAIConsentPrompt(rfh, needs_model, std::move(callback));
  }

  std::string GetSiteKey() override {
    // Profile + top-level site. In memory only, as a map key.
    std::string key = base::NumberToString(
        reinterpret_cast<uintptr_t>(browser_context_.get()));
    key += '|';
    content::RenderFrameHost* rfh = document_.AsRenderFrameHostIfValid();
    if (rfh) {
      key += net::SchemefulSite(TopLevelOrigin(rfh)).Serialize();
    }
    return key;
  }

  bool IsForeground() override {
    content::RenderFrameHost* rfh = document_.AsRenderFrameHostIfValid();
    return rfh && rfh->GetVisibilityState() ==
                      content::PageVisibilityState::kVisible;
  }

 private:
  static const url::Origin& TopLevelOrigin(content::RenderFrameHost* rfh) {
    return rfh->GetOutermostMainFrame()->GetLastCommittedOrigin();
  }

  const raw_ptr<content::BrowserContext> browser_context_;
  const bool is_document_;
  const content::WeakDocumentPtr document_;
  base::CallbackListSubscription consent_subscription_;
};

}  // namespace

void BindMoltAIManager(
    content::BrowserContext* browser_context,
    base::SupportsUserData* context_user_data,
    content::RenderFrameHost* rfh,
    const void* user_data_key,
    mojo::PendingReceiver<blink::mojom::AIManager> receiver) {
  // Only MoltBrowser stores anything under this key now (upstream's
  // AIManager is never created), so the cast is exact.
  auto* manager =
      static_cast<MoltAIManager*>(context_user_data->GetUserData(user_data_key));
  if (!manager) {
    auto owned = std::make_unique<MoltAIManager>(
        std::make_unique<FrameEnvironment>(browser_context, rfh),
        RuntimeWebAIBackend::GetInstance());
    manager = owned.get();
    context_user_data->SetUserData(user_data_key, std::move(owned));
  }
  manager->AddReceiver(std::move(receiver));
}

}  // namespace molt_ai::web_ai
