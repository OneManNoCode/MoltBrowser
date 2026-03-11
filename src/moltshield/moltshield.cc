// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "src/moltshield/moltshield.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <random>

namespace moltshield {

struct MoltShield::Impl {
  FingerprintProtection fp_level = FingerprintProtection::STANDARD;
  CookiePolicy cookie_policy = CookiePolicy::BLOCK_THIRD_PARTY;
  std::unordered_set<std::string> blocked_domains;
  std::vector<std::string> filter_rules;
  std::string data_dir;
  bool initialized = false;

  // Known tracking URL parameters
  const std::vector<std::string> tracking_params = {
      "utm_source", "utm_medium", "utm_campaign", "utm_term",
      "utm_content", "fbclid", "gclid", "msclkid", "dclid",
      "mc_cid", "mc_eid", "yclid", "_openstat", "oly_enc_id",
      "oly_anon_id", "vero_id", "wickedid", "__s", "rb_clickid",
      "s_cid", "elqTrackId", "elqTrack", "assetType", "assetId",
      "recipientId", "campaignId", "siteId", "trkCampId",
  };

  // Known tracker domains (subset, expanded via filter lists)
  void InitializeDefaultBlocklist() {
    blocked_domains = {
        "doubleclick.net",
        "googlesyndication.com",
        "googleadservices.com",
        "google-analytics.com",
        "googletagmanager.com",
        "facebook.net",
        "connect.facebook.net",
        "analytics.twitter.com",
        "ads.linkedin.com",
        "bat.bing.com",
        "pixel.quantserve.com",
        "scorecardresearch.com",
        "taboola.com",
        "outbrain.com",
        "criteo.com",
        "adnxs.com",
        "rubiconproject.com",
        "pubmatic.com",
        "openx.net",
        "casalemedia.com",
        "amazon-adsystem.com",
    };
  }
};

MoltShield::MoltShield() : impl_(std::make_unique<Impl>()) {}
MoltShield::~MoltShield() = default;

bool MoltShield::Initialize(const std::string& data_dir) {
  impl_->data_dir = data_dir;
  impl_->InitializeDefaultBlocklist();
  impl_->initialized = true;
  return true;
}

bool MoltShield::ShouldBlockRequest(const std::string& request_url,
                                     const std::string& page_url,
                                     const std::string& resource_type) const {
  // Extract domain from request URL
  std::string domain;
  size_t proto_end = request_url.find("://");
  if (proto_end != std::string::npos) {
    domain = request_url.substr(proto_end + 3);
    size_t path_start = domain.find('/');
    if (path_start != std::string::npos) {
      domain = domain.substr(0, path_start);
    }
  }

  // Check against blocked domains
  // Also check parent domains (e.g., ads.example.com → example.com)
  std::string check_domain = domain;
  while (!check_domain.empty()) {
    if (impl_->blocked_domains.count(check_domain)) {
      return true;
    }
    size_t dot = check_domain.find('.');
    if (dot == std::string::npos) break;
    check_domain = check_domain.substr(dot + 1);
  }

  // TODO: Check against loaded EasyList filter rules

  return false;
}

std::vector<BlockedRequest> MoltShield::GetBlockedRequests(int tab_id) const {
  // TODO: Per-tab tracking of blocked requests
  return {};
}

ShieldStats MoltShield::GetStats(int tab_id) const {
  // TODO: Per-tab statistics
  ShieldStats stats = {};
  return stats;
}

bool MoltShield::LoadFilterList(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) return false;

  std::string line;
  while (std::getline(file, line)) {
    // Skip comments and empty lines
    if (line.empty() || line[0] == '!' || line[0] == '[') continue;

    impl_->filter_rules.push_back(line);

    // Extract domain blocks (||domain.com^)
    if (line.substr(0, 2) == "||") {
      size_t end = line.find('^');
      if (end != std::string::npos) {
        std::string domain = line.substr(2, end - 2);
        impl_->blocked_domains.insert(domain);
      }
    }
  }

  return true;
}

bool MoltShield::UpdateFilterLists() {
  // TODO: Download updated filter lists from:
  // - EasyList: https://easylist.to/easylist/easylist.txt
  // - EasyPrivacy: https://easylist.to/easylist/easyprivacy.txt
  // - Peter Lowe's: https://pgl.yoyo.org/adservers/serverlist.php
  return false;
}

void MoltShield::SetFingerprintProtection(FingerprintProtection level) {
  impl_->fp_level = level;
}

std::string MoltShield::GetCanvasNoiseScript() const {
  if (impl_->fp_level == FingerprintProtection::OFF) return "";

  // JavaScript that adds subtle noise to Canvas operations
  // to prevent Canvas fingerprinting
  return R"JS(
(function() {
  const origToDataURL = HTMLCanvasElement.prototype.toDataURL;
  const origGetImageData = CanvasRenderingContext2D.prototype.getImageData;

  HTMLCanvasElement.prototype.toDataURL = function() {
    const ctx = this.getContext('2d');
    if (ctx) {
      const imageData = origGetImageData.call(ctx, 0, 0, this.width, this.height);
      const data = imageData.data;
      // Add subtle noise to prevent fingerprinting
      for (let i = 0; i < data.length; i += 4) {
        data[i] = data[i] ^ (Math.random() > 0.99 ? 1 : 0);     // R
        data[i+1] = data[i+1] ^ (Math.random() > 0.99 ? 1 : 0); // G
        data[i+2] = data[i+2] ^ (Math.random() > 0.99 ? 1 : 0); // B
      }
      ctx.putImageData(imageData, 0, 0);
    }
    return origToDataURL.apply(this, arguments);
  };
})();
)JS";
}

