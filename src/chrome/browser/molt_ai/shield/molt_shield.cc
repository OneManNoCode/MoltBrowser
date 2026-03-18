// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield implementation — full ABP filter engine
// Parses EasyList/uBlock Origin filter syntax and blocks ads/trackers.

#include "chrome/browser/molt_ai/shield/molt_shield.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unordered_map>

namespace molt_ai {

// ============================================================
// Internal implementation
// ============================================================
struct MoltShield::Impl {
  ProtectionLevel protection_level = ProtectionLevel::STANDARD;
  CookiePolicy cookie_policy = CookiePolicy::BLOCK_THIRD_PARTY;

  // Network filter rules indexed by domain for fast lookup
  std::vector<NetworkFilter> network_block_rules;
  std::vector<NetworkFilter> network_allow_rules;

  // Domain-indexed hash map for O(1) domain lookups
  std::unordered_map<std::string, std::vector<size_t>> domain_block_index;
  std::unordered_map<std::string, std::vector<size_t>> domain_allow_index;

  // Rules without specific domains (checked for every request)
  std::vector<size_t> generic_block_indices;
  std::vector<size_t> generic_allow_indices;

  // Cosmetic filters
  std::vector<CosmeticFilter> cosmetic_hide_rules;
  std::vector<CosmeticFilter> cosmetic_exception_rules;
  std::vector<CosmeticFilter> scriptlet_rules;

  // Allowlist
  std::unordered_set<std::string> allowlisted_domains;

  // Stats (mutable for const methods)
  mutable ShieldStats stats;

  // Tracking params to strip
  std::unordered_set<std::string> tracking_params;

  // Known tracker/ad domains for fast O(1) blocking
  std::unordered_set<std::string> blocked_domains;

  void InitializeTrackingParams() {
    tracking_params = {
        // Google Analytics / Ads
        "utm_source", "utm_medium", "utm_campaign", "utm_term",
        "utm_content", "utm_id", "utm_source_platform",
        "gclid", "gclsrc", "dclid", "gbraid", "wbraid",
        // Facebook
        "fbclid", "fb_action_ids", "fb_action_types", "fb_ref",
        "fb_source", "fbadid",
        // Microsoft / Bing
        "msclkid", "mc_cid", "mc_eid",
        // HubSpot
        "hsa_cam", "hsa_grp", "hsa_mt", "hsa_src", "hsa_ad",
        "hsa_acc", "hsa_net", "hsa_ver", "hsa_la", "hsa_ol",
        "hsa_kw", "_hsenc", "_hsmi", "__hstc", "__hsfp",
        // Mailchimp
        "mc_cid", "mc_eid",
        // Adobe / Marketo
        "s_kwcid", "ef_id", "mkt_tok",
        // Yandex
        "yclid", "ymclid", "_openstat",
        // Twitter
        "twclid",
        // TikTok
        "ttclid",
        // Snapchat
        "sclid", "ScCid",
        // Pinterest
        "epik",
        // Generic
        "ref", "referer", "referrer", "affiliate",
        "tracking_id", "click_id", "campaign_id",
    };
  }

  void InitializeDefaultBlockedDomains() {
    blocked_domains = {
        // Major ad networks
        "doubleclick.net", "googlesyndication.com", "googleadservices.com",
        "google-analytics.com", "googletagmanager.com",
        "googletagservices.com",
        "pagead2.googlesyndication.com", "adservice.google.com",
        "adsense.google.com", "afs.googlesyndication.com",
        // Facebook tracking
        "facebook.net", "connect.facebook.net",
        "pixel.facebook.com", "an.facebook.com",
        // Amazon ads
        "amazon-adsystem.com", "aax.amazon-adsystem.com",
        "fls-na.amazon.com",
        // Microsoft / Bing ads
        "bat.bing.com", "ads.microsoft.com",
        // Twitter ads
        "ads-api.twitter.com", "analytics.twitter.com",
        "ads-twitter.com",
        // Advertising networks
        "adnxs.com", "adsrvr.org", "criteo.com", "criteo.net",
        "taboola.com", "outbrain.com", "revcontent.com",
        "media.net", "mgid.com", "zergnet.com",
        "popads.net", "popcash.net", "propellerads.com",
        "adcolony.com", "admob.com",
        "appsflyer.com", "adjust.com", "branch.io",
        // Tracking / Analytics
        "scorecardresearch.com", "quantserve.com",
        "comscore.com", "omtrdc.net", "demdex.net",
        "krxd.net", "bluekai.com", "exelator.com",
        "rubiconproject.com", "pubmatic.com",
        "openx.net", "casalemedia.com",
        "mathtag.com", "rlcdn.com", "sharethrough.com",
        "indexexchange.com", "33across.com",
        "bidswitch.net", "smartadserver.com",
        "serving-sys.com", "flashtalking.com",
        // Trackers
        "hotjar.com", "mixpanel.com", "segment.com",
        "amplitude.com", "fullstory.com", "mouseflow.com",
        "luckyorange.com", "crazyegg.com", "optimizely.com",
        "newrelic.com", "bugsnag.com", "rollbar.com",
        // Social widgets / tracking
        "platform.linkedin.com", "snap.licdn.com",
        "ads.linkedin.com", "px.ads.linkedin.com",
        // Malware domains
        "malware-check.disconnect.me",
    };
  }
};

// ============================================================
// Constructor / Destructor
// ============================================================

MoltShield::MoltShield() : impl_(std::make_unique<Impl>()) {}
MoltShield::~MoltShield() = default;

// ============================================================
// Lifecycle
// ============================================================

bool MoltShield::Initialize(ProtectionLevel level) {
  impl_->protection_level = level;
  impl_->InitializeTrackingParams();
  impl_->InitializeDefaultBlockedDomains();

  switch (level) {
    case ProtectionLevel::OFF:
      impl_->cookie_policy = CookiePolicy::ALLOW_ALL;
      break;
    case ProtectionLevel::STANDARD:
    case ProtectionLevel::STRICT:
      impl_->cookie_policy = CookiePolicy::BLOCK_THIRD_PARTY;
      break;
    case ProtectionLevel::AGGRESSIVE:
      impl_->cookie_policy = CookiePolicy::BLOCK_ALL_EXCEPT_ESSENTIAL;
      break;
  }

  LoadDefaultLists();
  return true;
}

void MoltShield::Shutdown() {
  impl_->network_block_rules.clear();
  impl_->network_allow_rules.clear();
  impl_->cosmetic_hide_rules.clear();
  impl_->domain_block_index.clear();
  impl_->domain_allow_index.clear();
}

// ============================================================
// Filter List Parsing
// ============================================================

int MoltShield::LoadFilterList(const std::string& name,
                                const std::string& content) {
  int count = 0;
  std::istringstream stream(content);
  std::string line;

  while (std::getline(stream, line)) {
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) continue;
    line = line.substr(start);
    size_t end = line.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) line = line.substr(0, end + 1);

