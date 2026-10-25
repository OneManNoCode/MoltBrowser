// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which of MoltNet's renderer processes serve one BrowsingInstance each
// (ChromeContentBrowserClient::ShouldKeepBrowsingInstancesInOwnProcesses(),
// content/browser/renderer_host/molt_browsing_instance_processes_policy.h):
// every one for a web page, a frame or a worker of a MoltNet profile, so
// that nothing counted per renderer process (MoltNet's blob budget among
// it) is shared by two tabs. The browser's own pages (chrome://,
// chrome-untrusted://, devtools://) and MoltBrowser's component extensions
// (the PDF viewer, chrome-extension://) keep Chromium's process sharing:
// they run no page script, and some of them rely on one process per site.
// Other profiles keep Chromium's process model. Header-only and std-only,
// so molt_ai_unittests tests it.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROCESS_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROCESS_POLICY_H_

#include <string_view>

namespace molt_ai {
namespace tor {

// `site_scheme`: the scheme of the destination's site URL (lowercase, as
// GURL keeps it).
constexpr bool KeepBrowsingInstancesApart(bool is_moltnet_profile,
                                          std::string_view site_scheme) {
  if (!is_moltnet_profile) {
    return false;
  }
  for (std::string_view browser_ui :
       {"chrome", "chrome-untrusted", "devtools", "chrome-extension"}) {
    if (site_scheme == browser_ui) {
      return false;
    }
  }
  return true;
}

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PROCESS_POLICY_H_
