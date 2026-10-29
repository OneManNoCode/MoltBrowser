// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Where "open YouTube", "open example dot com" and "search Wikipedia for
// Queen" go (docs/design/TOUCHLESS_BROWSING.md §2, §6.2, §6.7).
//
// The grammar (voice_command.h) gives a site as host-like text: a name
// ("youtube", "duckduckgo") or a spoken address ("example.com",
// "bbc.co.uk/news"). This file turns it into somewhere to go:
//   - a site of the small built-in table below, by one of its names: its
//     home page, and its search page for "search YouTube for ...";
//   - an address: "https://" + the address, when it reads as a host name
//     (labels of letters, digits and hyphens, a top-level domain of
//     letters, or an IPv4 address) and an optional path;
//   - anything else is a name the browser doesn't know. It is never made
//     into a domain ("open foo" never goes to foo.com): the browser
//     searches for it with the default search engine and says so.
//
// Every URL built here starts with "https://", has a checked host, and
// the user's words only ever appear form-encoded after the search page's
// "?...=" (letters, digits and "-_.~" as they are, '+' for a space, %XX
// for every other byte). The browser still checks each URL with
// agent_safety::IsAllowedNavigationURL() before loading it
// (voice_sites_agent_safety_unittest.cc checks every one of them here).
//
// Std-only (no //base): unit-tested in molt_ai_unittests and built by the
// standalone harness.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SITES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SITES_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"

namespace molt_ai {
namespace voice {

// A site the browser knows by name.
struct KnownSite {
  // As the browser says it: "YouTube".
  std::string_view name;
  // Its home page.
  std::string_view home_url;
  // Its search page; the form-encoded query is appended.
  std::string_view search_url;
  // The names the grammar may give it (folded, no spaces): "youtube".
  std::vector<std::string_view> names;
  // Its pages are on these hosts or their subdomains: "youtube.com".
  std::vector<std::string_view> domains;
};

// The built-in sites: YouTube, Google, DuckDuckGo, Wikipedia, Brave
// Search (the web agent's engine) and Bing. Kept for the life of the
// process.
const std::vector<KnownSite>& KnownSites();

// The known site |site| names (the grammar's host-like text, any case),
// or null.
const KnownSite* FindKnownSite(std::string_view site);

// The known site whose pages |host| serves ("www.youtube.com",
// "m.youtube.com" -> YouTube), or null. |host| is compared in ASCII lower
// case.
const KnownSite* KnownSiteForHost(std::string_view host);

// |query| form-encoded for a URL's query string: ASCII letters, digits and
// "-_.~" stay, a space becomes '+', every other byte is %XX.
std::string FormEncode(std::string_view query);

// |site|'s search page for |query|.
std::string SiteSearchURL(const KnownSite& site, std::string_view query);

// A spoken address ("example.com", "www.bbc.co.uk/news") as an https URL,
// or nullopt when it doesn't read as a host name and path.
std::optional<std::string> AddressURL(std::string_view address);

// What the user named.
struct SiteTarget {
  enum class Kind {
    kKnownSite,  // |site|, |url| is its home page
    kAddress,    // |url| is "https://" + the address
    kUnknown,    // nothing to open: search for |name| instead
  };
  Kind kind = Kind::kUnknown;
  // Points into KnownSites()' static table (or a test's site).
  MOLT_RAW_PTR_EXCLUSION const KnownSite* site = nullptr;
  std::string url;
  // What to call it when speaking: the known site's name, the address, or
  // the words the user said.
  std::string name;
};

// |site| and |site_is_address| as the grammar wrote them
// (VoiceCommand::site), |site_words| as said ("the New York Times").
SiteTarget ResolveSpokenSite(std::string_view site,
                             bool site_is_address,
                             std::string_view site_words);

// Test-only: while it lives, FindKnownSite() and KnownSiteForHost() also
// know |site| (looked at before the built-in sites), so a browser test can
// serve a fixture site under a name ("open VideoSite"). The strings |site|
// views must outlive it. One at a time; not for production code.
class ScopedTestSite {
 public:
  explicit ScopedTestSite(KnownSite site);
  ScopedTestSite(const ScopedTestSite&) = delete;
  ScopedTestSite& operator=(const ScopedTestSite&) = delete;
  ~ScopedTestSite();

 private:
  KnownSite site_;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SITES_H_