    if (line.empty()) continue;
    if (line[0] == '!' || line.substr(0, 8) == "[Adblock") continue;

    // Generic cosmetic rules starting with ##
    if (line.size() > 2 && line[0] == '#' && line[1] == '#') {
      ParseCosmeticRule(line, "##");
      count++;
      continue;
    }
    if (line.size() > 3 && line[0] == '#' && line[1] == '@' &&
        line[2] == '#') {
      ParseCosmeticRule(line, "#@#");
      count++;
      continue;
    }

    ParseRule(line);
    count++;
  }

  return count;
}

int MoltShield::LoadFilterListFromFile(const std::string& name,
                                        const std::string& file_path) {
  std::ifstream file(file_path);
  if (!file.is_open()) return 0;

  std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
  return LoadFilterList(name, content);
}

void MoltShield::LoadDefaultLists() {
  // YouTube-specific ad blocking rules
  const char* youtube_rules = R"(
||youtube.com/pagead/
||youtube.com/ptracking
||youtube.com/api/stats/ads
||youtube.com/api/stats/qoe?
||youtube.com/get_midroll_info
||youtube.com/youtubei/v1/player/ad_break
||googlevideo.com/videoplayback*&ctier=L
||googlevideo.com/initplayback
||static.doubleclick.net^
||s0.2mdn.net^
||ad.doubleclick.net^
||ads.youtube.com^
youtube.com##.ytp-ad-module
youtube.com##.ytp-ad-overlay-container
youtube.com##.ytp-ad-progress
youtube.com##.ytp-ad-progress-list
youtube.com##.video-ads
youtube.com##.ytd-ad-slot-renderer
youtube.com##.ytd-banner-promo-renderer
youtube.com##.ytd-compact-promoted-video-renderer
youtube.com##.ytd-display-ad-renderer
youtube.com##.ytd-promoted-sparkles-web-renderer
youtube.com##.ytd-promoted-video-renderer
youtube.com##.ytd-player-legacy-desktop-watch-ads-renderer
youtube.com##.ytd-action-companion-ad-renderer
youtube.com##.ytd-in-feed-ad-layout-renderer
youtube.com##.ytd-merch-shelf-renderer
youtube.com##.ytp-ad-skip-button-container
youtube.com##.ytp-ad-text-overlay
youtube.com##.ytp-ad-image-overlay
youtube.com##tp-yt-paper-dialog:has(#feedback.ytd-official-card-renderer)
youtube.com##+js(set, yt.config_.EXPERIMENT_FLAGS.ad_pod_disable_bumpable, true)
youtube.com##+js(set, yt.config_.EXPERIMENT_FLAGS.html5_enable_ads, false)
)";
  LoadFilterList("molt_youtube", youtube_rules);

  // Essential network blocking rules
  const char* essential_rules = R"(
||doubleclick.net^
||googlesyndication.com^
||googleadservices.com^
||google-analytics.com^
||googletagmanager.com^
||googletagservices.com^
||facebook.net^$third-party
||connect.facebook.net^$third-party
||pixel.facebook.com^
||amazon-adsystem.com^
||adsrvr.org^
||criteo.com^$third-party
||criteo.net^$third-party
||taboola.com^$third-party
||outbrain.com^$third-party
||scorecardresearch.com^
||quantserve.com^
||comscore.com^$third-party
||hotjar.com^$third-party
||mixpanel.com^$third-party
||segment.com^$third-party
||amplitude.com^$third-party
||fullstory.com^$third-party
||adnxs.com^
||pubmatic.com^
||openx.net^
||rubiconproject.com^
||casalemedia.com^
||indexexchange.com^
||33across.com^
||bidswitch.net^
||smartadserver.com^
||bat.bing.com^
||ads.microsoft.com^
||ads-api.twitter.com^
||analytics.twitter.com^
||ads.linkedin.com^
||platform.linkedin.com^$third-party
||popads.net^
||popcash.net^
||propellerads.com^
||media.net^$third-party
||mgid.com^$third-party
||zergnet.com^$third-party
||serving-sys.com^
||flashtalking.com^
||sharethrough.com^
||demdex.net^
||krxd.net^
||bluekai.com^
||exelator.com^
||omtrdc.net^
||newrelic.com^$third-party
||bugsnag.com^$third-party
##.ad-banner
##.ad-container
##.ad-wrapper
##.ad-slot
##.adsbygoogle
##[id^="google_ads_"]
##[id^="div-gpt-ad"]
##.sponsored-content
##.promoted-content
##.native-ad
)";
  LoadFilterList("molt_essential", essential_rules);

  // ---- Cookie Consent Popup Blocking ----
  // Blocks cookie consent banners, GDPR popups, and consent management
  // platforms. This protects user privacy by preventing tracking consent
  // dialogs from appearing and auto-declining when interaction is required.
  const char* cookie_consent_rules = R"(
