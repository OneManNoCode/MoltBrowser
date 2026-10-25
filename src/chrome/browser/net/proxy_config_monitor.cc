// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/net/proxy_config_monitor.h"

#include <optional>  // MoltBrowser
#include <utility>

#include "base/functional/bind.h"  // MoltBrowser
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"  // MoltBrowser
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"  // MoltBrowser
#include "chrome/browser/net/proxy_service_factory.h"
#include "chrome/browser/policy/profile_policy_connector.h"
#include "chrome/browser/profiles/profile.h"
#include "components/enterprise/buildflags/buildflags.h"
#include "components/proxy_config/pref_proxy_config_tracker_impl.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/buildflags/buildflags.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"  // MoltBrowser
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/network_context.mojom.h"

#if BUILDFLAG(ENTERPRISE_PROXY)
#include "chrome/browser/enterprise/net/enterprise_proxy_service_factory.h"
#endif  // BUILDFLAG(ENTERPRISE_PROXY)

#if BUILDFLAG(IS_CHROMEOS)
#include "chrome/browser/ash/profiles/profile_helper.h"
#endif  // BUILDFLAG(IS_CHROMEOS)

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
#include "extensions/browser/api/proxy/proxy_api.h"
#endif // BUILDFLAG(ENABLE_EXTENSIONS_CORE)

using content::BrowserThread;

ProxyConfigMonitor::ProxyConfigMonitor(Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile);

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  profile_ = profile;
#endif  // BUILDFLAG(ENABLE_EXTENSIONS_CORE)

  // MoltBrowser: a MoltNet profile's network contexts only ever get
  // MoltNet's own Tor proxy configuration, never a policy's proxy or
  // override rules, and from the moment the identity ends, one that refuses
  // every request (molt_net_live_policy.h), with which each context ends
  // (services/network/molt_ended_identity_policy.h).
  if (molt_ai::tor::IsMoltNetProfile(profile)) {
    moltnet_profile_ = profile;
    moltnet_identity_ended_ =
        molt_ai::tor::MoltNetRouting::AddIdentityEndedCallback(
            base::BindRepeating(
                [](ProxyConfigMonitor* monitor, Profile* ended,
                   const molt_ai::tor::MoltNetRouting::EndAckHolder& hold) {
                  if (ended != monitor->moltnet_profile_) {
                    return;
                  }
                  for (const auto& client :
                       monitor->proxy_config_client_set_) {
                    client->OnProxyConfigUpdated(
                        molt_ai::tor::MoltNetRouting::RefuseAllProxyConfig());
                    // On the same pipe, so answered once the context has
                    // applied the refusal and ended with it; a context that
                    // goes away answers too (it is gone).
                    client->FlushProxyConfig(
                        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                            hold.Run()));
                  }
                },
                base::Unretained(this)));
  }

// If this is the ChromeOS sign-in or lock screen profile, just create the
// tracker from global state.
#if BUILDFLAG(IS_CHROMEOS)
  if (ash::ProfileHelper::IsSigninProfile(profile) ||
      ash::ProfileHelper::IsLockScreenProfile(profile)) {
    pref_proxy_config_tracker_ =
        ProxyServiceFactory::CreatePrefProxyConfigTrackerOfLocalState(
            g_browser_process->local_state());
  }
#endif  // BUILDFLAG(IS_CHROMEOS)

  if (!pref_proxy_config_tracker_) {
    enterprise_net::EnterpriseProxyService* enterprise_proxy_service = nullptr;
#if BUILDFLAG(ENTERPRISE_PROXY)
    enterprise_proxy_service =
        EnterpriseProxyServiceFactory::GetForProfile(profile);
#endif  // BUILDFLAG(ENTERPRISE_PROXY)
    pref_proxy_config_tracker_ =
        ProxyServiceFactory::CreatePrefProxyConfigTrackerOfProfile(
            profile->GetPrefs(), g_browser_process->local_state(),
            profile->GetProfilePolicyConnector()->policy_service(),
            enterprise_proxy_service);
  }

  proxy_config_service_ = ProxyServiceFactory::CreateProxyConfigService(
      pref_proxy_config_tracker_.get(), profile);

  proxy_config_service_->AddObserver(this);
}

ProxyConfigMonitor::ProxyConfigMonitor(PrefService* local_state) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
         !BrowserThread::IsThreadInitialized(BrowserThread::UI));

  pref_proxy_config_tracker_ =
      ProxyServiceFactory::CreatePrefProxyConfigTrackerOfLocalState(
          local_state);

  proxy_config_service_ = ProxyServiceFactory::CreateProxyConfigService(
      pref_proxy_config_tracker_.get(), nullptr);
  proxy_config_service_->AddObserver(this);
}

