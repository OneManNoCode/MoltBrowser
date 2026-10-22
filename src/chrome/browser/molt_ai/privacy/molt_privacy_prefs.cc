// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_privacy_prefs.h"

#include "base/values.h"
#include "chrome/common/pref_names.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/embedder_support/pref_names.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"

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
  // The Translator web API keeps upstream's default (TranslatorAPIAllowed
  // on): a page's Translator now runs on the user's own on-device model,
  // behind the same per-site answer as the other web AI APIs, and never
  // reaches TranslateKit or the component updater
  // (docs/design/WEB_AI_APIS.md, section 10). The policy still turns it off.
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