! === OneTrust (Nike, ESPN, many Fortune 500 sites) ===
##onetrust-consent-sdk
###onetrust-consent-sdk
###onetrust-banner-sdk
##.onetrust-pc-dark-filter
###onetrust-pc-sdk
##.ot-fade-in
||cdn.cookielaw.org/consent$third-party
||cdn.cookielaw.org/scripttemplates$third-party
||geolocation.onetrust.com^
||optanon.blob.core.windows.net^

! === CookieBot ===
###CybotCookiebotDialog
###CybotCookiebotDialogBodyUnderlay
##.CybotCookiebotDialogActive
||consent.cookiebot.com^$third-party
||consentcdn.cookiebot.com^$third-party

! === Quantcast / CMP2 ===
###qc-cmp2-container
##.qc-cmp2-summary-buttons
###qc-cmp-ui-container
||quantcast.mgr.consensu.org^
||cmp.quantcast.com^

! === TrustArc ===
##.truste_overlay
##.truste_box_overlay
###truste-consent-track
###truste-consent-content
||consent-pref.trustarc.com^$third-party
||consent.trustarc.com^$third-party

! === Didomi ===
###didomi-host
###didomi-popup
###didomi-notice
##.didomi-popup-backdrop
||sdk.privacy-center.org^$third-party

! === CookieYes ===
##.cky-consent-container
##.cky-consent-bar
###cky-consent
||cdn-cookieyes.com^$third-party

! === Klaro ===
##.klaro .cookie-modal
##.klaro .cookie-notice
##.klaro .cm-modal

! === Generic cookie banners ===
##.cookie-banner
##.cookie-notice
##.cookie-consent
##.cookie-popup
##.cookie-wall
##.cookie-bar
##.cookie-disclaimer
##.cc-window
##.cc-banner
##.cc-revoke
###cookie-law-info-bar
##.cookie-law-info-bar
###cookie-notice
###cookieNotice
###cookie-consent-banner
##.gdpr-banner
##.gdpr-consent
##.gdpr-popup
##.privacy-banner
##.privacy-notice
##.consent-banner
##.consent-popup
##.consent-modal
##.consent-overlay
##[class*="cookie-banner"]
##[class*="cookie-consent"]
##[class*="CookieConsent"]
##[id*="cookie-banner"]
##[id*="cookie-consent"]
##[id*="cookieconsent"]

! === Osano ===
##.osano-cm-dialog
##.osano-cm-window
||cmp.osano.com^$third-party

! === Sourcepoint ===
##.sp-message-container
###sp_message_container
||sourcepoint.mgr.consensu.org^

! === Usercentrics ===
###usercentrics-root
||app.usercentrics.eu^$third-party

! === Complianz ===
###cmplz-cookiebanner-container
##.cmplz-cookiebanner
)";
  LoadFilterList("molt_cookie_consent", cookie_consent_rules);
}

// ============================================================
// Rule Parsing
// ============================================================

void MoltShield::ParseRule(const std::string& line) {
  size_t pos = line.find("##");
  if (pos != std::string::npos && (pos == 0 || line[pos - 1] != '\\')) {
    size_t exc_pos = line.find("#@#");
    if (exc_pos != std::string::npos) {
      ParseCosmeticRule(line, "#@#");
    } else {
      ParseCosmeticRule(line, "##");
    }
    return;
  }
  ParseNetworkRule(line);
}

