// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the URL loader factory for a page's Background Fetch request
// that comes without its service worker's (molt_resumed_fetch_policy.h says
// which and why): one that checks Local Network Access for the fetch's
// origin. Header-only; DownloadManagerImpl::
// BeginResourceDownloadOnChecksComplete() uses it in every profile.

#ifndef CONTENT_BROWSER_DOWNLOAD_MOLT_RESUMED_FETCH_FACTORY_H_
#define CONTENT_BROWSER_DOWNLOAD_MOLT_RESUMED_FETCH_FACTORY_H_

#include <memory>
#include <utility>

#include "base/feature_list.h"
#include "components/download/public/common/download_source.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/browser/download/molt_resumed_fetch_policy.h"
#include "content/browser/renderer_host/local_network_access_util.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/child_process_host.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"
#include "services/network/public/cpp/originating_process_id.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/client_security_state.mojom.h"
#include "services/network/public/mojom/ip_address_space.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"
#include "url/origin.h"

namespace content {

// For `params`, a request that needs one (MoltFetchNeedsLnaFactory()), a
// factory of `storage_partition` with the client security state of a worker
// of its initiator served from a public address and the observer that asks
// the initiator's Local Network Access permission; else null (the caller
// makes Chromium's).
inline std::unique_ptr<network::PendingSharedURLLoaderFactory>
MoltLnaFactoryForFetch(StoragePartitionImpl* storage_partition,
                       download::DownloadUrlParameters& params) {
  if (!storage_partition || !params.initiator().has_value() ||
      !MoltFetchNeedsLnaFactory(
          params.download_source() == download::DownloadSource::INTERNAL_API,
          params.initiator().has_value(),
          /*has_factory=*/params.url_loader_factory() != nullptr,
          base::FeatureList::IsEnabled(
              features::kBackgroundFetchLocalNetworkAccess))) {
    return nullptr;
  }
  const url::Origin& initiator = *params.initiator();
  const bool secure = network::IsOriginPotentiallyTrustworthy(initiator);
  network::mojom::ClientSecurityStatePtr client_security_state =
      network::mojom::ClientSecurityState::New();
  client_security_state->is_web_secure_context = secure;
  client_security_state->ip_address_space =
      network::mojom::IPAddressSpace::kPublic;
  client_security_state->local_network_access_request_policy =
      OverrideLocalNetworkAccessPolicy(
          DeriveLocalNetworkAccessRequestPolicy(
              network::mojom::IPAddressSpace::kPublic, secure,
              LocalNetworkAccessRequestContext::kWorker),
          GetContentClient()
              ->browser()
              ->ShouldOverrideLocalNetworkAccessRequestPolicy(
                  storage_partition->browser_context(), initiator));

  network::mojom::URLLoaderFactoryParamsPtr factory_params =
      storage_partition->CreateURLLoaderFactoryParams();
  factory_params->client_security_state = std::move(client_security_state);
  if (mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver>
          observer = storage_partition
                         ->CreateURLLoaderNetworkObserverForServiceOrSharedWorker(
                             network::OriginatingProcessId::renderer(
                                 network::RendererProcessId(
                                     ChildProcessHost::kInvalidUniqueID)),
                             initiator)) {
    factory_params->url_loader_network_observer = std::move(observer);
  }
  mojo::PendingRemote<network::mojom::URLLoaderFactory> factory;
  storage_partition->GetNetworkContext()->CreateURLLoaderFactory(
      factory.InitWithNewPipeAndPassReceiver(), std::move(factory_params));
  return std::make_unique<network::WrapperPendingSharedURLLoaderFactory>(
      std::move(factory));
}

}  // namespace content

#endif  // CONTENT_BROWSER_DOWNLOAD_MOLT_RESUMED_FETCH_FACTORY_H_
