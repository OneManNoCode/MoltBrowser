// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/vault/vault_host_match.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai {
namespace vault {
namespace {

// ---- Pure classification (no Public Suffix List) ----

TEST(VaultHostMatchTest, ClassifyExact) {
  EXPECT_EQ(ClassifyHostMatch("github.com", "github.com", "github.com",
                              "github.com"),
            HostMatch::kExact);
  // IPs have no registrable domain but can still match exactly.
  EXPECT_EQ(ClassifyHostMatch("10.0.0.1", "", "10.0.0.1", ""),
            HostMatch::kExact);
}

TEST(VaultHostMatchTest, ClassifySameSite) {
  EXPECT_EQ(ClassifyHostMatch("github.com", "github.com", "gist.github.com",
                              "github.com"),
            HostMatch::kSameSite);
  EXPECT_EQ(ClassifyHostMatch("accounts.example.co.uk", "example.co.uk",
                              "www.example.co.uk", "example.co.uk"),
            HostMatch::kSameSite);
}

TEST(VaultHostMatchTest, ClassifyNone) {
  EXPECT_EQ(ClassifyHostMatch("", "", "github.com", "github.com"),
            HostMatch::kNone);
  EXPECT_EQ(ClassifyHostMatch("github.com", "github.com", "", ""),
            HostMatch::kNone);
  EXPECT_EQ(ClassifyHostMatch("github.com", "github.com", "evilgithub.com",
                              "evilgithub.com"),
            HostMatch::kNone);
  // Two hosts without a registrable domain never "share" the empty site.
  EXPECT_EQ(ClassifyHostMatch("10.0.0.1", "", "10.0.0.2", ""),
            HostMatch::kNone);
}

// ---- Canonicalisation ----

TEST(VaultHostMatchTest, CanonicalizeSiteHost) {
  EXPECT_EQ(CanonicalizeSiteHost("github.com"), "github.com");
  EXPECT_EQ(CanonicalizeSiteHost("  GitHub.COM "), "github.com");
  EXPECT_EQ(CanonicalizeSiteHost("https://github.com/login?x=1"),
            "github.com");
  EXPECT_EQ(CanonicalizeSiteHost("github.com:8443"), "github.com");
  EXPECT_EQ(CanonicalizeSiteHost(""), "");
  EXPECT_EQ(CanonicalizeSiteHost("   "), "");
  EXPECT_EQ(CanonicalizeSiteHost("javascript:alert(1)"), "");
}

// ---- Full matcher, with the real Public Suffix List ----

TEST(VaultHostMatchTest, MatchSiteHostExactAndSameSite) {
  EXPECT_EQ(MatchSiteHost("github.com", "github.com"), HostMatch::kExact);
  EXPECT_EQ(MatchSiteHost("https://GitHub.com/", "github.com"),
            HostMatch::kExact);
  EXPECT_EQ(MatchSiteHost("github.com", "gist.github.com"),
            HostMatch::kSameSite);
  EXPECT_EQ(MatchSiteHost("login.example.co.uk", "www.example.co.uk"),
            HostMatch::kSameSite);
}

TEST(VaultHostMatchTest, MatchSiteHostRejectsOtherSites) {
  // The old suffix matcher's false friends.
  EXPECT_EQ(MatchSiteHost("github.com", "github.com.evil.net"),
            HostMatch::kNone);
  EXPECT_EQ(MatchSiteHost("github.com", "evilgithub.com"), HostMatch::kNone);
  // Different registrable domains under a shared public suffix.
  EXPECT_EQ(MatchSiteHost("a.co.uk", "b.co.uk"), HostMatch::kNone);
  // Private-registry suffixes are sites of their own: one user's
  // github.io page must never get another user's credentials.
  EXPECT_EQ(MatchSiteHost("alice.github.io", "bob.github.io"),
            HostMatch::kNone);
  EXPECT_EQ(MatchSiteHost("myapp.appspot.com", "other.appspot.com"),
            HostMatch::kNone);
  // A bare public suffix is never a site.
  EXPECT_EQ(MatchSiteHost("co.uk", "example.co.uk"), HostMatch::kNone);
  EXPECT_EQ(MatchSiteHost("", "example.com"), HostMatch::kNone);
}

TEST(VaultHostMatchTest, IpHostsOnlyMatchExactly) {
  EXPECT_EQ(MatchSiteHost("192.168.1.10", "192.168.1.10"), HostMatch::kExact);
  EXPECT_EQ(MatchSiteHost("192.168.1.10", "192.168.1.11"), HostMatch::kNone);
}

// ---- Fillable origins ----

TEST(VaultHostMatchTest, IsFillableOrigin) {
  auto origin = [](const char* spec) {
    return url::Origin::Create(GURL(spec));
  };
  EXPECT_TRUE(IsFillableOrigin(origin("https://github.com/login")));
  EXPECT_TRUE(IsFillableOrigin(origin("http://localhost:3000/")));
  EXPECT_TRUE(IsFillableOrigin(origin("http://127.0.0.1:8080/")));
  EXPECT_TRUE(IsFillableOrigin(origin("http://[::1]/")));

  EXPECT_FALSE(IsFillableOrigin(origin("http://github.com/login")));
  EXPECT_FALSE(IsFillableOrigin(origin("http://192.168.1.1/")));
  EXPECT_FALSE(IsFillableOrigin(origin("file:///tmp/login.html")));
  EXPECT_FALSE(IsFillableOrigin(origin("data:text/html,<form>")));
  EXPECT_FALSE(IsFillableOrigin(url::Origin()));  // opaque
}

}  // namespace
}  // namespace vault
}  // namespace molt_ai