void MoltShield::ParseNetworkRule(const std::string& line) {
  NetworkFilter filter;
  filter.raw_rule = line;

  std::string rule = line;

  if (rule.size() >= 2 && rule.substr(0, 2) == "@@") {
    filter.is_exception = true;
    rule = rule.substr(2);
  }

  size_t dollar = rule.rfind('$');
  std::string pattern_part = rule;
  std::string options_part;

  if (dollar != std::string::npos && dollar > 0) {
    if (rule[0] != '/' || rule[dollar - 1] != '/') {
      pattern_part = rule.substr(0, dollar);
      options_part = rule.substr(dollar + 1);
    }
  }

  if (pattern_part.size() >= 2 && pattern_part.front() == '/' &&
      pattern_part.back() == '/') {
    filter.is_regex = true;
    filter.pattern = pattern_part.substr(1, pattern_part.size() - 2);
  } else {
    filter.pattern = pattern_part;
  }

  if (!options_part.empty()) {
    std::istringstream opts(options_part);
    std::string opt;
    while (std::getline(opts, opt, ',')) {
      if (opt == "third-party" || opt == "3p") {
        filter.third_party = true;
      } else if (opt == "~third-party" || opt == "1p" ||
                 opt == "first-party") {
        filter.first_party = true;
      } else if (opt == "match-case") {
        filter.match_case = true;
      } else if (opt == "important") {
        filter.is_important = true;
      } else if (opt == "script") {
        filter.include_types.insert(ResourceType::SCRIPT);
      } else if (opt == "image") {
        filter.include_types.insert(ResourceType::IMAGE);
      } else if (opt == "stylesheet" || opt == "css") {
        filter.include_types.insert(ResourceType::STYLESHEET);
      } else if (opt == "xmlhttprequest" || opt == "xhr") {
        filter.include_types.insert(ResourceType::XMLHTTPREQUEST);
      } else if (opt == "media") {
        filter.include_types.insert(ResourceType::MEDIA);
      } else if (opt == "subdocument") {
        filter.include_types.insert(ResourceType::SUBDOCUMENT);
      } else if (opt == "websocket") {
        filter.include_types.insert(ResourceType::WEBSOCKET);
      } else if (opt == "font") {
        filter.include_types.insert(ResourceType::FONT);
      } else if (opt == "object") {
        filter.include_types.insert(ResourceType::OBJECT);
      } else if (opt == "ping") {
        filter.include_types.insert(ResourceType::PING);
      } else if (opt.substr(0, 7) == "domain=") {
        std::string domains = opt.substr(7);
        std::istringstream dstream(domains);
        std::string d;
        while (std::getline(dstream, d, '|')) {
          if (!d.empty()) {
            if (d[0] == '~') {
              filter.exclude_domains.push_back(d.substr(1));
            } else {
              filter.include_domains.push_back(d);
            }
          }
        }
      } else if (opt.substr(0, 9) == "redirect=") {
        filter.redirect = opt.substr(9);
      } else if (opt.substr(0, 4) == "csp=") {
        filter.csp_directive = opt.substr(4);
      }
    }
  }

  if (filter.is_exception) {
    size_t idx = impl_->network_allow_rules.size();
    impl_->network_allow_rules.push_back(filter);
    if (!filter.include_domains.empty()) {
      for (const auto& d : filter.include_domains) {
        impl_->domain_allow_index[d].push_back(idx);
      }
    } else {
      impl_->generic_allow_indices.push_back(idx);
    }
  } else {
    size_t idx = impl_->network_block_rules.size();
    impl_->network_block_rules.push_back(filter);
    if (!filter.include_domains.empty()) {
      for (const auto& d : filter.include_domains) {
        impl_->domain_block_index[d].push_back(idx);
      }
    } else {
      impl_->generic_block_indices.push_back(idx);
    }
  }
}

void MoltShield::ParseCosmeticRule(const std::string& line,
                                    const std::string& separator) {
  CosmeticFilter filter;
  filter.raw_rule = line;

  size_t sep_pos = line.find(separator);
  if (sep_pos == std::string::npos) return;

  if (sep_pos > 0) {
    std::string domain_part = line.substr(0, sep_pos);
    std::istringstream ds(domain_part);
    std::string d;
    while (std::getline(ds, d, ',')) {
      if (!d.empty()) {
        if (d[0] == '~') {
          filter.exclude_domains.push_back(d.substr(1));
        } else {
          filter.domains.push_back(d);
        }
      }
    }
  }

  std::string after = line.substr(sep_pos + separator.size());

  if (separator == "#@#") {
    filter.is_exception = true;
    filter.css_selector = after;
    impl_->cosmetic_exception_rules.push_back(filter);
  } else if (after.substr(0, 4) == "+js(") {
    filter.scriptlet = after.substr(4);
    if (!filter.scriptlet.empty() && filter.scriptlet.back() == ')') {
      filter.scriptlet.pop_back();
    }
    impl_->scriptlet_rules.push_back(filter);
  } else {
    filter.css_selector = after;
    impl_->cosmetic_hide_rules.push_back(filter);
  }
}

// ============================================================
// Network Request Filtering
// ============================================================

bool MoltShield::MatchesPattern(const std::string& url,
                                 const NetworkFilter& filter) const {
  const std::string& pattern = filter.pattern;
  if (pattern.empty()) return false;

  std::string check_url = url;
  std::string check_pattern = pattern;
  if (!filter.match_case) {
    std::transform(check_url.begin(), check_url.end(),
                   check_url.begin(), ::tolower);
    std::transform(check_pattern.begin(), check_pattern.end(),
                   check_pattern.begin(), ::tolower);
  }

  // ||domain.com^ = start of domain anchor
  if (check_pattern.size() >= 2 && check_pattern.substr(0, 2) == "||") {
    std::string domain_pattern = check_pattern.substr(2);
    if (!domain_pattern.empty() && domain_pattern.back() == '^') {
      domain_pattern.pop_back();
    }

    std::string host = ExtractHost(check_url);

    // Check path patterns: ||domain.com/path
    size_t slash = domain_pattern.find('/');
    if (slash != std::string::npos) {
      std::string d = domain_pattern.substr(0, slash);
      std::string path = domain_pattern.substr(slash);
      if ((host == d || (host.size() > d.size() &&
           host.substr(host.size() - d.size() - 1) == "." + d)) &&
          check_url.find(path) != std::string::npos) {
        return true;
      }
      return false;
    }

    if (host == domain_pattern ||
        (host.size() > domain_pattern.size() &&
         host.substr(host.size() - domain_pattern.size() - 1) ==
             "." + domain_pattern)) {
      return true;
    }
    return false;
  }

  // |pattern = anchored to URL start
  if (!check_pattern.empty() && check_pattern[0] == '|') {
    return check_url.find(check_pattern.substr(1)) == 0;
  }

  // pattern| = anchored to URL end
  if (!check_pattern.empty() && check_pattern.back() == '|') {
    std::string p = check_pattern.substr(0, check_pattern.size() - 1);
    return check_url.size() >= p.size() &&
           check_url.substr(check_url.size() - p.size()) == p;
  }

  // * wildcard matching
  if (check_pattern.find('*') != std::string::npos) {
    std::vector<std::string> parts;
    std::istringstream ss(check_pattern);
    std::string part;
    while (std::getline(ss, part, '*')) {
      if (!part.empty()) parts.push_back(part);
    }
    size_t pos = 0;
    for (const auto& p : parts) {
      size_t found = check_url.find(p, pos);
      if (found == std::string::npos) return false;
      pos = found + p.size();
    }
    return true;
  }

  // ^ separator
  if (check_pattern.find('^') != std::string::npos) {
    std::string clean = check_pattern;
    size_t hat = clean.find('^');
    if (hat != std::string::npos) {
      clean = clean.substr(0, hat);
    }
    return check_url.find(clean) != std::string::npos;
  }

  // Simple substring match
  return check_url.find(check_pattern) != std::string::npos;
}

