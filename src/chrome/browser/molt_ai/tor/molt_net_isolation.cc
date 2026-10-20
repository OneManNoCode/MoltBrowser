// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_isolation.h"

#include <string>
#include <utility>

#include "base/base64.h"
#include "base/functional/callback.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/tor/molt_net_isolation_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/value_map_pref_store.h"
#include "components/security_interstitials/content/stateful_ssl_host_state_delegate.h"
#include "content/public/browser/browser_context.h"
#include "extensions/buildflags/buildflags.h"
#include "net/base/hash_value.h"
#include "net/base/net_errors.h"
#include "net/cert/x509_certificate.h"

#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"
#endif

namespace molt_ai {
namespace tor {

namespace {

// StatefulSSLHostStateDelegate keeps certificate-error exceptions for the
// default storage partition in the profile's HostContentSettingsMap
// (ContentSettingsType::SSL_CERT_DECISIONS, INHERIT_IN_INCOGNITO), so an
// off-the-record profile honours every "Proceed (unsafe)" the user clicked
// in the regular profile. For MoltNet that would skip the interstitial for
// a certificate the user accepted outside Tor, and lets anyone who once got
// the user past a warning for their certificate recognise the same user in
// every MoltNet session. This one keeps MoltNet's own exceptions in memory
// and never reads the map for them.
class MoltNetSSLHostStateDelegate : public StatefulSSLHostStateDelegate {
 public:
  using StatefulSSLHostStateDelegate::StatefulSSLHostStateDelegate;

  MoltNetSSLHostStateDelegate(const MoltNetSSLHostStateDelegate&) = delete;
  MoltNetSSLHostStateDelegate& operator=(const MoltNetSSLHostStateDelegate&) =
      delete;
  ~MoltNetSSLHostStateDelegate() override = default;

  // content::SSLHostStateDelegate:
  void AllowCert(const std::string& host,
                 const net::X509Certificate& cert,
                 net::Error error,
                 content::StoragePartition* storage_partition) override {
    cert_decisions_.Allow(host, DecisionKey(cert, error));
  }

  CertJudgment QueryPolicy(
      const std::string& host,
      const net::X509Certificate& cert,
      net::Error error,
      content::StoragePartition* storage_partition) override {
    return cert_decisions_.IsAllowed(host, DecisionKey(cert, error)) ? ALLOWED
                                                                     : DENIED;
  }

  bool HasAllowException(
      const std::string& host,
      content::StoragePartition* storage_partition) override {
    return cert_decisions_.HasAnyFor(host) ||
           HasHttpAllowException(host, storage_partition);
  }

  void RevokeUserAllowExceptions(const std::string& host) override {
    cert_decisions_.Revoke(host);
    StatefulSSLHostStateDelegate::RevokeUserAllowExceptions(host);
  }

  void Clear(
      base::RepeatingCallback<bool(const std::string&)> host_filter) override {
    if (host_filter.is_null()) {
      cert_decisions_.Clear();
    } else {
      cert_decisions_.RemoveIf(
          [&host_filter](const std::string& host) {
            return host_filter.Run(host);
          });
    }
    StatefulSSLHostStateDelegate::Clear(std::move(host_filter));
  }

 private:
  // The same certificate + error identity StatefulSSLHostStateDelegate uses.
  static std::string DecisionKey(const net::X509Certificate& cert,
                                 net::Error error) {
    return base::NumberToString(error) +
           base::Base64Encode(cert.CalculateChainFingerprint256());
  }

  CertDecisionMemory cert_decisions_;
};

}  // namespace

bool IsMoltNetContext(content::BrowserContext* context) {
  return context && IsMoltNetProfile(Profile::FromBrowserContext(context));
}

bool IsExtensionKeptOutOf(const extensions::Extension* extension,
                          content::BrowserContext* context) {
  if (!IsMoltNetContext(context)) {
    return false;
  }
#if BUILDFLAG(ENABLE_EXTENSIONS_CORE)
  const bool is_component =
      extension &&
      extensions::Manifest::IsComponentLocation(extension->location());
#else
  const bool is_component = false;
#endif
  return IsKeptOutOfMoltNet(
      ClassifyExtension(/*known=*/extension != nullptr, is_component));
}

scoped_refptr<PrefStore> CreateMoltNetExtensionPrefStore() {
  return base::MakeRefCounted<ValueMapPrefStore>();
}

std::unique_ptr<KeyedService> CreateMoltNetSSLHostStateDelegate(
    Profile* profile,
    HostContentSettingsMap* host_content_settings_map) {
  return std::make_unique<MoltNetSSLHostStateDelegate>(
      profile, host_content_settings_map);
}

}  // namespace tor
}  // namespace molt_ai
