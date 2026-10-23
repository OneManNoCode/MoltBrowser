// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/network/browser_online_state_observer.h"

#include "content/browser/network/molt_uniform_network_state.h"  // MoltBrowser
#include "content/common/renderer.mojom.h"
#include "content/public/browser/render_process_host.h"

namespace content {

BrowserOnlineStateObserver::BrowserOnlineStateObserver() {
  net::NetworkChangeNotifier::AddMaxBandwidthObserver(this);
}

BrowserOnlineStateObserver::~BrowserOnlineStateObserver() {
  net::NetworkChangeNotifier::RemoveMaxBandwidthObserver(this);
}

void BrowserOnlineStateObserver::OnMaxBandwidthChanged(
    double max_bandwidth_mbps,
    net::NetworkChangeNotifier::ConnectionType type) {
  for (RenderProcessHost::iterator it(RenderProcessHost::AllHostsIterator());
       !it.IsAtEnd(); it.Advance()) {
    // TODO(crbug.com/40563310): Remove this check once we have a better
    // way of iterating the hosts.
    if (it.GetCurrentValue()->IsInitializedAndNotDead()) {
      // MoltBrowser: uniform values for MoltNet's renderers.
      MoltNotifyNetworkConnectionChanged(it.GetCurrentValue(), type,
                                         max_bandwidth_mbps);
    }
  }
}

void BrowserOnlineStateObserver::OnRenderProcessHostCreated(
    content::RenderProcessHost* rph) {
  double max_bandwidth_mbps;
  net::NetworkChangeNotifier::ConnectionType connection_type;
  net::NetworkChangeNotifier::GetMaxBandwidthAndConnectionType(
      &max_bandwidth_mbps, &connection_type);
  // MoltBrowser: uniform values for MoltNet's renderers.
  MoltNotifyNetworkConnectionChanged(rph, connection_type, max_bandwidth_mbps);
}

}  // namespace content