bool MoltShield::MatchesDomain(const std::string& domain,
                                const NetworkFilter& filter) const {
  for (const auto& ed : filter.exclude_domains) {
    if (DomainMatches(domain, ed)) return false;
  }
  if (filter.include_domains.empty()) return true;
  for (const auto& id : filter.include_domains) {
    if (DomainMatches(domain, id)) return true;
  }
  return false;
}

BlockResult MoltShield::ShouldBlockRequest(
    const std::string& url,
    const std::string& source_url,
    ResourceType resource_type) const {
  BlockResult result;
  impl_->stats.total_requests_checked++;

  if (impl_->protection_level == ProtectionLevel::OFF) return result;

  std::string request_domain = ExtractDomain(url);
  std::string page_domain = ExtractDomain(source_url);

  if (IsAllowlisted(page_domain)) return result;

  // Fast O(1) domain blocklist
  std::string host = ExtractHost(url);
  if (impl_->blocked_domains.count(host) > 0) {
    result.should_block = true;
    result.matched_rule = "domain:" + host;
    impl_->stats.ads_blocked++;
    return result;
  }

  bool is_third_party = (request_domain != page_domain);

  // Check exception rules
  auto check_allow = [&](size_t idx) -> bool {
    const auto& filter = impl_->network_allow_rules[idx];
    if (filter.third_party && !is_third_party) return false;
    if (filter.first_party && is_third_party) return false;
    if (!filter.include_types.empty() &&
        filter.include_types.count(resource_type) == 0) return false;
    if (!MatchesDomain(page_domain, filter)) return false;
    return MatchesPattern(url, filter);
  };

  auto it_allow = impl_->domain_allow_index.find(page_domain);
  if (it_allow != impl_->domain_allow_index.end()) {
    for (size_t idx : it_allow->second) {
      if (check_allow(idx)) { result.is_exception = true; return result; }
    }
  }
  for (size_t idx : impl_->generic_allow_indices) {
    if (check_allow(idx)) { result.is_exception = true; return result; }
  }

  // Check block rules
  auto check_block = [&](size_t idx) -> bool {
    const auto& filter = impl_->network_block_rules[idx];
    if (filter.third_party && !is_third_party) return false;
    if (filter.first_party && is_third_party) return false;
    if (!filter.include_types.empty() &&
        filter.include_types.count(resource_type) == 0) return false;
    if (!MatchesDomain(page_domain, filter)) return false;
    return MatchesPattern(url, filter);
  };

  auto it_block = impl_->domain_block_index.find(page_domain);
  if (it_block != impl_->domain_block_index.end()) {
    for (size_t idx : it_block->second) {
      if (check_block(idx)) {
        const auto& f = impl_->network_block_rules[idx];
        result.should_block = true;
        result.matched_rule = f.raw_rule;
        if (!f.redirect.empty()) {
          result.should_redirect = true;
          result.redirect_url = f.redirect;
        }
        if (!f.csp_directive.empty()) {
          result.csp_directive = f.csp_directive;
        }
        impl_->stats.ads_blocked++;
        return result;
      }
    }
  }

  for (size_t idx : impl_->generic_block_indices) {
    if (check_block(idx)) {
      const auto& f = impl_->network_block_rules[idx];
      result.should_block = true;
      result.matched_rule = f.raw_rule;
      if (!f.redirect.empty()) {
        result.should_redirect = true;
        result.redirect_url = f.redirect;
      }
      impl_->stats.ads_blocked++;
      return result;
    }
  }

  return result;
}

bool MoltShield::ShouldBlock(const std::string& url,
                              const std::string& page_url) const {
  return ShouldBlockRequest(url, page_url, ResourceType::OTHER).should_block;
}

// ============================================================
// Cosmetic Filtering
// ============================================================

