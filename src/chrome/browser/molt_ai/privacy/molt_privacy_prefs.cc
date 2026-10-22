// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_privacy_prefs.h"

#include "base/values.h"
#include "chrome/common/pref_names.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/embedder_support/pref_names.h"
#include "components/on_device_translation/buildflags/buildflags.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"

#if BUILDFLAG(ENABLE_ON_DEVICE_TRANSLATION)
#include "components/on_device_translation/public/pref_names.h"
#endif

namespace molt_ai {
namespace privacy {

namespace {

// blink::kWebRTCIPHandlingDefaultPublicInterfaceOnly (stable, web-exposed
// policy value; inlined to avoid a blink/common dependency).
constexpr char kWebRtcDefaultPublicInterfaceOnly[] =
    "default_public_interface_only";

// prefetch::NetworkPredictionOptions::kDisabled.
constexpr int kNetworkPredictionDisabled = 2;

}  // namespace

void SetPrivacyPrefDefaults(user_prefs::PrefRegistrySyncable* registry) {
  registry->SetDefaultPrefValue(
      prefs::kCookieControlsMode,
      base::Value(static_cast<int>(
          content_settings::CookieControlsMode::kBlockThirdParty)));
  registry->SetDefaultPrefValue(prefs::kWebRTCIPHandlingPolicy,
                                base::Value(kWebRtcDefaultPublicInterfaceOnly));
  registry->SetDefaultPrefValue(prefs::kNetworkPredictionOptions,
                                base::Value(kNetworkPredictionDisabled));
  registry->SetDefaultPrefValue(prefs::kEnableDoNotTrack, base::Value(false));
  // Registered by ProfileNetworkContextService::RegisterProfilePrefs().
  registry->SetDefaultPrefValue(embedder_support::kAlternateErrorPagesEnabled,
                                base::Value(false));
  // Registered by password_manager::PasswordManager::RegisterProfilePrefs().
  registry->SetDefaultPrefValue(
      password_manager::prefs::kPasswordLeakDetectionEnabled,
      base::Value(false));
#if BUILDFLAG(ENABLE_ON_DEVICE_TRANSLATION)
  // The Translator web API (the TranslatorAPIAllowed policy's pref,
  // registered by on_device_translation::RegisterProfilePrefs()). Upstream's
  // Translator has the component updater fetch Google's TranslateKit and
  // language packs once a page asks after a click, outside Tor even for a
  // MoltNet page. Off, it reports "unavailable" and create() fails before
  // any download (docs/design/WEB_AI_APIS.md). The policy can turn it on.
  // Incognito and MoltNet profiles fork this registry, so they get the same
  // default.
  registry->SetDefaultPrefValue(prefs::kTranslatorAPIAllowed,
                                base::Value(false));
#endif
}

void ApplyPrivacyPrefMigrations(PrefService* prefs) {
  if (!prefs) {
    return;
  }
  // HTTPS-First balanced mode: warn before insecure public sites. Only when
  // the user (or Chrome's own heuristic) never touched any HTTPS-First pref,
  // mirroring HttpsFirstModeService's "typically secure" auto-enable. Once
  // set, the user value makes this a no-op; turning it off in Settings
  // writes user values too, so it is never forced back on.
  if (!prefs->HasPrefPath(prefs::kHttpsOnlyModeEnabled) &&
      !prefs->HasPrefPath(prefs::kHttpsFirstBalancedMode) &&
      !prefs->HasPrefPath(prefs::kHttpsOnlyModeAutoEnabled)) {
    prefs->SetBoolean(prefs::kHttpsFirstBalancedMode, true);
  }
}

}  // namespace privacy
}  // namespace molt_ai
