// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_util.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

// Real, public v3 onion services (format reference).
constexpr char kDuckDuckGo[] =
    "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion";
constexpr char kTorProject[] =
    "2gzyxa5ihm7nsggfxnu52rck2vv4rvmdlkiu3zzui5du4xyclen53wid.onion";

std::optional<OnionLocation> Parse(const std::string& value) {
  return ParseOnionLocationHeaderValues(std::vector<std::string>{value});
}

TEST(OnionUtilTest, IsOnionHostMatchesTheWholeSpecialUseDomain) {
  EXPECT_TRUE(IsOnionHost("onion"));
  EXPECT_TRUE(IsOnionHost("x.onion"));
  EXPECT_TRUE(IsOnionHost("www.x.onion"));
  EXPECT_TRUE(IsOnionHost(kDuckDuckGo));
  // Not a valid onion address, but still never sent to DNS.
  EXPECT_TRUE(IsOnionHost("not-an-address.onion"));
  EXPECT_TRUE(IsOnionHost(".onion"));
  // Case and trailing dots don't change the DNS name.
  EXPECT_TRUE(IsOnionHost("X.ONION"));
  EXPECT_TRUE(IsOnionHost("x.Onion"));
  EXPECT_TRUE(IsOnionHost("x.onion."));
  EXPECT_TRUE(IsOnionHost("x.onion.."));
  EXPECT_TRUE(IsOnionHost("onion."));
}

TEST(OnionUtilTest, IsOnionHostRejectsOtherNames) {
  EXPECT_FALSE(IsOnionHost(""));
  EXPECT_FALSE(IsOnionHost("."));
  EXPECT_FALSE(IsOnionHost("onio"));
  EXPECT_FALSE(IsOnionHost("xonion"));
  EXPECT_FALSE(IsOnionHost("x-onion"));
  EXPECT_FALSE(IsOnionHost("onion.com"));
  EXPECT_FALSE(IsOnionHost("x.onion.com"));
  EXPECT_FALSE(IsOnionHost("x.onions"));
  EXPECT_FALSE(IsOnionHost("example.com"));
  EXPECT_FALSE(IsOnionHost("127.0.0.1"));
  EXPECT_FALSE(IsOnionHost("[::1]"));
}

TEST(OnionUtilTest, V3OnionServiceHost) {
  EXPECT_TRUE(IsV3OnionServiceHost(kDuckDuckGo));
  EXPECT_TRUE(IsV3OnionServiceHost(kTorProject));
  EXPECT_TRUE(IsV3OnionServiceHost(
      "facebookwkhpilnemxj7asaniu7vnjjbiltxjqhye3mhbshg7kx5tfyd.onion"));
  EXPECT_TRUE(IsV3OnionServiceHost(std::string("www.") + kDuckDuckGo));
  EXPECT_TRUE(IsV3OnionServiceHost(std::string("a.b-c.9.") + kDuckDuckGo));

  // Wrong length (a v2 address is 16 characters).
  EXPECT_FALSE(IsV3OnionServiceHost("expyuzz4wqqyqhjn.onion"));
  EXPECT_FALSE(IsV3OnionServiceHost(
      "uckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"));
  EXPECT_FALSE(IsV3OnionServiceHost(
      "dduckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"));
  // Not base32 ('1', '8', '0' are not in the alphabet).
  EXPECT_FALSE(IsV3OnionServiceHost(
      "1uckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"));
  // Version character must be 'd', the one before it a/i/q/y.
  EXPECT_FALSE(IsV3OnionServiceHost(
      "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczae.onion"));
  EXPECT_FALSE(IsV3OnionServiceHost(
      "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczbd.onion"));
  // Not canonical: uppercase, trailing dot.
  EXPECT_FALSE(IsV3OnionServiceHost(
      "DUCKDUCKGOGG42XJOC72X3SJASOWOARFBGCMVFIMAFTT6TWAGSWZCZAD.onion"));
  EXPECT_FALSE(IsV3OnionServiceHost(std::string(kDuckDuckGo) + "."));
  // Bad subdomain labels.
  EXPECT_FALSE(IsV3OnionServiceHost(std::string(".") + kDuckDuckGo));
  EXPECT_FALSE(IsV3OnionServiceHost(std::string("a..") + kDuckDuckGo));
  EXPECT_FALSE(IsV3OnionServiceHost(std::string("a_b.") + kDuckDuckGo));
  EXPECT_FALSE(IsV3OnionServiceHost(std::string("A.") + kDuckDuckGo));
  EXPECT_FALSE(IsV3OnionServiceHost(std::string(64, 'a') + "." + kDuckDuckGo));
  // The address must be the label right before ".onion".
  EXPECT_FALSE(IsV3OnionServiceHost(std::string(kDuckDuckGo) + ".x.onion"));
  EXPECT_FALSE(IsV3OnionServiceHost("onion"));
  EXPECT_FALSE(IsV3OnionServiceHost(""));
}