std::string MoltShield::GetCosmeticFiltersCSS(
    const std::string& page_url) const {
  std::string host = ExtractHost(page_url);
  std::string css;

  for (const auto& filter : impl_->cosmetic_hide_rules) {
    bool applies = filter.domains.empty();
    for (const auto& d : filter.domains) {
      if (DomainMatches(host, d)) { applies = true; break; }
    }
    for (const auto& d : filter.exclude_domains) {
      if (DomainMatches(host, d)) { applies = false; break; }
    }
    if (applies) {
      for (const auto& exc : impl_->cosmetic_exception_rules) {
        if (exc.css_selector == filter.css_selector) {
          for (const auto& d : exc.domains) {
            if (DomainMatches(host, d)) { applies = false; break; }
          }
        }
        if (!applies) break;
      }
    }
    if (applies && !filter.css_selector.empty()) {
      css += filter.css_selector + " { display: none !important; }\n";
      impl_->stats.cosmetic_rules_applied++;
    }
  }

  return css;
}

std::string MoltShield::GetScriptletsJS(const std::string& page_url) const {
  std::string host = ExtractHost(page_url);
  std::string js;

  for (const auto& filter : impl_->scriptlet_rules) {
    bool applies = filter.domains.empty();
    for (const auto& d : filter.domains) {
      if (DomainMatches(host, d)) { applies = true; break; }
    }
    if (applies && !filter.scriptlet.empty()) {
      std::vector<std::string> args;
      std::istringstream ss(filter.scriptlet);
      std::string arg;
      while (std::getline(ss, arg, ',')) {
        size_t s = arg.find_first_not_of(" \t");
        size_t e = arg.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos) {
          args.push_back(arg.substr(s, e - s + 1));
        }
      }
      if (args.size() >= 2 && args[0] == "set") {
        std::string value = args.size() >= 3 ? args[2] : "undefined";
        js += "(function(){try{Object.defineProperty(window,'" + args[1] +
              "',{value:" + value +
              ",writable:false,configurable:false});}catch(e){}})();\n";
      }
      impl_->stats.scriptlets_injected++;
    }
  }
  return js;
}

std::string MoltShield::GetYouTubeAdBlockJS() const {
  return R"JS(
(function(){'use strict';
var origFetch=window.fetch;
window.fetch=function(){
  var url=typeof arguments[0]==='string'?arguments[0]:(arguments[0]&&arguments[0].url)||'';
  if(url.indexOf('/pagead/')>-1||url.indexOf('/ptracking')>-1||
     url.indexOf('get_midroll_')>-1||url.indexOf('/api/stats/ads')>-1||
     url.indexOf('doubleclick.net')>-1||url.indexOf('googlesyndication.com')>-1||
     url.indexOf('/ad_break')>-1){return new Promise(function(){});}
  return origFetch.apply(this,arguments);
};
var origOpen=XMLHttpRequest.prototype.open;
XMLHttpRequest.prototype.open=function(m,u){
  if(typeof u==='string'&&(u.indexOf('/pagead/')>-1||u.indexOf('/ptracking')>-1||
     u.indexOf('doubleclick.net')>-1||u.indexOf('googlesyndication.com')>-1||
     u.indexOf('/api/stats/ads')>-1)){
    return origOpen.call(this,m,'data:text/plain,');
  }
  return origOpen.apply(this,arguments);
};
function skipAd(){
  var v=document.querySelector('video.html5-main-video');
  if(!v)return;
  var ao=document.querySelector('.ytp-ad-player-overlay');
  var am=document.querySelector('.ytp-ad-module');
  if(ao||(am&&am.children.length>0)){
    v.currentTime=v.duration||9999;v.playbackRate=16;
    var sb=document.querySelector('.ytp-ad-skip-button,.ytp-skip-ad-button,.ytp-ad-skip-button-modern');
    if(sb)sb.click();
  }
}
function removeAds(){
  var sels=['.ytp-ad-module','.ytp-ad-overlay-container','.video-ads',
    '.ytd-ad-slot-renderer','.ytd-banner-promo-renderer',
    '.ytd-compact-promoted-video-renderer','.ytd-display-ad-renderer',
    '.ytd-promoted-sparkles-web-renderer','.ytd-promoted-video-renderer',
    '.ytd-player-legacy-desktop-watch-ads-renderer',
    '.ytd-action-companion-ad-renderer','.ytd-in-feed-ad-layout-renderer',
    '.ytd-merch-shelf-renderer','#masthead-ad','#player-ads',
    '.ytp-ad-text-overlay','.ytp-ad-image-overlay'];
  sels.forEach(function(s){
    document.querySelectorAll(s).forEach(function(e){
      e.style.display='none';e.remove();
    });
  });
}
if(typeof yt!=='undefined'&&yt.config_&&yt.config_.EXPERIMENT_FLAGS){
  yt.config_.EXPERIMENT_FLAGS.ad_pod_disable_bumpable=true;
  yt.config_.EXPERIMENT_FLAGS.html5_enable_ads=false;
}
setInterval(skipAd,500);setInterval(removeAds,1000);
new MutationObserver(function(){skipAd();removeAds();})
  .observe(document.documentElement,{childList:true,subtree:true});
console.log('[MoltShield] YouTube ad protection active');
})();
)JS";
}

