// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_fingerprinting_browser.h"

#include <memory>

#include "base/command_line.h"
#include "base/supports_user_data.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "crypto/random.h"

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
  content::BrowserContext* context = host->GetBrowserContext();
  const RendererMode mode = ModeForBrowserContext(context);
  if (mode == RendererMode::kOff) {
    return;
  }
  command_line->AppendSwitchASCII(switches::kMode, ModeToSwitchValue(mode));
  command_line->AppendSwitchASCII(switches::kSeed,
                                  EncodeSeed(GetSessionSeed(context)));
}

}  // namespace molt_ai::fingerprinting
