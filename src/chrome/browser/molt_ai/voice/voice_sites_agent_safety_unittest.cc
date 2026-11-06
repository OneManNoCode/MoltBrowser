// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Every URL voice mode builds passes agent_safety's navigation check and
// goes to the host it should: known sites' home and search pages with any
// query, and spoken addresses.

#include <optional>
#include <string>

#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/voice/voice_sites.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace voice {
namespace {

const char* const kQueries[] = {
    "Bohemian Rhapsody",
    "",
    "a&b=c#d?e",
    "javascript:alert(1)",
    "https://evil.example/",
    "@evil.example",
    "\\\\evil.example",
    "%00%2F..%2F",
    "\xE2\x80\xAEgnp.exe",
    "Beyonc\xC3\xA9 / AC/DC",
};

TEST(VoiceSitesAgentSafetyTest, KnownSitesPassTheNavigationCheck) {
  for (const KnownSite& site : KnownSites()) {
    SCOPED_TRACE(site.name);
    const GURL home{std::string(site.home_url)};
    EXPECT_TRUE(agent_safety::IsAllowedNavigationURL(home));
    // Its pages are its own; one without domains of its own lives on a
    // known site's (Google Maps on Google's).
    const KnownSite* owner = site.domains.empty() ? KnownSiteForHost(home.host())
                                                  : &site;
    ASSERT_TRUE(owner);
    EXPECT_EQ(KnownSiteForHost(home.host()), owner);
    if (site.search_url.empty()) {
      continue;
    }
    for (const char* query : kQueries) {
      SCOPED_TRACE(query);
      const GURL search(SiteSearchURL(site, query));
      EXPECT_TRUE(agent_safety::IsAllowedNavigationURL(search));
      EXPECT_EQ(KnownSiteForHost(search.host()), owner);
    }
  }
}

TEST(VoiceSitesAgentSafetyTest, AddressesPassTheNavigationCheck) {
  struct Case {
    const char* address;
    const char* host;
  } kCases[] = {
      {"example.com", "example.com"},
      {"www.bbc.co.uk/news", "www.bbc.co.uk"},
      {"192.168.1.1", "192.168.1.1"},
      {"a-b.example.org/x_y/z.html", "a-b.example.org"},
  };
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.address);
    const std::optional<std::string> url = AddressURL(c.address);
    ASSERT_TRUE(url);
    const GURL gurl(*url);
    EXPECT_TRUE(agent_safety::IsAllowedNavigationURL(gurl));
    EXPECT_EQ(gurl.host(), c.host);
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
