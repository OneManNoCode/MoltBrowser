// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_sites.h"

#include <cstddef>
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

// The length of the longest of |known|'s domains that |folded| is or is
// under, or 0 when it serves none of them.
size_t ServedDomainLength(const KnownSite& known, std::string_view folded) {
  size_t longest = 0;
  for (std::string_view domain : known.domains) {
    if (domain.size() > longest && HostIsOrIsUnder(folded, domain)) {
      longest = domain.size();
    }
  }
  return longest;
}

}  // namespace

const std::vector<KnownSite>& KnownSites() {
  // The four sites of the design's Phase 1 (§10), the engine the web agent
  // already uses (agent_actions.h kSearchURLPrefix), the other major
  // engine, then popular sites. Search pages take the query as the last
  // parameter. YouTube's "youtwo" is "you two", the name as the recognizer
  // hears it over music (the spoken suite's music runs).
  static const std::vector<KnownSite>* const sites = new std::vector<KnownSite>{
      {"YouTube",
       "https://www.youtube.com/",
       "https://www.youtube.com/results?search_query=",
       {"youtube", "youtubecom", "youtwo"},
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
      // Popular sites by their official addresses, so that "open
      // Facebook" goes to facebook.com and never to a look-alike. An
      // empty search page: "search <site> for ..." searches the web.
      {"Facebook",
       "https://www.facebook.com/",
       "https://www.facebook.com/search/top/?q=",
       {"facebook", "facebookcom", "fb"},
       {"facebook.com"}},
      {"Instagram",
       "https://www.instagram.com/",
       "",
       {"instagram", "instagramcom", "insta"},
       {"instagram.com"}},
      {"WhatsApp",
       "https://web.whatsapp.com/",
       "",
       {"whatsapp", "whatsappweb", "whatsup"},
       {"whatsapp.com"}},
      {"Messenger",
       "https://www.messenger.com/",
       "",
       {"messenger", "facebookmessenger"},
       {"messenger.com"}},
      {"X",
       "https://x.com/",
       "https://x.com/search?q=",
       {"x", "xcom", "twitter", "twittercom"},
       {"x.com", "twitter.com"}},
      {"LinkedIn",
       "https://www.linkedin.com/",
       "https://www.linkedin.com/search/results/all/?keywords=",
       {"linkedin", "linkedincom"},
       {"linkedin.com"}},
      {"Reddit",
       "https://www.reddit.com/",
       "https://www.reddit.com/search/?q=",
       {"reddit", "redditcom"},
       {"reddit.com"}},
      {"TikTok",
       "https://www.tiktok.com/",
       "https://www.tiktok.com/search?q=",
       {"tiktok", "ticktock", "tiktokcom"},
       {"tiktok.com"}},
      {"Pinterest",
       "https://www.pinterest.com/",
       "https://www.pinterest.com/search/pins/?q=",
       {"pinterest", "pinterestcom"},
       {"pinterest.com"}},
      {"Amazon",
       "https://www.amazon.com/",
       "https://www.amazon.com/s?k=",
       {"amazon", "amazoncom"},
       {"amazon.com"}},
      {"eBay",
       "https://www.ebay.com/",
       "https://www.ebay.com/sch/i.html?_nkw=",
       {"ebay", "ebaycom"},
       {"ebay.com"}},
      {"Walmart",
       "https://www.walmart.com/",
       "https://www.walmart.com/search?q=",
       {"walmart", "walmartcom"},
       {"walmart.com"}},
      {"Etsy",
       "https://www.etsy.com/",
       "https://www.etsy.com/search?q=",
       {"etsy", "etsycom"},
       {"etsy.com"}},
      {"Netflix",
       "https://www.netflix.com/",
       "https://www.netflix.com/search?q=",
       {"netflix", "netflixcom"},
       {"netflix.com"}},
      {"Spotify",
       "https://open.spotify.com/",
       "",
       {"spotify", "spotifycom"},
       {"spotify.com"}},
      {"Twitch",
       "https://www.twitch.tv/",
       "https://www.twitch.tv/search?term=",
       {"twitch", "twitchtv"},
       {"twitch.tv"}},
      {"Disney+",
       "https://www.disneyplus.com/",
       "",
       {"disneyplus", "disney+"},
       {"disneyplus.com"}},
      {"Prime Video",
       "https://www.primevideo.com/",
       "",
       {"primevideo", "amazonprime", "amazonprimevideo"},
       {"primevideo.com"}},
      {"Gmail",
       "https://mail.google.com/",
       "",
       {"gmail", "googlemail", "gmailcom"},
       {"mail.google.com"}},
      {"Outlook",
       "https://outlook.live.com/",
       "",
       {"outlook", "hotmail", "outlookcom"},
       {"outlook.live.com", "outlook.com"}},
      {"Yahoo",
       "https://www.yahoo.com/",
       "https://search.yahoo.com/search?p=",
       {"yahoo", "yahoocom"},
       {"yahoo.com"}},
      {"Google Maps",
       "https://www.google.com/maps",
       "https://www.google.com/maps/search/?api=1&query=",
       {"googlemaps", "maps"},
       {}},
      {"BBC",
       "https://www.bbc.com/",
       "https://www.bbc.co.uk/search?q=",
       {"bbc", "bbcnews", "bbccom"},
       {"bbc.com", "bbc.co.uk"}},
      {"CNN",
       "https://www.cnn.com/",
       "https://www.cnn.com/search?q=",
       {"cnn", "cnncom"},
       {"cnn.com"}},
      {"The New York Times",
       "https://www.nytimes.com/",
       "https://www.nytimes.com/search?query=",
       {"newyorktimes", "nytimes", "nyt"},
       {"nytimes.com"}},
      {"The Guardian",
       "https://www.theguardian.com/",
       "",
       {"guardian", "theguardian"},
       {"theguardian.com"}},
      {"ESPN", "https://www.espn.com/", "", {"espn", "espncom"}, {"espn.com"}},
      {"The Weather Channel",
       "https://weather.com/",
       "",
       {"weather", "weathercom", "weatherchannel", "theweatherchannel"},
       {"weather.com"}},
      {"IMDb",
       "https://www.imdb.com/",
       "https://www.imdb.com/find/?q=",
       {"imdb", "imdbcom"},
       {"imdb.com"}},
      {"GitHub",
       "https://github.com/",
       "https://github.com/search?q=",
       {"github", "githubcom"},
       {"github.com"}},
      {"PayPal",
       "https://www.paypal.com/",
       "",
       {"paypal", "paypalcom"},
       {"paypal.com"}},
      {"Zoom", "https://zoom.us/", "", {"zoom", "zoomus"}, {"zoom.us"}},
      {"Apple",
       "https://www.apple.com/",
       "",
       {"apple", "applecom"},
       {"apple.com"}},
      {"Microsoft",
       "https://www.microsoft.com/",
       "",
       {"microsoft", "microsoftcom"},
       {"microsoft.com"}},
      {"ChatGPT",
       "https://chatgpt.com/",
       "",
       {"chatgpt", "chatgptcom"},
       {"chatgpt.com"}},
      {"Claude",
       "https://claude.ai/",
       "",
       {"claude", "claudeai"},
       {"claude.ai"}},
      {"Airbnb",
       "https://www.airbnb.com/",
       "",
       {"airbnb", "airbnbcom"},
       {"airbnb.com"}},
      {"Craigslist",
       "https://www.craigslist.org/",
       "",
       {"craigslist", "craigslistorg"},
       {"craigslist.org"}},
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
  if (TestSite() && ServedDomainLength(*TestSite(), folded) > 0) {
    return TestSite();
  }
  // The most specific: mail.google.com is Gmail's, not Google's.
  const KnownSite* best = nullptr;
  size_t best_length = 0;
  for (const KnownSite& known : KnownSites()) {
    const size_t length = ServedDomainLength(known, folded);
    if (length > best_length) {
      best = &known;
      best_length = length;
    }
  }
  return best;
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
  if (address.empty() ||
      address.size() > kMaxHostBytes + kMaxAddressPathBytes) {
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
