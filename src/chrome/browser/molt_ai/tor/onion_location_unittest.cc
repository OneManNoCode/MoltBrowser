// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_location.h"

#include <string>

#include "base/check.h"
#include "base/memory/scoped_refptr.h"
#include "net/http/http_response_headers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai {
namespace tor {
namespace {

constexpr char kHost[] =
    "duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion";

// A 200 response with |extra_lines| ("Name: value\n" each) as headers.
scoped_refptr<net::HttpResponseHeaders> Headers(
    const std::string& extra_lines) {
  scoped_refptr<net::HttpResponseHeaders> headers =
      net::HttpResponseHeaders::TryToCreate("HTTP/1.1 200 OK\n" + extra_lines +
                                            "\n");
  CHECK(headers);  // Test input, not renderer input.
  return headers;
}

GURL Offered(const std::string& page, const std::string& extra_lines) {
  return GetOnionLocation(GURL(page), Headers(extra_lines).get());
}

TEST(OnionLocationTest, ValidHeaderOnHttpsPage) {
  const std::string onion = std::string("http://") + kHost + "/";
  EXPECT_EQ(GURL(onion), Offered("https://duckduckgo.com/",
                                 "Onion-Location: " + onion + "\n"));
  // Header names are case-insensitive; path, query and port are kept.
  const std::string deep =
      std::string("https://www.") + kHost + ":8443/a/b?q=1#f";
  EXPECT_EQ(GURL(deep), Offered("https://duckduckgo.com/x",
                                "onion-location: " + deep + "\n"));
}

TEST(OnionLocationTest, OnlyOnHttpsNonOnionPages) {
  const std::string line =
      std::string("Onion-Location: http://") + kHost + "/\n";
  EXPECT_FALSE(Offered("http://duckduckgo.com/", line).is_valid());
  EXPECT_FALSE(Offered(std::string("https://") + kHost + "/", line).is_valid());
  EXPECT_FALSE(Offered("file:///tmp/x.html", line).is_valid());
  EXPECT_FALSE(
      GetOnionLocation(GURL("https://duckduckgo.com/"), nullptr).is_valid());
  EXPECT_FALSE(Offered("https://duckduckgo.com/", "").is_valid());
}

TEST(OnionLocationTest, InvalidHeadersIgnored) {
  const std::string page = "https://duckduckgo.com/";
  const std::string onion = std::string("http://") + kHost + "/";
  // Twice (joined with ", " by net, refused).
  EXPECT_FALSE(Offered(page, "Onion-Location: " + onion +
                                 "\nOnion-Location: " + onion + "\n")
                   .is_valid());
  // Not an onion service, not http(s), not absolute.
  EXPECT_FALSE(
      Offered(page, "Onion-Location: https://example.com/\n").is_valid());
  EXPECT_FALSE(
      Offered(page, "Onion-Location: https://example.onion/\n").is_valid());
  EXPECT_FALSE(
      Offered(page, std::string("Onion-Location: ftp://") + kHost + "/\n")
          .is_valid());
  EXPECT_FALSE(Offered(page, std::string("Onion-Location: //") + kHost + "/\n")
                   .is_valid());
  EXPECT_FALSE(
      Offered(page, "Onion-Location: javascript:alert(1)\n").is_valid());
  // User info that would hide the real host.
  EXPECT_FALSE(Offered(page, std::string("Onion-Location: https://") + kHost +
                                 "@evil.example/\n")
                   .is_valid());
  EXPECT_FALSE(
      Offered(page, std::string("Onion-Location: https://u:p@") + kHost + "/\n")
          .is_valid());
  // Escaped host characters.
  EXPECT_FALSE(
      Offered(page,
              "Onion-Location: "
              "https://duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad"
              "%2Eonion/\n")
          .is_valid());
  EXPECT_FALSE(Offered(page, "Onion-Location:\n").is_valid());
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
