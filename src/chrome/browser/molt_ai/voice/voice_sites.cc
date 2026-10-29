// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_sites.h"

#include <cstdint>
#include <utility>

namespace molt_ai {
namespace voice {

namespace {

// A host name's limits (RFC 1035).
constexpr size_t kMaxHostBytes = 253;
constexpr size_t kMaxLabelBytes = 63;
// An address's path, in bytes.
constexpr size_t kMaxAddressPathBytes = 512;

char LowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = LowerAscii(c);
  }
  return out;
}

bool IsLetter(char c) {
  return c >= 'a' && c <= 'z';
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

// Whether |host| (lower case) is a host name: dot-separated labels of
// letters, digits and inner hyphens, a top-level domain of at least two
// letters; or four decimal numbers 0-255.
bool IsHostName(std::string_view host) {
  if (host.empty() || host.size() > kMaxHostBytes) {
    return false;
  }
  std::vector<std::string_view> labels;
  size_t start = 0;
  for (;;) {
    const size_t dot = host.find('.', start);
    labels.push_back(host.substr(start, dot == std::string_view::npos
                                            ? std::string_view::npos
                                            : dot - start));
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  if (labels.size() < 2) {
    return false;
  }
  bool all_numeric = true;
  for (std::string_view label : labels) {
    if (label.empty() || label.size() > kMaxLabelBytes ||
        label.front() == '-' || label.back() == '-') {
      return false;
    }
    for (char c : label) {
      if (!IsLetter(c) && !IsDigit(c) && c != '-') {
        return false;
      }
      if (!IsDigit(c)) {
        all_numeric = false;
      }
    }
  }
  if (all_numeric) {
    if (labels.size() != 4) {
      return false;
    }
    for (std::string_view label : labels) {
      if (label.size() > 3 || (label.size() > 1 && label.front() == '0')) {
        return false;
      }
      int value = 0;
      for (char c : label) {
        value = value * 10 + (c - '0');
      }
      if (value > 255) {
        return false;
      }
    }
    return true;
  }
  const std::string_view tld = labels.back();
  if (tld.size() < 2) {
    return false;
  }
  for (char c : tld) {
    if (!IsLetter(c)) {
      return false;
    }
  }
  return true;
}

// Whether |path| (after the host, starting with '/') holds only what a
// spoken address can: letters, digits and "-._/".
bool IsAddressPath(std::string_view path) {
  if (path.size() > kMaxAddressPathBytes) {
    return false;
  }
  for (char c : path) {
    if (!IsLetter(c) && !IsDigit(c) && c != '-' && c != '.' && c != '_' &&
        c != '/') {
      return false;
    }
  }
  return true;
}

bool HostIsOrIsUnder(std::string_view host, std::string_view domain) {
  if (host == domain) {
    return true;
  }
  return host.size() > domain.size() &&
         host.substr(host.size() - domain.size()) == domain &&
         host[host.size() - domain.size() - 1] == '.';
}

// ScopedTestSite's site, or null.
const KnownSite*& TestSite() {
  static const KnownSite* site = nullptr;
  return site;
}

bool HasName(const KnownSite& known, std::string_view folded) {
  for (std::string_view name : known.names) {
    if (folded == name) {
      return true;
    }
  }
  return false;
}

bool ServesHost(const KnownSite& known, std::string_view folded) {
  for (std::string_view domain : known.domains) {
    if (HostIsOrIsUnder(folded, domain)) {
      return true;
    }
  }
  return false;
}

}  // namespace

const std::vector<KnownSite>& KnownSites() {
  // The four sites of the design's Phase 1 (§10), the engine the web agent
  // already uses (agent_actions.h kSearchURLPrefix), and the other major
  // engine. Search pages take the query as the last parameter.
  static const std::vector<KnownSite>* const sites =
      new std::vector<KnownSite>{
          {"YouTube",
           "https://www.youtube.com/",
           "https://www.youtube.com/results?search_query=",
           {"youtube", "youtubecom"},
           {"youtube.com"}},
          {"Google",
           "https://www.google.com/",
           "https://www.google.com/search?q=",
           {"google", "googlesearch", "googlecom"},
           {"google.com"}},
          {"DuckDuckGo",
           "https://duckduckgo.com/",
           "https://duckduckgo.com/?q=",
           {"duckduckgo", "duckduck", "ddg"},
           {"duckduckgo.com"}},
          {"Wikipedia",
           "https://en.wikipedia.org/",
           "https://en.wikipedia.org/w/index.php?search=",
           {"wikipedia", "wiki", "wikipediaorg"},
           {"wikipedia.org"}},
          {"Brave Search",
           "https://search.brave.com/",
           "https://search.brave.com/search?q=",
           {"bravesearch", "brave"},
           {"search.brave.com"}},
          {"Bing",
           "https://www.bing.com/",
           "https://www.bing.com/search?q=",
           {"bing", "bingcom", "bingsearch"},
           {"bing.com"}},
      };
  return *sites;
}

const KnownSite* FindKnownSite(std::string_view site) {
  const std::string folded = LowerAscii(site);
  if (folded.empty()) {
    return nullptr;
  }
  if (TestSite() && HasName(*TestSite(), folded)) {
    return TestSite();
  }
  for (const KnownSite& known : KnownSites()) {
    if (HasName(known, folded)) {
      return &known;
    }
  }
  return nullptr;
}

const KnownSite* KnownSiteForHost(std::string_view host) {
  std::string folded = LowerAscii(host.substr(0, kMaxHostBytes + 1));
  // A fully qualified name ("youtube.com.") is the same host.
  if (!folded.empty() && folded.back() == '.') {
    folded.pop_back();
  }
  if (folded.empty()) {
    return nullptr;
  }
  if (TestSite() && ServesHost(*TestSite(), folded)) {
    return TestSite();
  }
  for (const KnownSite& known : KnownSites()) {
    if (ServesHost(known, folded)) {
      return &known;
    }
  }
  return nullptr;
}

std::string FormEncode(std::string_view query) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(query.size() * 3);
  for (char ch : query) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out.push_back(ch);
    } else if (c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

std::string SiteSearchURL(const KnownSite& site, std::string_view query) {
  return std::string(site.search_url) + FormEncode(query);
}

std::optional<std::string> AddressURL(std::string_view address) {
  if (address.empty() || address.size() > kMaxHostBytes + kMaxAddressPathBytes) {
    return std::nullopt;
  }
  const std::string lower = LowerAscii(address);
  const size_t slash = lower.find('/');
  std::string_view host(lower);
  std::string_view path;
  if (slash != std::string::npos) {
    host = std::string_view(lower).substr(0, slash);
    path = std::string_view(lower).substr(slash);
  }
  if (!IsHostName(host) || !IsAddressPath(path)) {
    return std::nullopt;
  }
  std::string url = "https://";
  url.append(host);
  url.append(path.empty() ? std::string_view("/") : path);
  return url;
}

SiteTarget ResolveSpokenSite(std::string_view site,
                             bool site_is_address,
                             std::string_view site_words) {
  SiteTarget target;
  if (!site_is_address) {
    if (const KnownSite* known = FindKnownSite(site)) {
      target.kind = SiteTarget::Kind::kKnownSite;
      target.site = known;
      target.url = std::string(known->home_url);
      target.name = std::string(known->name);
      return target;
    }
  } else if (std::optional<std::string> url = AddressURL(site)) {
    target.kind = SiteTarget::Kind::kAddress;
    target.url = std::move(*url);
    target.name = LowerAscii(site);
    return target;
  }
  target.kind = SiteTarget::Kind::kUnknown;
  target.name = std::string(site_words.empty() ? site : site_words);
  return target;
}

ScopedTestSite::ScopedTestSite(KnownSite site) : site_(std::move(site)) {
  TestSite() = &site_;
}

ScopedTestSite::~ScopedTestSite() {
  TestSite() = nullptr;
}

}  // namespace voice
}  // namespace molt_ai
