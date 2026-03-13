// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield — Ad Blocker & Privacy Engine for MoltBrowser
//
// Full-featured ad and tracker blocker supporting:
//   - Adblock Plus (ABP) filter syntax parsing
//   - EasyList, EasyPrivacy, uBlock Origin filter lists
//   - YouTube ad blocking (network + cosmetic + scriptlets)
//   - Fingerprint protection (Canvas, WebGL, Audio noise)
//   - Cookie management (4 policies)
//   - URL tracking parameter stripping
//   - Cosmetic filtering (element hiding CSS injection)
//   - Scriptlet injection for anti-adblock bypass

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace molt_ai {

// ---- Resource types for network request matching ----
enum class ResourceType {
  DOCUMENT,
  SUBDOCUMENT,
  STYLESHEET,
  SCRIPT,
  IMAGE,
  FONT,
  OBJECT,
  XMLHTTPREQUEST,
  PING,
  CSP_REPORT,
  MEDIA,
  WEBSOCKET,
  WEBRTC,
  OTHER,
};

// ---- Filter rule types ----
enum class FilterType {
  NETWORK_BLOCK,      // Block a network request
  NETWORK_ALLOW,      // Exception rule (@@prefix)
  COSMETIC_HIDE,      // Element hiding (##selector)
  COSMETIC_EXCEPTION, // Element hiding exception (#@#selector)
  SCRIPTLET,          // Scriptlet injection (##+js(...))
};

// ---- A parsed ABP network filter rule ----
struct NetworkFilter {
  std::string raw_rule;         // Original rule text
  std::string pattern;          // URL matching pattern
  bool is_exception = false;    // @@prefix
  bool is_regex = false;        // /regex/ pattern
  bool match_case = false;      // $match-case option
  bool third_party = false;     // $third-party option
  bool first_party = false;     // $first-party option
  bool is_important = false;    // $important option

  // Domain options ($domain=)
  std::vector<std::string> include_domains;
  std::vector<std::string> exclude_domains;

  // Resource type options
  std::unordered_set<ResourceType> include_types;
  std::unordered_set<ResourceType> exclude_types;

  // Redirect target (for $redirect rules)
  std::string redirect;

  // CSP directive (for $csp rules)
  std::string csp_directive;
};

// ---- A parsed ABP cosmetic filter rule ----
struct CosmeticFilter {
  std::string raw_rule;
  std::string css_selector;
  std::string scriptlet;    // For ##+js(...) rules
  bool is_exception = false;
  std::vector<std::string> domains;       // Domains where this applies
  std::vector<std::string> exclude_domains; // Domains where this doesn't apply
};

// ---- Cookie blocking policy ----
enum class CookiePolicy {
  ALLOW_ALL,
  BLOCK_THIRD_PARTY,
  BLOCK_ALL_EXCEPT_ESSENTIAL,
  BLOCK_ALL,
};

// ---- Shield protection level ----
enum class ProtectionLevel {
  OFF,
  STANDARD,   // Block known trackers + ads
  STRICT,     // + fingerprinting + third-party cookies
  AGGRESSIVE, // + HTTPS-only + referrer stripping
};

// ---- Blocking decision result ----
struct BlockResult {
  bool should_block = false;
  bool should_redirect = false;
  std::string redirect_url;
  std::string matched_rule;
  std::string csp_directive;  // Content-Security-Policy to inject
  bool is_exception = false;
};

// ---- Statistics ----
struct ShieldStats {
  int trackers_blocked = 0;
  int ads_blocked = 0;
  int cookies_blocked = 0;
  int fingerprints_mitigated = 0;
  int https_upgrades = 0;
  int tracking_params_stripped = 0;
  int cosmetic_rules_applied = 0;
  int scriptlets_injected = 0;
  int total_requests_checked = 0;
};

// ============================================================
// MoltShield — Full ad blocker + privacy engine
// ============================================================
class MoltShield {
 public:
  MoltShield();
  ~MoltShield();

  MoltShield(const MoltShield&) = delete;
  MoltShield& operator=(const MoltShield&) = delete;

  // ---- Lifecycle ----

  bool Initialize(ProtectionLevel level = ProtectionLevel::STANDARD);
  void Shutdown();

  // ---- Filter List Management ----

  // Load a filter list from string content (EasyList format)
  // Returns number of rules parsed
  int LoadFilterList(const std::string& name, const std::string& content);

  // Load filter list from file path
  int LoadFilterListFromFile(const std::string& name,
                             const std::string& file_path);

  // Bundled default lists (loaded on Initialize)
  void LoadDefaultLists();

  // Get number of active rules
  int GetNetworkFilterCount() const;
  int GetCosmeticFilterCount() const;

  // ---- Network Request Filtering ----

  // Check if a network request should be blocked
  BlockResult ShouldBlockRequest(const std::string& url,
                                  const std::string& source_url,
                                  ResourceType resource_type) const;

  // Simplified version for backwards compatibility
  bool ShouldBlock(const std::string& url,
                   const std::string& page_url) const;

  // ---- Cosmetic Filtering ----

  // Get CSS selectors to hide elements on a page
  std::string GetCosmeticFiltersCSS(const std::string& page_url) const;

  // Get scriptlets to inject on a page (for anti-adblock bypass)
  std::string GetScriptletsJS(const std::string& page_url) const;

  // Get YouTube-specific blocking scripts
  std::string GetYouTubeAdBlockJS() const;

  // ---- URL Param Stripping ----

  std::string StripTrackingParams(const std::string& url) const;

  // ---- Fingerprint Protection ----

  std::string GetCanvasProtectionJS() const;
  std::string GetWebGLProtectionJS() const;
  std::string GetAudioProtectionJS() const;
  std::string GetAllFingerprintProtectionJS() const;

  // ---- HTTPS Upgrade ----

  bool ShouldUpgradeToHttps(const std::string& url) const;

  // ---- Cookie Policy ----

  void SetCookiePolicy(CookiePolicy policy);
  CookiePolicy GetCookiePolicy() const;
  bool ShouldBlockCookie(const std::string& domain,
                          const std::string& page_domain,
                          bool is_third_party) const;

  // ---- Referrer ----

  std::string SanitizeReferrer(const std::string& referrer_url,
                                const std::string& destination_url) const;

  // ---- Protection Level ----

  void SetProtectionLevel(ProtectionLevel level);
  ProtectionLevel GetProtectionLevel() const;

  // ---- Allowlist ----

  void AddToAllowlist(const std::string& domain);
  void RemoveFromAllowlist(const std::string& domain);
  bool IsAllowlisted(const std::string& domain) const;

  // ---- Stats ----

  ShieldStats GetStats() const;
  void ResetStats();

 private:
  // Parse a single ABP filter rule line
  void ParseRule(const std::string& line);
  void ParseNetworkRule(const std::string& line);
  void ParseCosmeticRule(const std::string& line,
                         const std::string& separator);

  // URL pattern matching
  bool MatchesPattern(const std::string& url,
                      const NetworkFilter& filter) const;
  bool MatchesDomain(const std::string& domain,
                     const NetworkFilter& filter) const;

  // Extract domain from URL
  static std::string ExtractDomain(const std::string& url);
  static std::string ExtractHost(const std::string& url);
  static bool DomainMatches(const std::string& domain,
                            const std::string& pattern);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_H_