std::string MoltShield::GetCookieConsentDeclineJS() const {
  return R"JS(
(function(){'use strict';
// MoltShield Cookie Consent Auto-Decline
// Automatically clicks "Decline All" / "Reject All" buttons
// on consent popups that require user interaction.

function declineConsent(){
  // OneTrust "Reject All" button
  var b=document.querySelector('#onetrust-reject-all-handler');
  if(b){b.click();console.log('[MoltShield] OneTrust declined');return true;}
  // CookieBot decline
  b=document.querySelector('#CybotCookiebotDialogBodyButtonDecline');
  if(b){b.click();console.log('[MoltShield] CookieBot declined');return true;}
  // Quantcast "Disagree"
  b=document.querySelector('.qc-cmp2-summary-buttons button:first-child');
  if(b&&/disagree|reject|decline/i.test(b.textContent)){b.click();return true;}
  // Didomi disagree
  b=document.querySelector('#didomi-notice-disagree-button');
  if(b){b.click();console.log('[MoltShield] Didomi declined');return true;}
  // CookieYes reject
  b=document.querySelector('.cky-btn-reject');
  if(b){b.click();console.log('[MoltShield] CookieYes declined');return true;}
  // Klaro decline
  b=document.querySelector('.klaro .cm-btn-decline');
  if(b){b.click();console.log('[MoltShield] Klaro declined');return true;}
  // Osano deny
  b=document.querySelector('.osano-cm-deny');
  if(b){b.click();console.log('[MoltShield] Osano declined');return true;}
  // Complianz deny
  b=document.querySelector('.cmplz-deny');
  if(b){b.click();console.log('[MoltShield] Complianz declined');return true;}
  // Usercentrics deny
  var uc=document.querySelector('#usercentrics-root');
  if(uc&&uc.shadowRoot){
    b=uc.shadowRoot.querySelector('[data-testid="uc-deny-all-button"]');
    if(b){b.click();console.log('[MoltShield] Usercentrics declined');return true;}
  }
  // Generic: find buttons with decline/reject text
  var btns=document.querySelectorAll(
    'button, [role="button"], a.btn, .consent-modal button');
  for(var i=0;i<btns.length;i++){
    var t=(btns[i].textContent||'').trim().toLowerCase();
    if(/^(decline|reject|deny|refuse|ablehnen|refuser)(\s+all)?$/i.test(t)){
      btns[i].click();
      console.log('[MoltShield] Generic consent declined: '+t);
      return true;
    }
  }
  return false;
}

// Try immediately, then watch for dynamically injected consent dialogs
if(!declineConsent()){
  var attempts=0;
  var timer=setInterval(function(){
    if(declineConsent()||++attempts>20){clearInterval(timer);}
  },500);
  // Also observe DOM for consent dialogs appearing later
  new MutationObserver(function(mutations,observer){
    if(declineConsent()){observer.disconnect();}
  }).observe(document.documentElement,{childList:true,subtree:true});
}
})();
)JS";
}

// ============================================================
// URL Param Stripping
// ============================================================

std::string MoltShield::StripTrackingParams(const std::string& url) const {
  size_t q = url.find('?');
  if (q == std::string::npos) return url;

  std::string base = url.substr(0, q);
  std::string query = url.substr(q + 1);

  std::vector<std::string> kept;
  std::istringstream qs(query);
  std::string param;
  while (std::getline(qs, param, '&')) {
    size_t eq = param.find('=');
    std::string key = (eq != std::string::npos) ? param.substr(0, eq) : param;
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(),
                   lower_key.begin(), ::tolower);
    if (impl_->tracking_params.count(lower_key) == 0) {
      kept.push_back(param);
    } else {
      impl_->stats.tracking_params_stripped++;
    }
  }

  if (kept.empty()) return base;
  std::string result = base + "?";
  for (size_t i = 0; i < kept.size(); ++i) {
    if (i > 0) result += "&";
    result += kept[i];
  }
  return result;
}

// ============================================================
// Fingerprint Protection
// ============================================================

std::string MoltShield::GetCanvasProtectionJS() const {
  return R"JS(
(function(){
var ogi=CanvasRenderingContext2D.prototype.getImageData;
CanvasRenderingContext2D.prototype.getImageData=function(){
  var d=ogi.apply(this,arguments);
  for(var i=0;i<d.data.length;i+=4){d.data[i]^=(Math.random()*2)|0;d.data[i+1]^=(Math.random()*2)|0;}
  return d;
};
var otd=HTMLCanvasElement.prototype.toDataURL;
HTMLCanvasElement.prototype.toDataURL=function(){
  var c=this.getContext('2d');
  if(c){var d=c.getImageData(0,0,this.width,this.height);for(var i=0;i<d.data.length;i+=100)d.data[i]^=1;c.putImageData(d,0,0);}
  return otd.apply(this,arguments);
};
})();
)JS";
}

std::string MoltShield::GetWebGLProtectionJS() const {
  return R"JS(
(function(){
var gp=WebGLRenderingContext.prototype.getParameter;
WebGLRenderingContext.prototype.getParameter=function(p){
  if(p===0x1F00)return'MoltBrowser';if(p===0x1F01)return'MoltBrowser GPU';
  if(p===0x1F02)return'WebGL 1.0';if(p===0x9246)return'WebGL GLSL';
  return gp.call(this,p);
};
if(typeof WebGL2RenderingContext!=='undefined'){
  var gp2=WebGL2RenderingContext.prototype.getParameter;
  WebGL2RenderingContext.prototype.getParameter=function(p){
    if(p===0x1F00)return'MoltBrowser';if(p===0x1F01)return'MoltBrowser GPU';
    return gp2.call(this,p);
  };
}
})();
)JS";
}