ProxyConfigMonitor::~ProxyConfigMonitor() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
         !BrowserThread::IsThreadInitialized(BrowserThread::UI));
  proxy_config_service_->RemoveObserver(this);
  pref_proxy_config_tracker_->DetachFromPrefService();
}

void ProxyConfigMonitor::AddToNetworkContextParams(
    network::mojom::NetworkContextParams* network_context_params) {
  mojo::PendingRemote<network::mojom::ProxyConfigClient> proxy_config_client;
  network_context_params->proxy_config_client_receiver =
      proxy_config_client.InitWithNewPipeAndPassReceiver();
  proxy_config_client_set_.Add(std::move(proxy_config_client));

  if (proxy_config_service_->UsesPolling()) {
    poller_receiver_set_.Add(this,
                             network_context_params->proxy_config_poller_client
                                 .InitWithNewPipeAndPassReceiver());
  }

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  error_receiver_set_.Add(this, network_context_params->proxy_error_client
                                    .InitWithNewPipeAndPassReceiver());
#endif  // BUILDFLAG(ENABLE_EXTENSIONS_CORE)

  net::ProxyConfigWithAnnotation proxy_config;
  net::ProxyConfigService::ConfigAvailability availability =
      proxy_config_service_->GetLatestProxyConfig(&proxy_config);
  // MoltBrowser: a MoltNet network context starts with MoltNet's own
  // configuration, with none (its requests wait for one), or, once the
  // identity has ended, with the one that refuses every request.
  if (moltnet_profile_) {
    if (std::optional<net::ProxyConfigWithAnnotation> moltnet_config =
            molt_ai::tor::MoltNetRouting::ProxyConfigFor(
                moltnet_profile_, proxy_config,
                availability == net::ProxyConfigService::CONFIG_VALID)) {
      network_context_params->initial_proxy_config = *moltnet_config;
    }
    return;
  }
  if (availability != net::ProxyConfigService::CONFIG_PENDING)
    network_context_params->initial_proxy_config = proxy_config;
}

void ProxyConfigMonitor::FlushForTesting() {
  proxy_config_client_set_.FlushForTesting();
}

void ProxyConfigMonitor::OnProxyConfigChanged(
    const net::ProxyConfigWithAnnotation& config,
    net::ProxyConfigService::ConfigAvailability availability) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
         !BrowserThread::IsThreadInitialized(BrowserThread::UI));
  // MoltBrowser: a MoltNet network context keeps MoltNet's own proxy
  // configuration; any other one (a policy's proxy, override rules, the
  // system's settings, none) is held back, and MoltNet ends
  // (molt_net_live_guard.h). An ended identity's keeps refusing every
  // request.
  if (moltnet_profile_) {
    if (std::optional<net::ProxyConfigWithAnnotation> moltnet_config =
            molt_ai::tor::MoltNetRouting::ProxyConfigFor(
                moltnet_profile_, config,
                availability == net::ProxyConfigService::CONFIG_VALID)) {
      for (const auto& proxy_config_client : proxy_config_client_set_) {
        proxy_config_client->OnProxyConfigUpdated(*moltnet_config);
      }
    }
    return;
  }
  for (const auto& proxy_config_client : proxy_config_client_set_) {
    switch (availability) {
      case net::ProxyConfigService::CONFIG_VALID:
        proxy_config_client->OnProxyConfigUpdated(config);
        break;
      case net::ProxyConfigService::CONFIG_UNSET:
        proxy_config_client->OnProxyConfigUpdated(
            net::ProxyConfigWithAnnotation::CreateDirect());
        break;
      case net::ProxyConfigService::CONFIG_PENDING:
        NOTREACHED();
    }
  }
}

void ProxyConfigMonitor::OnLazyProxyConfigPoll() {
  proxy_config_service_->OnLazyPoll();
}

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
void ProxyConfigMonitor::OnPACScriptError(int32_t line_number,
                                          const std::string& details) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  extensions::ProxyEventRouter::GetInstance()->OnPACScriptError(
      profile_, line_number, base::UTF8ToUTF16(details));
}

void ProxyConfigMonitor::OnRequestMaybeFailedDueToProxySettings(
    int32_t net_error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI) ||
         !BrowserThread::IsThreadInitialized(BrowserThread::UI));

  if (net_error >= 0) {
    // If the error is obviously wrong, don't dispatch it to extensions. If the
    // PAC executor process is compromised, then |net_error| could be attacker
    // controlled.
    return;
  }

  extensions::ProxyEventRouter::GetInstance()->OnProxyError(profile_,
                                                            net_error);
}
#endif  // BUILDFLAG(ENABLE_EXTENSIONS_CORE)
