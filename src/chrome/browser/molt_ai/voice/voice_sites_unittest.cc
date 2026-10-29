// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_sites.h"

#include <string>

#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(VoiceSitesTest, KnownSitesByTheGrammarsNames) {
  struct Case {
    const char* site;
    const char* name;
  } kCases[] = {
      {"youtube", "YouTube"},       {"YouTube", "YouTube"},
      {"google", "Google"},         {"duckduckgo", "DuckDuckGo"},
      {"wikipedia", "Wikipedia"},   {"wiki", "Wikipedia"},
      {"bravesearch", "Brave Search"}, {"brave", "Brave Search"},
      {"bing", "Bing"},
  };
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.site);
    const KnownSite* site = FindKnownSite(c.site);
    ASSERT_TRUE(site);
    EXPECT_EQ(site->name, c.name);
  }
  EXPECT_FALSE(FindKnownSite(""));
  EXPECT_FALSE(FindKnownSite("netflix"));
  EXPECT_FALSE(FindKnownSite("youtube.com.evil"));
  EXPECT_FALSE(FindKnownSite("you"));
}

TEST(VoiceSitesTest, EveryKnownSiteIsHttpsWithASearchPage) {
  for (const KnownSite& site : KnownSites()) {
    SCOPED_TRACE(site.name);
    EXPECT_EQ(site.home_url.substr(0, 8), "https://");
    EXPECT_EQ(site.search_url.substr(0, 8), "https://");
    EXPECT_NE(site.search_url.find('?'), std::string_view::npos);
    EXPECT_EQ(site.search_url.back(), '=');
    EXPECT_FALSE(site.names.empty());
    EXPECT_FALSE(site.domains.empty());
    // Its own pages are recognized.
    const std::string home(site.home_url);
    const std::string host = home.substr(8, home.find('/', 8) - 8);
    EXPECT_EQ(KnownSiteForHost(host), &site);
    // Each name the grammar may write finds it.
    for (std::string_view name : site.names) {
      EXPECT_EQ(FindKnownSite(name), &site);
    }
  }
}

TEST(VoiceSitesTest, TheGrammarsSiteForOpenYouTubeIsKnown) {
  ParseContext context;
  for (const char* said : {"open YouTube", "go to you tube", "open Wikipedia",
                           "take me to duck duck go", "open bing"}) {
    SCOPED_TRACE(said);
    const VoiceCommand command = ParseVoiceCommand(said, context);
    ASSERT_EQ(command.kind, VoiceCommandKind::kOpenSite);
    EXPECT_TRUE(FindKnownSite(command.site)) << command.site;
  }
}

TEST(VoiceSitesTest, SitesForHosts) {
  EXPECT_EQ(KnownSiteForHost("www.youtube.com")->name, "YouTube");
  EXPECT_EQ(KnownSiteForHost("m.youtube.com")->name, "YouTube");
  EXPECT_EQ(KnownSiteForHost("YouTube.com")->name, "YouTube");
  EXPECT_EQ(KnownSiteForHost("youtube.com.")->name, "YouTube");
  EXPECT_EQ(KnownSiteForHost("de.wikipedia.org")->name, "Wikipedia");
  EXPECT_EQ(KnownSiteForHost("search.brave.com")->name, "Brave Search");
  EXPECT_FALSE(KnownSiteForHost("brave.com"));
  EXPECT_FALSE(KnownSiteForHost("notyoutube.com"));
  EXPECT_FALSE(KnownSiteForHost("youtube.com.example.net"));
  EXPECT_FALSE(KnownSiteForHost(""));
  EXPECT_FALSE(KnownSiteForHost("video.test"));
}

TEST(VoiceSitesTest, SearchURLsEncodeTheQuery) {
  const KnownSite& youtube = *FindKnownSite("youtube");
  EXPECT_EQ(SiteSearchURL(youtube, "Bohemian Rhapsody"),
            "https://www.youtube.com/results?search_query=Bohemian+Rhapsody");
  EXPECT_EQ(SiteSearchURL(*FindKnownSite("wikipedia"), "AC/DC"),
            "https://en.wikipedia.org/w/index.php?search=AC%2FDC");
  EXPECT_EQ(SiteSearchURL(*FindKnownSite("google"), "node.js tips~"),
            "https://www.google.com/search?q=node.js+tips~");
  // Nothing in a query can end the parameter or the URL.
  EXPECT_EQ(FormEncode("a&b=c#d?e%f+g"), "a%26b%3Dc%23d%3Fe%25f%2Bg");
  EXPECT_EQ(FormEncode("\n\r\t\x7F"), "%0A%0D%09%7F");
  EXPECT_EQ(FormEncode("Beyonc\xC3\xA9"), "Beyonc%C3%A9");
  EXPECT_EQ(FormEncode("javascript:alert(1)"), "javascript%3Aalert%281%29");
  EXPECT_EQ(FormEncode(""), "");
}

