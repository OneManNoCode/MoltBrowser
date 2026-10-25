// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: applies molt_browsing_instance_processes_policy.h in
// RenderProcessHostImpl::IsSuitableHost(). Header-only and in no GN target's
// `sources` (like content/browser/download/molt_download_isolation.h).

#ifndef CONTENT_BROWSER_RENDERER_HOST_MOLT_BROWSING_INSTANCE_PROCESSES_H_
#define CONTENT_BROWSER_RENDERER_HOST_MOLT_BROWSING_INSTANCE_PROCESSES_H_

#include "content/browser/isolation_context.h"
#include "content/browser/renderer_host/molt_browsing_instance_processes_policy.h"
#include "content/browser/security/cpsp/child_process_security_policy_impl.h"
#include "content/browser/site_info.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/content_client.h"

namespace content {

// Whether `host` may serve `site_info` in the BrowsingInstance of
// `isolation_context`.
inline bool MoltMayServeBrowsingInstance(
    RenderProcessHost* host,
    const IsolationContext& isolation_context,
    const SiteInfo& site_info) {
  ContentBrowserClient* browser_client = GetContentClient()->browser();
  const bool keep_apart =
      browser_client &&
      browser_client->ShouldKeepBrowsingInstancesInOwnProcesses(
          isolation_context.browser_context(), site_info.site_url());
  return MoltMayServeBrowsingInstance(
      keep_apart,
      keep_apart &&
          ChildProcessSecurityPolicyImpl::GetInstance()
              ->MoltHostsOtherBrowsingInstance(
                  host->GetID(), isolation_context.browsing_instance_id()));
}

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_MOLT_BROWSING_INSTANCE_PROCESSES_H_
