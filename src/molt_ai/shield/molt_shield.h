// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield — Privacy & Security Engine for MoltBrowser
//
// Features:
//   - Tracker/ad blocking (built-in blocklist + EasyList)
//   - Fingerprint protection (Canvas, WebGL, Audio noise)
//   - Cookie management (4 policies)
//   - URL tracking parameter stripping
//   - Referrer policy enforcement
//   - HTTPS upgrade enforcement

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_H_

#include <string>
#include <vector>
#include <unordered_set>
#include <memory>

namespace molt_ai {

// Cookie blocking policy
enum class CookiePolicy {
  ALLOW_ALL,                    // No blocking
  BLOCK_THIRD_PARTY,            // Block third-party cookies only
  BLOCK_ALL_EXCEPT_ESSENTIAL,   // Block all except session cookies
  BLOCK_ALL,                    // Block everything
};

// Shield protection level
enum class ProtectionLevel {
  OFF,        // No protection
  STANDARD,   // Block known trackers
  STRICT,     // Block trackers + fingerprinting + third-party cookies
  AGGRESSIVE, // Block everything + HTTPS-only + referrer stripping
};

// Statistics for the current browsing session
struct ShieldStats {
  int trackers_blocked = 0;
  int ads_blocked = 0;
  int cookies_blocked = 0;
  int fingerprints_mitigated = 0;
  int https_upgrades = 0;
  int tracking_params_stripped = 0;
};

// MoltShield engine
class MoltShield {
 public:
  MoltShield();
  ~MoltShield();

  MoltShield(const MoltShield&) = delete;
  MoltShield& operator=(const MoltShield&) = delete;

  // Initialize with protection level
  bool Initialize(ProtectionLevel level = ProtectionLevel::STANDARD);

  // Set protection level
  void SetProtectionLevel(ProtectionLevel level);
  ProtectionLevel GetProtectionLevel() const;

  // ---- URL Filtering ----

  // Check if a URL should be blocked (returns true = block)
  bool ShouldBlockRequest(const std::string& url,
                           const std::string& page_url) const;

  // Strip tracking parameters from URL (modifies in-place)
  std::string StripTrackingParams(const std::string& url) const;

  // Check if URL should be upgraded to HTTPS
  bool ShouldUpgradeToHttps(const std::string& url) const;

  // ---- Cookie Policy ----

  void SetCookiePolicy(CookiePolicy policy);
  CookiePolicy GetCookiePolicy() const;

  // Check if a cookie should be blocked
  bool ShouldBlockCookie(const std::string& domain,
                          const std::string& page_domain,
                          bool is_third_party) const;

  // ---- Fingerprint Protection ----

  // Get JavaScript to inject for Canvas fingerprint noise
  std::string GetCanvasProtectionJS() const;

  // Get JavaScript to inject for WebGL fingerprint noise
  std::string GetWebGLProtectionJS() const;

  // Get JavaScript to inject for AudioContext protection
  std::string GetAudioProtectionJS() const;

  // ---- Referrer Policy ----

  // Sanitize referrer for cross-origin requests
  std::string SanitizeReferrer(const std::string& referrer_url,
                                const std::string& destination_url) const;

  // ---- Allowlist/Blocklist ----

  // Add domain to allowlist (bypass blocking)
  void AddToAllowlist(const std::string& domain);

  // Remove domain from allowlist
  void RemoveFromAllowlist(const std::string& domain);

  // Check if domain is in allowlist
  bool IsAllowlisted(const std::string& domain) const;

  // ---- Stats ----

  ShieldStats GetStats() const;
  void ResetStats();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_H_
