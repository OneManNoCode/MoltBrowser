// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield.h"

#include <algorithm>
#include <regex>
#include <sstream>

namespace molt_ai {

// Known tracker domains (built-in blocklist)
static const std::vector<std::string> kDefaultBlocklist = {
    // Ad networks
    "doubleclick.net",
    "googlesyndication.com",
    "googleadservices.com",
    "adservice.google.com",
    "pagead2.googlesyndication.com",

    // Analytics trackers
    "google-analytics.com",
    "googletagmanager.com",
    "analytics.google.com",
    "hotjar.com",
    "mixpanel.com",
    "amplitude.com",
    "segment.io",
    "segment.com",

    // Social media trackers
    "connect.facebook.net",
    "pixel.facebook.com",
    "platform.twitter.com",
    "ads-twitter.com",
    "ads.linkedin.com",
    "snap.licdn.com",

    // Other trackers
    "scorecardresearch.com",
    "quantserve.com",
    "outbrain.com",
    "taboola.com",
    "criteo.com",
    "criteo.net",
    "moatads.com",
    "newrelic.com",
    "nr-data.net",
    "optimizely.com",
    "adobedtm.com",
    "demdex.net",
    "omtrdc.net",
    "bat.bing.com",
    "clarity.ms",
};

// Tracking URL parameters to strip
static const std::vector<std::string> kTrackingParams = {
    "utm_source", "utm_medium", "utm_campaign", "utm_term", "utm_content",
    "utm_id", "utm_cid",
    "fbclid", "gclid", "gclsrc", "dclid", "gbraid", "wbraid",
    "msclkid", "twclid",
    "mc_eid", "mc_cid",
    "_hsenc", "_hsmi", "hsa_cam", "hsa_grp", "hsa_mt", "hsa_src",
    "hsa_ad", "hsa_acc", "hsa_net", "hsa_ver", "hsa_la", "hsa_ol",
    "hsa_kw", "hsa_tgt",
    "ref", "ref_src",
    "igshid",
    "yclid",
    "wickedid",
};

struct MoltShield::Impl {
  ProtectionLevel level = ProtectionLevel::STANDARD;
  CookiePolicy cookie_policy = CookiePolicy::BLOCK_THIRD_PARTY;
  std::unordered_set<std::string> blocklist;
  std::unordered_set<std::string> allowlist;
  ShieldStats stats;
  bool initialized = false;
};

MoltShield::MoltShield() : impl_(std::make_unique<Impl>()) {}
MoltShield::~MoltShield() = default;

bool MoltShield::Initialize(ProtectionLevel level) {
  impl_->level = level;

  // Load default blocklist
  for (const auto& domain : kDefaultBlocklist) {
    impl_->blocklist.insert(domain);
  }

  // Set cookie policy based on protection level
  switch (level) {
    case ProtectionLevel::OFF:
      impl_->cookie_policy = CookiePolicy::ALLOW_ALL;
      break;
    case ProtectionLevel::STANDARD:
      impl_->cookie_policy = CookiePolicy::BLOCK_THIRD_PARTY;
      break;
    case ProtectionLevel::STRICT:
      impl_->cookie_policy = CookiePolicy::BLOCK_THIRD_PARTY;
      break;
    case ProtectionLevel::AGGRESSIVE:
      impl_->cookie_policy = CookiePolicy::BLOCK_ALL_EXCEPT_ESSENTIAL;
      break;
  }

  impl_->initialized = true;
  return true;
}

void MoltShield::SetProtectionLevel(ProtectionLevel level) {
  impl_->level = level;
}

ProtectionLevel MoltShield::GetProtectionLevel() const {
  return impl_->level;
}

bool MoltShield::ShouldBlockRequest(const std::string& url,
                                     const std::string& page_url) const {
  if (impl_->level == ProtectionLevel::OFF) return false;

  // Extract domain from URL
  std::string domain;
  size_t start = url.find("://");
  if (start != std::string::npos) {
    start += 3;
    size_t end = url.find('/', start);
    domain = url.substr(start, end - start);
  }

  // Check allowlist first
  if (impl_->allowlist.count(domain) > 0) return false;

  // Check blocklist
  for (const auto& blocked : impl_->blocklist) {
    if (domain.find(blocked) != std::string::npos) {
      const_cast<ShieldStats&>(impl_->stats).trackers_blocked++;
      return true;
    }
  }

  return false;
}

std::string MoltShield::StripTrackingParams(const std::string& url) const {
  if (impl_->level == ProtectionLevel::OFF) return url;

  size_t query_start = url.find('?');
  if (query_start == std::string::npos) return url;

  std::string base = url.substr(0, query_start);
  std::string query = url.substr(query_start + 1);

  // Split query params
  std::stringstream ss(query);
  std::string param;
  std::vector<std::string> clean_params;

  while (std::getline(ss, param, '&')) {
    std::string key = param.substr(0, param.find('='));

    // Check if this is a tracking parameter
    bool is_tracking = false;
    for (const auto& tracking_param : kTrackingParams) {
      if (key == tracking_param) {
        is_tracking = true;
        const_cast<ShieldStats&>(impl_->stats).tracking_params_stripped++;
        break;
      }
    }

    if (!is_tracking) {
      clean_params.push_back(param);
    }
  }

  if (clean_params.empty()) return base;

  std::string result = base + "?";
  for (size_t i = 0; i < clean_params.size(); ++i) {
    if (i > 0) result += "&";
    result += clean_params[i];
  }
  return result;
}

bool MoltShield::ShouldUpgradeToHttps(const std::string& url) const {
  if (impl_->level == ProtectionLevel::OFF) return false;
  if (impl_->level == ProtectionLevel::AGGRESSIVE) {
    return url.substr(0, 5) == "http:" && url.substr(0, 6) != "https:";
  }
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
    case CookiePolicy::ALLOW_ALL:
      return false;
    case CookiePolicy::BLOCK_THIRD_PARTY:
      if (is_third_party) {
        const_cast<ShieldStats&>(impl_->stats).cookies_blocked++;
        return true;
      }
      return false;
    case CookiePolicy::BLOCK_ALL_EXCEPT_ESSENTIAL:
      if (is_third_party || domain != page_domain) {
        const_cast<ShieldStats&>(impl_->stats).cookies_blocked++;
        return true;
      }
      return false;
    case CookiePolicy::BLOCK_ALL:
      const_cast<ShieldStats&>(impl_->stats).cookies_blocked++;
      return true;
  }
  return false;
}

