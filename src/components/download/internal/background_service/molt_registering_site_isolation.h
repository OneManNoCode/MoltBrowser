// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a Background Fetch request that the in-memory download
// service makes (the service off-the-record profiles use) carries the
// network isolation of the site that started the fetch, as the same request
// does in a regular profile (DownloadDriverImpl and
// download::CreateResourceRequest() put it in the request's trusted
// parameters).
//
// Content gives each fetch that isolation: the service worker's own
// (StorageKey::ToPartialNetIsolationInfo(), what its fetches use) or the
// document's (RenderFrameHost::GetIsolationInfoForSubresources()). Chromium
// 156's in-memory download takes only its site for cookies, and sends the
// request to a trusted URL loader factory that gives each request without
// an isolation of its own one made from the request's URL
// (URLLoaderFactoryParams::automatically_assign_isolation_info;
// url_loader_util::GetIsolationInfo()). The request was then filed under
// its destination: two sites' fetches to one server shared its HTTP cache
// partition, connections and partitioned cookies, and in MoltNet its Tor
// circuit (the SOCKS username is the top-level site). With the isolation
// in the request's trusted parameters, the network service uses it (these
// factories have no isolation of their own, which would come first), so
// the request has the network key of the site's own requests.
//
// It applies to every off-the-record profile: the in-memory service exists
// only for them, and there Background Fetch is its only client (other
// clients pass no isolation and are unchanged); both factories it uses are
// trusted (StoragePartitionImpl::CreateURLLoaderFactoryParams(), the
// profile's browser-process factory), as for the regular path. Regular
// profiles don't use this code.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_REGISTERING_SITE_ISOLATION_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_REGISTERING_SITE_ISOLATION_H_

#include <optional>

#include "net/base/isolation_info.h"
#include "services/network/public/cpp/resource_request.h"

namespace download {

// Puts `isolation_info` (the site's that started the fetch, from
// RequestParams) into `request`'s trusted parameters, with its site for
// cookies. Leaves `request` alone without one.
inline void MoltCarryRegisteringSiteIsolation(
    const std::optional<net::IsolationInfo>& isolation_info,
    network::ResourceRequest& request) {
  if (!isolation_info || isolation_info->IsEmpty()) {
    return;
  }
  if (!request.trusted_params) {
    request.trusted_params.emplace();
  }
  request.trusted_params->isolation_info = *isolation_info;
  request.site_for_cookies = isolation_info->site_for_cookies();
}

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_REGISTERING_SITE_ISOLATION_H_
