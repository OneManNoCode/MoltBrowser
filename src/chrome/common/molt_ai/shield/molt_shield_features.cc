// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/shield/molt_shield_features.h"

#include "base/command_line.h"

namespace molt_shield {

// Feature names are the identifiers without the leading "k", e.g.
// "MoltShieldRequestProxy".
BASE_FEATURE(kMoltShieldRequestProxy, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kMoltShieldFrameBlocking, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kMoltShieldWebSocket, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kMoltShieldKeepAlive, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kMoltShieldCosmetics, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kMoltShieldConsentKiller, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE(kMoltShieldRemoveParam, base::FEATURE_ENABLED_BY_DEFAULT);

namespace {

struct Hook {
  const base::Feature* feature;
  const char* flag_name;
};

constexpr Hook kHooks[] = {
    {&kMoltShieldRequestProxy, flags::kRequestProxy},
    {&kMoltShieldFrameBlocking, flags::kFrameBlocking},
    {&kMoltShieldWebSocket, flags::kWebSocket},
    {&kMoltShieldKeepAlive, flags::kKeepAlive},
    {&kMoltShieldCosmetics, flags::kCosmetics},
    {&kMoltShieldConsentKiller, flags::kConsentKiller},
    {&kMoltShieldRemoveParam, flags::kRemoveParam},
};

bool Enabled(const base::Feature& feature) {
  return !IsMoltShieldDisabledByCommandLine() &&
         base::FeatureList::IsEnabled(feature);
}

}  // namespace

bool IsMoltShieldDisabledByCommandLine() {
  return base::CommandLine::InitializedForCurrentProcess() &&
         base::CommandLine::ForCurrentProcess()->HasSwitch(
             switches::kDisableMoltShield);
}

bool IsRequestProxyEnabled() {
  return Enabled(kMoltShieldRequestProxy);
}

bool IsFrameBlockingEnabled() {
  return Enabled(kMoltShieldFrameBlocking);
}

bool IsWebSocketEnabled() {
  return Enabled(kMoltShieldWebSocket);
}

bool IsKeepAliveEnabled() {
  return Enabled(kMoltShieldKeepAlive);
}

bool IsCosmeticsEnabled() {
  return Enabled(kMoltShieldCosmetics);
}

bool IsConsentKillerEnabled() {
  return Enabled(kMoltShieldConsentKiller);
}

bool IsRemoveParamEnabled() {
  return Enabled(kMoltShieldRemoveParam);
}

std::vector<std::string_view> FlagNamesOfDisabledHooks() {
  std::vector<std::string_view> names;
  for (const Hook& hook : kHooks) {
    if (!base::FeatureList::IsEnabled(*hook.feature)) {
      names.push_back(hook.flag_name);
    }
  }
  return names;
}

}  // namespace molt_shield
