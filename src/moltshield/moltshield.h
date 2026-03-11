// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLTSHIELD_MOLTSHIELD_H_
#define MOLTSHIELD_MOLTSHIELD_H_

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace moltshield {

// Tracker types
enum class TrackerType {
  ADVERTISING,
  ANALYTICS,
  SOCIAL,
  CRYPTOMINER,
  FINGERPRINTING,
  MALWARE,
  UNKNOWN
};

// Blocked request info
struct BlockedRequest {
  std::string url;
  std::string domain;
  TrackerType type;
  std::string rule_matched;
  int64_t timestamp;
};

// Shield statistics for a tab
struct ShieldStats {
  int trackers_blocked;
  int ads_blocked;
  int fingerprinting_attempts_blocked;
  int cookies_blocked;
  int total_requests;
  int blocked_requests;
  float page_load_savings_ms;
};

// Fingerprinting protection level
enum class FingerprintProtection {
  OFF,         // No protection
  STANDARD,    // Basic fingerprint mitigation
  STRICT,      // Aggressive fingerprint blocking
  AGGRESSIVE   // Maximum randomization
};

// Cookie policy
enum class CookiePolicy {
  ALLOW_ALL,
  BLOCK_THIRD_PARTY,
  BLOCK_ALL_EXCEPT_ESSENTIAL,
  BLOCK_ALL
};

// ============================================================
// MoltShield — Privacy protection system
// ============================================================
// Privacy protection features:
//   - Tracker blocking (advertising, analytics, social)
//   - Fingerprinting mitigation (Canvas, WebGL, Audio, etc.)
//   - Cookie management (first-party, third-party, essential)
//   - Anti-tracking systems
//
// Anti-detection capabilities (from V4 spec Layer 4):
//   - Canvas noise injection
//   - WebGL parameter spoofing
//   - Audio context fingerprint shielding
//   - TLS fingerprint normalization
//   - Behavioral simulation
//   - Device profile randomization
// ============================================================
class MoltShield {
 public:
  MoltShield();
  ~MoltShield();

  // Initialize with filter lists
  bool Initialize(const std::string& data_dir);

  // ---- Tracker Blocking ----

  // Check if a URL should be blocked
  bool ShouldBlockRequest(const std::string& request_url,
                           const std::string& page_url,
                           const std::string& resource_type) const;

  // Get blocked requests for a tab
  std::vector<BlockedRequest> GetBlockedRequests(int tab_id) const;

  // Get shield statistics for a tab
  ShieldStats GetStats(int tab_id) const;

  // ---- Filter Lists ----

  // Load EasyList-compatible filter rules
  bool LoadFilterList(const std::string& path);

  // Update filter lists from remote sources
  bool UpdateFilterLists();

  // ---- Fingerprinting Protection ----

  // Set fingerprint protection level
  void SetFingerprintProtection(FingerprintProtection level);

  // Get Canvas noise injection script
  std::string GetCanvasNoiseScript() const;

  // Get WebGL parameter spoofing script
  std::string GetWebGLSpoofScript() const;

  // Get AudioContext protection script
  std::string GetAudioShieldScript() const;

  // Generate a randomized device profile
  std::string GenerateDeviceProfile() const;

  // ---- Cookie Management ----

  // Set cookie policy
  void SetCookiePolicy(CookiePolicy policy);

  // Check if a cookie should be allowed
  bool ShouldAllowCookie(const std::string& cookie_domain,
                          const std::string& page_domain,
                          bool is_essential) const;

  // Clear tracking cookies
  void ClearTrackingCookies();

  // ---- Anti-Tracking ----

  // Strip tracking parameters from URLs (utm_*, fbclid, etc.)
  std::string StripTrackingParams(const std::string& url) const;

  // Block referrer leaking
  std::string SanitizeReferrer(const std::string& referrer,
                                const std::string& target_url) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace moltshield

#endif  // MOLTSHIELD_MOLTSHIELD_H_