TEST(OnionUtilTest, OnionLocationAccepted) {
  std::optional<OnionLocation> loc =
      Parse(std::string("http://") + kDuckDuckGo + "/");
  ASSERT_TRUE(loc);
  EXPECT_EQ("http", loc->scheme);
  EXPECT_EQ(kDuckDuckGo, loc->host);

  loc = Parse(std::string("https://www.") + kTorProject +
              ":8443/path/x.html?q=1#frag");
  ASSERT_TRUE(loc);
  EXPECT_EQ("https", loc->scheme);
  EXPECT_EQ(std::string("www.") + kTorProject, loc->host);

  // No path; surrounding whitespace trimmed; scheme and host lowercased.
  loc = Parse(std::string(" \tHTTP://") +
              "DuckDuckGoGG42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.ONION"
              "\t ");
  ASSERT_TRUE(loc);
  EXPECT_EQ("http", loc->scheme);
  EXPECT_EQ(kDuckDuckGo, loc->host);
}

TEST(OnionUtilTest, OnionLocationRejected) {
  const std::string host = kDuckDuckGo;
  const char* const kBad[] = {
      "",
      " ",
      // Not http(s), or not absolute.
      "ftp://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion/",
      "//duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion/",
      "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion",
      "javascript:alert(1)",
      "http:duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion",
      "https:/duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion",
      // Not an onion service.
      "https://example.com/",
      "https://example.onion/",
      "https://expyuzz4wqqyqhjn.onion/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion."
      "/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion."
      "evil.com/",
      // User info hides the real host from naive readers.
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion@"
      "evil.com/",
      "https://user@duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad."
      "onion/",
      // Backslash, escapes, spaces, controls, non-ASCII.
      "https://evil.com\\@duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzc"
      "zad.onion/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad%2Eonion"
      "/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion/"
      "a b",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion/"
      "\x01",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion/"
      "\xc3\xa9",
      // IP literals.
      "https://[::1]/",
      "http://127.0.0.1/",
      // Bad ports.
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion:"
      "/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion:"
      "0/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion:"
      "65536/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion:"
      "123456/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion:"
      "8a/",
      "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion:"
      "80:80/",
  };
  for (const char* bad : kBad) {
    EXPECT_FALSE(Parse(bad)) << bad;
  }
  // Too long.
  EXPECT_TRUE(Parse("https://" + host + "/"));
  EXPECT_FALSE(Parse("https://" + host + "/" + std::string(2048, 'a')));
  // Must appear exactly once.
  EXPECT_FALSE(ParseOnionLocationHeaderValues({}));
  EXPECT_FALSE(ParseOnionLocationHeaderValues(
      {"https://" + host + "/", "https://" + host + "/"}));
}

TEST(OnionUtilTest, OnionLocationOnlyHonoredOnHttpsNonOnionPages) {
  EXPECT_TRUE(ShouldHonorOnionLocation("https", "duckduckgo.com"));
  EXPECT_FALSE(ShouldHonorOnionLocation("http", "duckduckgo.com"));
  EXPECT_FALSE(ShouldHonorOnionLocation("file", ""));
  EXPECT_FALSE(ShouldHonorOnionLocation("https", ""));
  EXPECT_FALSE(ShouldHonorOnionLocation("https", kDuckDuckGo));
  EXPECT_FALSE(ShouldHonorOnionLocation("https", "x.onion."));
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
