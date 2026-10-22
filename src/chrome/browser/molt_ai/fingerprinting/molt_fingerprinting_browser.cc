// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/supports_user_data.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting.mojom.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "crypto/random.h"
#include "ipc/ipc_channel_proxy.h"
#include "mojo/public/cpp/bindings/associated_remote.h"

namespace molt_ai::fingerprinting {

namespace {

const char kSessionSeedKey[] = "MoltFingerprintingSessionSeed";

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
  // Shields decisions are per document and per worker, never per process
  // (molt_shields_delivery.h).
}

}  // namespace molt_ai::fingerprinting