std::string MoltShield::GetAudioProtectionJS() const {
  return R"JS(
(function(){
var ogf=AnalyserNode.prototype.getFloatFrequencyData;
AnalyserNode.prototype.getFloatFrequencyData=function(a){
  ogf.call(this,a);for(var i=0;i<a.length;i++)a[i]+=(Math.random()-0.5)*0.001;
};
var ogc=AudioBuffer.prototype.getChannelData;
AudioBuffer.prototype.getChannelData=function(c){
  var d=ogc.call(this,c);for(var i=0;i<d.length;i+=100)d[i]+=(Math.random()-0.5)*0.0001;return d;
};
})();
)JS";
}

std::string MoltShield::GetAllFingerprintProtectionJS() const {
  return GetCanvasProtectionJS() + GetWebGLProtectionJS() +
         GetAudioProtectionJS();
}

// ============================================================
// HTTPS / Cookie / Referrer / Level / Allowlist / Stats
// ============================================================

bool MoltShield::ShouldUpgradeToHttps(const std::string& url) const {
  if (impl_->protection_level == ProtectionLevel::AGGRESSIVE)
    return url.substr(0, 7) == "http://";
  return false;
}

void MoltShield::SetCookiePolicy(CookiePolicy policy) {
  impl_->cookie_policy = policy;
}
CookiePolicy MoltShield::GetCookiePolicy() const {
  return impl_->cookie_policy;
}
bool MoltShield::ShouldBlockCookie(const std::string& domain,
                                    const std::string& page_domain,
                                    bool is_third_party) const {
  switch (impl_->cookie_policy) {
    case CookiePolicy::ALLOW_ALL: return false;
    case CookiePolicy::BLOCK_THIRD_PARTY:
      if (is_third_party) { impl_->stats.cookies_blocked++; return true; }
      return false;
    case CookiePolicy::BLOCK_ALL_EXCEPT_ESSENTIAL:
      if (is_third_party || impl_->blocked_domains.count(domain) > 0) {
        impl_->stats.cookies_blocked++; return true;
      }
      return false;
    case CookiePolicy::BLOCK_ALL:
      impl_->stats.cookies_blocked++; return true;
  }
  return false;
}

std::string MoltShield::SanitizeReferrer(
    const std::string& referrer_url,
    const std::string& destination_url) const {
  if (impl_->protection_level == ProtectionLevel::OFF) return referrer_url;
  std::string ref_domain = ExtractDomain(referrer_url);
  std::string dest_domain = ExtractDomain(destination_url);
  if (ref_domain == dest_domain) return referrer_url;
  size_t proto_end = referrer_url.find("://");
  if (proto_end != std::string::npos) {
    size_t path_start = referrer_url.find('/', proto_end + 3);
    if (path_start != std::string::npos)
      return referrer_url.substr(0, path_start) + "/";
  }
  return referrer_url;
}

void MoltShield::SetProtectionLevel(ProtectionLevel level) {
  impl_->protection_level = level;
}
ProtectionLevel MoltShield::GetProtectionLevel() const {
  return impl_->protection_level;
}
void MoltShield::AddToAllowlist(const std::string& domain) {
  impl_->allowlisted_domains.insert(domain);
}
void MoltShield::RemoveFromAllowlist(const std::string& domain) {
  impl_->allowlisted_domains.erase(domain);
}
bool MoltShield::IsAllowlisted(const std::string& domain) const {
  return impl_->allowlisted_domains.count(domain) > 0;
}
ShieldStats MoltShield::GetStats() const { return impl_->stats; }
void MoltShield::ResetStats() { impl_->stats = ShieldStats{}; }
int MoltShield::GetNetworkFilterCount() const {
  return static_cast<int>(impl_->network_block_rules.size() +
                           impl_->network_allow_rules.size());
}
int MoltShield::GetCosmeticFilterCount() const {
  return static_cast<int>(impl_->cosmetic_hide_rules.size() +
                           impl_->cosmetic_exception_rules.size() +
                           impl_->scriptlet_rules.size());
}

// ============================================================
// Utility functions
// ============================================================

std::string MoltShield::ExtractDomain(const std::string& url) {
  std::string host = ExtractHost(url);
  size_t dot = host.rfind('.');
  if (dot == std::string::npos || dot == 0) return host;
  size_t dot2 = host.rfind('.', dot - 1);
  if (dot2 == std::string::npos) return host;
  std::string tld = host.substr(dot + 1);
  std::string sld = host.substr(dot2 + 1, dot - dot2 - 1);
  if ((sld == "co" || sld == "com" || sld == "org" || sld == "net" ||
       sld == "gov" || sld == "edu" || sld == "ac") && tld.size() <= 3) {
    size_t dot3 = host.rfind('.', dot2 - 1);
    if (dot3 != std::string::npos) return host.substr(dot3 + 1);
    return host;
  }
  return host.substr(dot2 + 1);
}

std::string MoltShield::ExtractHost(const std::string& url) {
  size_t start = url.find("://");
  if (start == std::string::npos) start = 0; else start += 3;
  size_t end = url.find('/', start);
  if (end == std::string::npos) end = url.size();
  std::string host = url.substr(start, end - start);
  size_t colon = host.find(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  if (host.substr(0, 4) == "www.") host = host.substr(4);
  return host;
}

bool MoltShield::DomainMatches(const std::string& domain,
                                const std::string& pattern) {
  if (domain == pattern) return true;
  if (domain.size() > pattern.size() &&
      domain.substr(domain.size() - pattern.size() - 1) == "." + pattern)
    return true;
  return false;
}

}  // namespace molt_ai