std::string MoltShield::GetWebGLSpoofScript() const {
  if (impl_->fp_level == FingerprintProtection::OFF) return "";

  return R"JS(
(function() {
  const getParameter = WebGLRenderingContext.prototype.getParameter;
  WebGLRenderingContext.prototype.getParameter = function(parameter) {
    // Spoof WebGL renderer and vendor strings
    if (parameter === 37445) return 'Google Inc. (Generic)';  // UNMASKED_VENDOR
    if (parameter === 37446) return 'ANGLE (Generic, Generic GPU, OpenGL)';  // UNMASKED_RENDERER
    return getParameter.apply(this, arguments);
  };
})();
)JS";
}

std::string MoltShield::GetAudioShieldScript() const {
  if (impl_->fp_level == FingerprintProtection::OFF) return "";

  return R"JS(
(function() {
  const origCreateOscillator = AudioContext.prototype.createOscillator;
  const origCreateAnalyser = AudioContext.prototype.createAnalyser;

  // Add noise to AudioContext fingerprinting attempts
  const origGetFloatFrequencyData = AnalyserNode.prototype.getFloatFrequencyData;
  AnalyserNode.prototype.getFloatFrequencyData = function(array) {
    origGetFloatFrequencyData.call(this, array);
    for (let i = 0; i < array.length; i++) {
      array[i] += (Math.random() - 0.5) * 0.001;
    }
  };
})();
)JS";
}

std::string MoltShield::GenerateDeviceProfile() const {
  // Generate a randomized but realistic device profile
  // to prevent device fingerprinting
  static std::random_device rd;
  static std::mt19937 gen(rd());

  const std::vector<std::string> platforms = {
      "Win32", "MacIntel", "Linux x86_64"
  };
  const std::vector<int> screen_widths = {
      1920, 2560, 1440, 1366, 3840
  };
  const std::vector<int> screen_heights = {
      1080, 1440, 900, 768, 2160
  };
  const std::vector<int> color_depths = {24, 32};

  std::uniform_int_distribution<> plat_dist(0, platforms.size() - 1);
  std::uniform_int_distribution<> width_dist(0, screen_widths.size() - 1);
  std::uniform_int_distribution<> depth_dist(0, color_depths.size() - 1);

  int w_idx = width_dist(gen);

  std::ostringstream profile;
  profile << "{"
          << "\"platform\": \"" << platforms[plat_dist(gen)] << "\", "
          << "\"screenWidth\": " << screen_widths[w_idx] << ", "
          << "\"screenHeight\": " << screen_heights[w_idx] << ", "
          << "\"colorDepth\": " << color_depths[depth_dist(gen)] << ", "
          << "\"hardwareConcurrency\": " << (4 + (gen() % 13)) << ", "
          << "\"deviceMemory\": " << (4 * (1 + gen() % 4))
          << "}";

  return profile.str();
}

void MoltShield::SetCookiePolicy(CookiePolicy policy) {
  impl_->cookie_policy = policy;
}

bool MoltShield::ShouldAllowCookie(const std::string& cookie_domain,
                                    const std::string& page_domain,
                                    bool is_essential) const {
  switch (impl_->cookie_policy) {
    case CookiePolicy::ALLOW_ALL:
      return true;

    case CookiePolicy::BLOCK_THIRD_PARTY:
      // Allow first-party and essential cookies
      if (is_essential) return true;
      return cookie_domain.find(page_domain) != std::string::npos;

    case CookiePolicy::BLOCK_ALL_EXCEPT_ESSENTIAL:
      return is_essential;

    case CookiePolicy::BLOCK_ALL:
      return false;
  }

  return false;
}

void MoltShield::ClearTrackingCookies() {
  // TODO: Integrate with Chromium's CookieStore
  // Delete cookies from known tracking domains
}

std::string MoltShield::StripTrackingParams(const std::string& url) const {
  std::string result = url;

  for (const auto& param : impl_->tracking_params) {
    // Remove ?param=value or &param=value
    std::regex param_re("([?&])" + param + "=[^&]*");
    result = std::regex_replace(result, param_re, "$1");
  }

  // Clean up resulting URL (remove trailing ?, &, or ?&)
  result = std::regex_replace(result, std::regex("[?&]+$"), "");
  result = std::regex_replace(result, std::regex("\\?&"), "?");

  return result;
}

std::string MoltShield::SanitizeReferrer(
    const std::string& referrer,
    const std::string& target_url) const {
  // For cross-origin requests, only send origin (not full path)
  // to prevent referrer leaking

  std::string ref_domain, target_domain;

  auto extract_domain = [](const std::string& url) -> std::string {
    size_t proto_end = url.find("://");
    if (proto_end == std::string::npos) return url;
    size_t domain_start = proto_end + 3;
    size_t path_start = url.find('/', domain_start);
    if (path_start == std::string::npos) return url.substr(domain_start);
    return url.substr(domain_start, path_start - domain_start);
  };

  ref_domain = extract_domain(referrer);
  target_domain = extract_domain(target_url);

  if (ref_domain == target_domain) {
    return referrer;  // Same origin, send full referrer
  }

  // Cross-origin: send only the origin
  size_t proto_end = referrer.find("://");
  if (proto_end != std::string::npos) {
    size_t path_start = referrer.find('/', proto_end + 3);
    if (path_start != std::string::npos) {
      return referrer.substr(0, path_start) + "/";
    }
  }

  return referrer;
}

}  // namespace moltshield
