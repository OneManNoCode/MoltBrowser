// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Who may open chrome:// pages, including through the molt:// alias (S19).
//
// molt:// is the user-facing name of the chrome:// WebUI: a BrowserURLHandler
// pair in chrome_content_browser_client.cc rewrites molt://<host> to
// chrome://molt-<host> (and molt://settings to chrome://settings).
//
// Chromium keeps web pages off chrome:// by refusing the renderer's request
// before any rewrite (ChildProcessSecurityPolicy::CanRequestURL, applied by
// RenderProcessHost::FilterURL). A molt:// URL passes that check: it is
// neither web-safe, granted, a WebUI scheme nor handled, so it is treated as
// an external-protocol URL any renderer may request. The rewrite then runs
// in NavigationControllerImpl::CreateNavigationRequestFromLoadParams for new
// noopener windows (a target=_blank link, window.open(url, '', 'noopener')),
// renderer OpenURL requests (e.g. middle-click) and navigations of another
// frame's main frame (opener / popup / top navigation across processes), and
// nothing checks the chrome:// result again. So any web page could open
// molt://ai-settings or molt://settings/... as the real WebUI.
//
// The rule, enforced after the rewrite: a navigation a renderer started may
// only go to a chrome:// (or molt://) URL if the document that started it is
// itself a chrome:// or devtools:// page. Browser-initiated navigations
// (omnibox, bookmarks, MoltBrowser's own UI), history navigations and
// reloads of an existing entry are not affected. Pure logic; the
// NavigationThrottle that applies it is webui_navigation_throttle.h.

#ifndef CHROME_BROWSER_MOLT_AI_SECURITY_WEBUI_NAVIGATION_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_SECURITY_WEBUI_NAVIGATION_POLICY_H_

#include <optional>

#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::security {

// True if this navigation must not proceed: it targets a chrome:// or
// molt:// URL, a renderer started it (not as a history navigation or a
// reload), and its initiator isn't a chrome:// or devtools:// document.
// A renderer-initiated navigation without an initiator origin is blocked.
bool ShouldBlockWebInitiatedWebUINavigation(
    const GURL& url,
    bool renderer_initiated,
    bool is_history_or_reload,
    const std::optional<url::Origin>& initiator_origin);

}  // namespace molt_ai::security

#endif  // CHROME_BROWSER_MOLT_AI_SECURITY_WEBUI_NAVIGATION_POLICY_H_
