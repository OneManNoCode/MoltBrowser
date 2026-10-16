// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/scoped_observation.h"
#include "base/supports_user_data.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/browser/content_settings_type_set.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "crypto/random.h"
#include "ipc/ipc_channel_proxy.h"
#include "mojo/public/cpp/bindings/associated_remote.h"

namespace molt_ai::fingerprinting {

namespace {

const char kSessionSeedKey[] = "MoltFingerprintingSessionSeed";
const char kUnfarbledSitesWatcherKey[] =
    "MoltFingerprintingUnfarbledSitesWatcher";

class SessionSeedData : public base::SupportsUserData::Data {
 public:
  SessionSeedData() { crypto::RandBytes(seed_); }
  SessionSeedData(const SessionSeedData&) = delete;
  SessionSeedData& operator=(const SessionSeedData&) = delete;
  ~SessionSeedData() override = default;

  const SessionSeed& seed() const { return seed_; }

 private:
  SessionSeed seed_;
};

// A fresh endpoint on `host`'s channel. Like chrome::mojom::
// RendererConfiguration (RendererUpdater), every message rides the channel
// itself, so it is ordered with everything else sent to the renderer.
mojo::AssociatedRemote<mojom::FingerprintingConfiguration> ConfigurationFor(
    content::RenderProcessHost* host) {
  mojo::AssociatedRemote<mojom::FingerprintingConfiguration> configuration;
  if (IPC::ChannelProxy* channel = host->GetChannel()) {
    channel->GetRemoteAssociatedInterface(&configuration);
  }
  return configuration;
}

// Re-sends the Shields-off sites to every renderer of one farbling
// BrowserContext whenever the user changes them (MoltShield's per-site
// switch, chrome://settings/content/ads, ...). Owned by the context.
class UnfarbledSitesWatcher : public base::SupportsUserData::Data,
                              public content_settings::Observer {
 public:
  UnfarbledSitesWatcher(content::BrowserContext* context,
                        scoped_refptr<HostContentSettingsMap> map)
      : context_(context),
        map_(std::move(map)),
        sites_(shield::GetShieldsDownTopLevelSites(context)) {
    observation_.Observe(map_.get());
  }
  UnfarbledSitesWatcher(const UnfarbledSitesWatcher&) = delete;
  UnfarbledSitesWatcher& operator=(const UnfarbledSitesWatcher&) = delete;
  ~UnfarbledSitesWatcher() override = default;

  // content_settings::Observer:
  void OnContentSettingChanged(
      const ContentSettingsPattern& primary_pattern,
      const ContentSettingsPattern& secondary_pattern,
      ContentSettingsTypeSet content_type_set) override {
    if (!content_type_set.Contains(ContentSettingsType::ADS)) {
      return;
    }
    std::vector<std::string> sites =
        shield::GetShieldsDownTopLevelSites(context_);
    if (sites == sites_) {
      return;
    }
    sites_ = std::move(sites);
    for (content::RenderProcessHost::iterator it(
             content::RenderProcessHost::AllHostsIterator());
         !it.IsAtEnd(); it.Advance()) {
      content::RenderProcessHost* host = it.GetCurrentValue();
      if (host->GetBrowserContext() != context_) {
        continue;
      }
      // Hosts that are still launching get it too: their launch-time list
      // may already be stale. A dead host has no channel.
      if (mojo::AssociatedRemote<mojom::FingerprintingConfiguration>
              configuration = ConfigurationFor(host)) {
        configuration->SetUnfarbledSites(sites_);
      }
    }
  }

 private:
  const raw_ptr<content::BrowserContext> context_;
  // Kept alive (past keyed-service shutdown) until the observation below is
  // removed.
  const scoped_refptr<HostContentSettingsMap> map_;
  std::vector<std::string> sites_;
  base::ScopedObservation<HostContentSettingsMap, content_settings::Observer>
      observation_{this};
};

void EnsureWatchingUnfarbledSites(content::BrowserContext* context) {
  if (context->GetUserData(kUnfarbledSitesWatcherKey)) {
    return;
  }
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(context);
  if (!map) {
    return;
  }
  context->SetUserData(kUnfarbledSitesWatcherKey,
                       std::make_unique<UnfarbledSitesWatcher>(
                           context, base::WrapRefCounted(map)));
}

}  // namespace

const SessionSeed& GetSessionSeed(content::BrowserContext* context) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto* data =
      static_cast<SessionSeedData*>(context->GetUserData(kSessionSeedKey));
  if (!data) {
    auto owned = std::make_unique<SessionSeedData>();
    data = owned.get();
    context->SetUserData(kSessionSeedKey, std::move(owned));
  }
  return data->seed();
}

RendererMode ModeForBrowserContext(content::BrowserContext* context) {
  if (!context || !IsFingerprintingProtectionEnabled()) {
    return RendererMode::kOff;
  }
  return tor::IsMoltNetProfile(Profile::FromBrowserContext(context))
             ? RendererMode::kUniform
             : RendererMode::kFarble;
}

std::vector<std::string> UnfarbledSitesFor(content::BrowserContext* context) {
  if (ModeForBrowserContext(context) != RendererMode::kFarble) {
    // MoltNet keeps farbling where Shields are off.
    return {};
  }
  return shield::GetShieldsDownTopLevelSites(context);
}

void AppendRendererSwitches(content::RenderProcessHost* host,
                            base::CommandLine* command_line) {
  if (!host || !command_line) {
    return;
  }
  const RendererMode mode = ModeForBrowserContext(host->GetBrowserContext());
  if (mode == RendererMode::kOff) {
    return;
  }
  // Only the mode: the seed goes over the channel (InitializeRenderer).
  command_line->AppendSwitchASCII(switches::kMode, ModeToSwitchValue(mode));
}

void InitializeRenderer(content::RenderProcessHost* host) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!host) {
    return;
  }
  content::BrowserContext* context = host->GetBrowserContext();
  const RendererMode mode = ModeForBrowserContext(context);
  if (mode == RendererMode::kOff) {
    return;
  }
  mojo::AssociatedRemote<mojom::FingerprintingConfiguration> configuration =
      ConfigurationFor(host);
  if (!configuration) {
    return;
  }
  const SessionSeed& seed = GetSessionSeed(context);
  configuration->SetSessionSeed(std::vector<uint8_t>(seed.begin(), seed.end()));
  if (mode == RendererMode::kFarble) {
    EnsureWatchingUnfarbledSites(context);
    configuration->SetUnfarbledSites(UnfarbledSitesFor(context));
  }
}

}  // namespace molt_ai::fingerprinting
