// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The pure part of MoltNet's isolation from the regular profile. Standard
// library only, so molt_ai_unittests (and a standalone harness) test it
// without a Profile; molt_net_isolation.h applies it to Chromium's types.
//
//   * Which extensions may run in MoltNet windows: MoltBrowser's own
//     component extensions (the PDF viewer). Nothing the user or an
//     administrator installed, whatever its "Allow in Incognito" switch or
//     its split/spanning incognito mode says.
//   * The in-memory store for the certificate-error exceptions MoltNet makes
//     ("Proceed to example.com (unsafe)"). MoltNet keeps its own instead of
//     the regular profile's, which an off-the-record profile would inherit.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ISOLATION_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ISOLATION_POLICY_H_

#include <map>
#include <set>
#include <string>

namespace molt_ai {
namespace tor {

// What MoltNet needs to know about an extension.
enum class ExtensionOrigin {
  // Only an ID is known: the extension isn't installed or isn't loaded.
  kUnknown,
  // Part of MoltBrowser: a component extension (extensions::Manifest::
  // IsComponentLocation(): kComponent or kExternalComponent).
  kComponent,
  // Anything a user or an administrator installed: Web Store, unpacked,
  // command line, external or default-installed, enterprise policy.
  kInstalled,
};

// `known` is false when only an ID is at hand.
ExtensionOrigin ClassifyExtension(bool known, bool is_component_location);

// True if an extension of `origin` must stay out of a MoltNet profile: it
// may not run there (background page, service worker, content scripts,
// extension pages, web-accessible resources), load into its renderers, see
// its windows, tabs, requests or events, or control its settings.
bool IsKeptOutOfMoltNet(ExtensionOrigin origin);

// Certificate-error exceptions ("the user proceeded past this error with
// this certificate on this host") held in memory for one MoltNet profile.
// A decision key identifies the certificate chain and the error; the
// caller builds it (MoltNetSSLHostStateDelegate: error code + SHA-256 of
// the chain).
class CertDecisionMemory {
 public:
  CertDecisionMemory();
  CertDecisionMemory(const CertDecisionMemory&) = delete;
  CertDecisionMemory& operator=(const CertDecisionMemory&) = delete;
  ~CertDecisionMemory();

  void Allow(const std::string& host, const std::string& decision_key);
  bool IsAllowed(const std::string& host,
                 const std::string& decision_key) const;
  bool HasAnyFor(const std::string& host) const;
  bool empty() const { return allowed_.empty(); }

  // Forgets every exception for `host`.
  void Revoke(const std::string& host);
  // Forgets every exception.
  void Clear();
  // Forgets the exceptions of the hosts `matches(host)` is true for.
  template <typename HostPredicate>
  void RemoveIf(const HostPredicate& matches) {
    for (auto it = allowed_.begin(); it != allowed_.end();) {
      if (matches(it->first)) {
        it = allowed_.erase(it);
      } else {
        ++it;
      }
    }
  }

 private:
  // host -> decision keys.
  std::map<std::string, std::set<std::string>> allowed_;
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ISOLATION_POLICY_H_