TEST(VoiceSitesTest, SpokenAddresses) {
  EXPECT_EQ(AddressURL("example.com"), "https://example.com/");
  EXPECT_EQ(AddressURL("www.bbc.co.uk"), "https://www.bbc.co.uk/");
  EXPECT_EQ(AddressURL("bbc.co.uk/news"), "https://bbc.co.uk/news");
  EXPECT_EQ(AddressURL("Example.COM"), "https://example.com/");
  EXPECT_EQ(AddressURL("my-site.example.org/a_b/c.html"),
            "https://my-site.example.org/a_b/c.html");
  EXPECT_EQ(AddressURL("192.168.1.1"), "https://192.168.1.1/");

  for (const char* bad :
       {"", "example", "localhost", ".com", "example.", "example..com",
        "-example.com", "example-.com", "example.c", "example.c0m",
        "exa_mple.com", "example.com:8080", "user@example.com",
        "example.com/a?b", "example.com/a#b", "example.com/%2e",
        "javascript:alert(1)", "data:text/html,x", "file:///etc/passwd",
        "256.1.1.1", "1.2.3", "01.2.3.4", "1.2.3.4.5", "exam ple.com",
        "example.com\\evil.com"}) {
    SCOPED_TRACE(bad);
    EXPECT_FALSE(AddressURL(bad));
  }
  // A label of 64 bytes and a host of over 253 aren't host names.
  EXPECT_FALSE(AddressURL(std::string(64, 'a') + ".com"));
  EXPECT_TRUE(AddressURL(std::string(63, 'a') + ".com"));
  std::string long_host;
  for (int i = 0; i < 60; ++i) {
    long_host += "abcd.";
  }
  EXPECT_FALSE(AddressURL(long_host + "com"));
}

TEST(VoiceSitesTest, ResolveNeverInventsADomain) {
  SiteTarget target = ResolveSpokenSite("youtube", false, "YouTube");
  EXPECT_EQ(target.kind, SiteTarget::Kind::kKnownSite);
  EXPECT_EQ(target.url, "https://www.youtube.com/");
  EXPECT_EQ(target.name, "YouTube");

  target = ResolveSpokenSite("example.com", true, "example dot com");
  EXPECT_EQ(target.kind, SiteTarget::Kind::kAddress);
  EXPECT_EQ(target.url, "https://example.com/");
  EXPECT_EQ(target.name, "example.com");

  // An unknown name: searched for, never "netflix.com".
  target = ResolveSpokenSite("netflix", false, "Netflix");
  EXPECT_EQ(target.kind, SiteTarget::Kind::kUnknown);
  EXPECT_TRUE(target.url.empty());
  EXPECT_EQ(target.name, "Netflix");

  // An address that isn't one is a name too.
  target = ResolveSpokenSite("example.c", true, "example dot c");
  EXPECT_EQ(target.kind, SiteTarget::Kind::kUnknown);
  EXPECT_EQ(target.name, "example dot c");

  // A known site's name said as an address is that address.
  target = ResolveSpokenSite("youtube.com", true, "youtube dot com");
  EXPECT_EQ(target.kind, SiteTarget::Kind::kAddress);
  EXPECT_EQ(target.url, "https://youtube.com/");
}

TEST(VoiceSitesTest, ATestSiteIsKnownWhileItIsRegistered) {
  EXPECT_FALSE(FindKnownSite("videosite"));
  {
    const std::string home = "http://video.test:1234/molt_voice/home.html";
    const ScopedTestSite registered(
        {"VideoSite", home, "http://video.test:1234/results?q=",
         {"videosite"}, {"video.test"}});
    const KnownSite* site = FindKnownSite("VideoSite");
    ASSERT_TRUE(site);
    EXPECT_EQ(site->home_url, home);
    EXPECT_EQ(KnownSiteForHost("video.test"), site);
    EXPECT_EQ(SiteSearchURL(*site, "a b"), "http://video.test:1234/results?q=a+b");
    // The built-in sites are still there.
    EXPECT_EQ(FindKnownSite("youtube")->name, "YouTube");
    EXPECT_EQ(ResolveSpokenSite("videosite", false, "VideoSite").url, home);
  }
  EXPECT_FALSE(FindKnownSite("videosite"));
  EXPECT_FALSE(KnownSiteForHost("video.test"));
}

TEST(VoiceSitesTest, TheGrammarsAddressesResolve) {
  ParseContext context;
  struct Case {
    const char* said;
    const char* url;
  } kCases[] = {
      {"open example dot com", "https://example.com/"},
      {"go to w w w dot b b c dot co dot uk", "https://www.bbc.co.uk/"},
      {"example.com", "https://example.com/"},
  };
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.said);
    const VoiceCommand command = ParseVoiceCommand(c.said, context);
    ASSERT_EQ(command.kind, VoiceCommandKind::kOpenSite);
    const SiteTarget target = ResolveSpokenSite(
        command.site, command.site_is_address, command.site_words);
    EXPECT_EQ(target.kind, SiteTarget::Kind::kAddress);
    EXPECT_EQ(target.url, c.url);
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