std::string MoltShield::GetCanvasProtectionJS() const {
  if (impl_->level < ProtectionLevel::STRICT) return "";
  return R"JS(
    (function() {
      const origToDataURL = HTMLCanvasElement.prototype.toDataURL;
      HTMLCanvasElement.prototype.toDataURL = function() {
        const ctx = this.getContext('2d');
        if (ctx) {
          const imgData = ctx.getImageData(0, 0, this.width, this.height);
          for (let i = 0; i < imgData.data.length; i += 4) {
            imgData.data[i] ^= (Math.random() * 2) | 0;
          }
          ctx.putImageData(imgData, 0, 0);
        }
        return origToDataURL.apply(this, arguments);
      };
    })();
  )JS";
}

std::string MoltShield::GetWebGLProtectionJS() const {
  if (impl_->level < ProtectionLevel::STRICT) return "";
  return R"JS(
    (function() {
      const origGetParameter = WebGLRenderingContext.prototype.getParameter;
      WebGLRenderingContext.prototype.getParameter = function(param) {
        if (param === 0x1F00) return 'MoltBrowser WebGL';
        if (param === 0x1F01) return 'MoltBrowser GPU';
        return origGetParameter.apply(this, arguments);
      };
    })();
  )JS";
}

std::string MoltShield::GetAudioProtectionJS() const {
  if (impl_->level < ProtectionLevel::STRICT) return "";
  return R"JS(
    (function() {
      const origCreateOscillator = AudioContext.prototype.createOscillator;
      AudioContext.prototype.createOscillator = function() {
        const osc = origCreateOscillator.apply(this, arguments);
        osc.frequency.value += (Math.random() - 0.5) * 0.01;
        return osc;
      };
    })();
  )JS";
}

std::string MoltShield::SanitizeReferrer(
    const std::string& referrer_url,
    const std::string& destination_url) const {
  if (impl_->level == ProtectionLevel::OFF) return referrer_url;

  // Extract origins for comparison
  auto extractOrigin = [](const std::string& url) -> std::string {
    size_t pos = url.find("://");
    if (pos == std::string::npos) return "";
    pos = url.find('/', pos + 3);
    return (pos != std::string::npos) ? url.substr(0, pos) : url;
  };

  std::string referrer_origin = extractOrigin(referrer_url);
  std::string dest_origin = extractOrigin(destination_url);

  // Same origin: allow full referrer
  if (referrer_origin == dest_origin) return referrer_url;

  // Cross-origin: strip to origin only (no path)
  if (impl_->level >= ProtectionLevel::STRICT) {
    return referrer_origin + "/";
  }

  return referrer_url;
}

void MoltShield::AddToAllowlist(const std::string& domain) {
  impl_->allowlist.insert(domain);
}

void MoltShield::RemoveFromAllowlist(const std::string& domain) {
  impl_->allowlist.erase(domain);
}

bool MoltShield::IsAllowlisted(const std::string& domain) const {
  return impl_->allowlist.count(domain) > 0;
}

ShieldStats MoltShield::GetStats() const {
  return impl_->stats;
}

void MoltShield::ResetStats() {
  impl_->stats = ShieldStats();
}

}  // namespace molt_ai
