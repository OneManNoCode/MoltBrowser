// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_autofill.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_requests_policy.h"
#include "chrome/browser/profiles/profile.h"
#include "components/autofill/core/browser/crowdsourcing/autofill_crowdsourcing_manager.h"
#include "components/autofill/core/browser/data_quality/addresses/address_normalizer_impl.h"
#include "components/autofill/core/browser/geo/region_data_loader_impl.h"
#include "components/autofill/core/browser/proto/server.pb.h"
#include "components/autofill/core/common/form_data.h"
#include "components/autofill/core/common/mojom/autofill_types.mojom-shared.h"
#include "content/public/browser/browser_context.h"
#include "net/base/isolation_info.h"
#include "third_party/libaddressinput/src/cpp/include/libaddressinput/null_storage.h"
#include "third_party/libaddressinput/src/cpp/include/libaddressinput/source.h"

namespace molt_ai::tor {

namespace {

// What molt_net_requests_policy.h allows a profile's tabs.
MoltNetRequestAccess AccessFor(content::BrowserContext* context) {
  return MoltNetRequestAccessForProfile(
      context && IsMoltNetProfile(Profile::FromBrowserContext(context)));
}

// An AutofillCrowdsourcingManager that never contacts the Autofill server.
// Its two entry points are the only ways anything is sent (queries from
// AutofillManager::QueryServerPredictions(), Autofill's votes from
// VotesUploader, the password manager's from its VotesUploader).
class NoServerCrowdsourcingManager final
    : public autofill::AutofillCrowdsourcingManager {
 public:
  using autofill::AutofillCrowdsourcingManager::AutofillCrowdsourcingManager;

  bool StartQueryRequest(
      const std::vector<autofill::FormData>& forms,
      std::optional<net::IsolationInfo> isolation_info,
      base::OnceCallback<void(std::optional<QueryResponse>)> callback)
      override {
    // Exactly what Chromium does when server communication is off: no
    // predictions, reported at once; the forms keep their local ones.
    std::move(callback).Run(std::nullopt);
    return false;
  }

  bool StartUploadRequest(
      std::vector<autofill::AutofillUploadContents> upload_contents,
      autofill::mojom::SubmissionSource form_submission_source,
      bool is_password_manager_upload) override {
    return false;
  }
};

// A source of address metadata that has none: every request fails at
// once, as NullStorage's do (libaddressinput then reports the rules as not
// loaded).
class NoAddressMetadataSource final : public ::i18n::addressinput::Source {
 public:
  void Get(const std::string& key, const Callback& data_ready) const override {
    data_ready(false, key, std::nullopt);
  }
};

}  // namespace

std::unique_ptr<autofill::AutofillCrowdsourcingManager>
MoltNetCrowdsourcingManagerFor(content::BrowserContext* context,
                               autofill::AutofillClient* client,
                               version_info::Channel channel) {
  if (!context || AccessFor(context).autofill_server) {
    return nullptr;
  }
  return std::make_unique<NoServerCrowdsourcingManager>(client, channel);
}

autofill::AddressNormalizer* MoltNetAddressNormalizerFor(
    content::BrowserContext* context) {
  if (context && AccessFor(context).address_metadata) {
    return nullptr;
  }
  // One for the whole browser, like Chromium's (AddressNormalizerFactory),
  // but it never has rules, so it is the same for every MoltNet identity.
  static base::NoDestructor<autofill::AddressNormalizerImpl> normalizer(
      std::make_unique<NoAddressMetadataSource>(),
      std::make_unique<::i18n::addressinput::NullStorage>(),
      g_browser_process->GetApplicationLocale());
  return normalizer.get();
}

autofill::RegionDataLoader* MoltNetRegionDataLoaderFor(
    content::BrowserContext* context,
    const std::string& app_locale) {
  if (context && AccessFor(context).address_metadata) {
    return nullptr;
  }
  // RegionDataLoaderImpl takes ownership of both.
  return new autofill::RegionDataLoaderImpl(
      new NoAddressMetadataSource(), new ::i18n::addressinput::NullStorage(),
      app_locale);
}

}  // namespace molt_ai::tor
