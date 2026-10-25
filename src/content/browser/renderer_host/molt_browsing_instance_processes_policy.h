// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: in a profile that keeps BrowsingInstances in processes of
// their own (ContentBrowserClient::ShouldKeepBrowsingInstancesInOwnProcesses():
// MoltNet's web content), a renderer process serves one BrowsingInstance.
// Std-only, so molt_ai_unittests tests it; molt_browsing_instance_processes.h
// applies it in RenderProcessHostImpl::IsSuitableHost(), which every way of
// reusing a process goes through (an iframe or a worker taking a process of
// its site, a navigation taking a service worker's process, an empty
// process, the process limit, process-per-site).
//
// Chromium 156 lets frames and workers of one site share a renderer
// process across BrowsingInstances: an iframe reuses a process its site has
// in another tab (ProcessReusePolicy::kReusePendingOrCommittedSiteSubframe),
// service and shared workers the same, and any suitable process is reused
// once the process limit is reached. A third party framed by many sites
// then runs in one process for all of them, and whatever is counted per
// renderer process is shared by those sites: the blob memory the browser
// counts to the process's BlobRegistry (MoltNet's blob budget,
// storage/browser/blob/molt_blob_budget_policy.h), the renderer's heap and
// caches. A BrowsingInstance holds one tab and the windows it opened with an
// opener, which can reach each other's same-origin frames anyway; with a
// process each, nothing one tab's pages do there can be seen from another
// tab's. Tor Browser's processes are kept apart by first-party site in the
// same way (Firefox's origin attributes in its process keys).
//
// A process keeps the BrowsingInstances it served until Chromium's delayed
// cleanup of their ids (ChildProcessSecurityPolicyImpl), so a process isn't
// handed to another BrowsingInstance right after its pages closed either.
// The cost is more renderer processes: one per site per tab, where Chromium
// would share a site's process between tabs.

#ifndef CONTENT_BROWSER_RENDERER_HOST_MOLT_BROWSING_INSTANCE_PROCESSES_POLICY_H_
#define CONTENT_BROWSER_RENDERER_HOST_MOLT_BROWSING_INSTANCE_PROCESSES_POLICY_H_

namespace content {

// Whether a process may serve a BrowsingInstance: `keep_apart`, the profile
// keeps BrowsingInstances apart for the destination; `hosts_other`, the
// process serves (or recently served) another BrowsingInstance.
constexpr bool MoltMayServeBrowsingInstance(bool keep_apart,
                                            bool hosts_other) {
  return !keep_apart || !hosts_other;
}

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_MOLT_BROWSING_INSTANCE_PROCESSES_POLICY_H_
