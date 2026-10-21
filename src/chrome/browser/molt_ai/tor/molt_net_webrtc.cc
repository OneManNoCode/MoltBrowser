// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_webrtc.h"

#include "base/command_line.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_request_binders.h"
#include "chrome/browser/molt_ai/tor/molt_net_requests_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_webrtc_binders.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/molt_ai/tor/molt_net_webrtc_policy.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"

namespace molt_ai::tor {

namespace {

bool IsMoltNetBrowserContext(content::BrowserContext* context) {
  return context && IsMoltNetProfile(Profile::FromBrowserContext(context));
}

}  // namespace

void RegisterMoltNetFrameBinders(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  if (!render_frame_host || !map) {
    return;
  }
  // The frame's process, as ChromeContentBrowserClient::
  // RegisterBrowserInterfaceBindersForFrame itself reads it there; a
  // process only ever hosts one BrowserContext.
  const bool is_moltnet = IsMoltNetBrowserContext(
      render_frame_host->GetProcess()->GetBrowserContext());
  ApplyWebRtcNetworkAccessToFrameBinders(
      WebRtcNetworkAccessForProfile(is_moltnet), map);
  // No image descriptions or location service either: both would send what
  // they get outside Tor (molt_net_request_binders.h).
  ApplyMoltNetRequestAccessToFrameBinders(
      MoltNetRequestAccessForProfile(is_moltnet), map);
}

void AppendMoltNetRendererSwitches(content::RenderProcessHost* host,
                                   base::CommandLine* command_line) {
  if (!host || !command_line) {
    return;
  }
  if (ShouldMarkMoltNetRenderer(
          IsMoltNetBrowserContext(host->GetBrowserContext()))) {
    command_line->AppendSwitch(switches::kMoltNetRenderer);
  }
}

}  // namespace molt_ai::tor
