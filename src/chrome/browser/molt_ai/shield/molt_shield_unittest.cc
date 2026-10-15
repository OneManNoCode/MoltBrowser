// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Unit tests for MoltShield's pure C++ glue (request mapping, list sanity
// checks, update scheduling, breakage report, on-disk state). The adblock
// engine itself is tested in Rust (tools/shield_bench).

#include <set>
#include <string>

#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/shield/filter_list_catalog.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/molt_ai/shield/molt_shield_store.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::shield {
namespace {

using network::mojom::RequestDestination;

url::Origin Origin(const char* url) {
  return url::Origin::Create(GURL(url));
}

std::string ListWithFilters(size_t n) {
  std::string text = "[Adblock Plus 2.0]\n! Title: Test\n";
  for (size_t i = 0; i < n; ++i) {
    text += "||ads" + std::to_string(i) + ".example^\n";
  }
  return text;
}

TEST(MoltShieldRequestUtilsTest, RequestTypes) {
  EXPECT_EQ("script",
            RequestTypeForDestination(RequestDestination::kScript, false));
  EXPECT_EQ("script",
            RequestTypeForDestination(RequestDestination::kWorker, false));
  EXPECT_EQ("image",
            RequestTypeForDestination(RequestDestination::kImage, false));
  EXPECT_EQ("stylesheet",
            RequestTypeForDestination(RequestDestination::kStyle, false));
  EXPECT_EQ("media",
            RequestTypeForDestination(RequestDestination::kVideo, false));
  EXPECT_EQ("sub_frame",
            RequestTypeForDestination(RequestDestination::kIframe, false));
  EXPECT_EQ("sub_frame", RequestTypeForDestination(
                             RequestDestination::kFencedframe, false));
  EXPECT_EQ("document",
            RequestTypeForDestination(RequestDestination::kDocument, false));
  // fetch()/XHR vs. sendBeacon / fetch keepalive.
  EXPECT_EQ("xmlhttprequest",
            RequestTypeForDestination(RequestDestination::kEmpty, false));
  EXPECT_EQ("ping",
            RequestTypeForDestination(RequestDestination::kEmpty, true));
  EXPECT_EQ("csp_report",
            RequestTypeForDestination(RequestDestination::kReport, false));
}

TEST(MoltShieldRequestUtilsTest, FilterableSchemes) {
  EXPECT_TRUE(IsFilterableUrl(GURL("https://ads.example/x.js")));
  EXPECT_TRUE(IsFilterableUrl(GURL("http://ads.example/x.js")));
  EXPECT_TRUE(IsFilterableUrl(GURL("wss://ws.example/socket")));
  EXPECT_TRUE(IsFilterableUrl(GURL("ws://ws.example/socket")));
  EXPECT_FALSE(IsFilterableUrl(GURL("chrome://settings")));
  EXPECT_FALSE(IsFilterableUrl(GURL("data:text/plain,hi")));
  EXPECT_FALSE(IsFilterableUrl(GURL("blob:https://a.example/uuid")));
  EXPECT_FALSE(IsFilterableUrl(GURL("file:///etc/hosts")));
  EXPECT_FALSE(IsFilterableUrl(GURL()));
}

TEST(MoltShieldRequestUtilsTest, ThirdPartyUsesRegistrableDomain) {
  EXPECT_FALSE(IsThirdParty(GURL("https://cdn.example.co.uk/a.js"),
                            Origin("https://www.example.co.uk")));
  EXPECT_TRUE(IsThirdParty(GURL("https://tracker.example/t.js"),
                           Origin("https://news.example.org")));
  // Private registries (PSL "private" section) separate sites.
  EXPECT_TRUE(IsThirdParty(GURL("https://alice.github.io/x.js"),
                           Origin("https://bob.github.io")));
  EXPECT_TRUE(IsThirdParty(GURL("https://a.example/x"), std::nullopt));
  EXPECT_TRUE(IsThirdParty(GURL("https://a.example/x"), url::Origin()));
}

TEST(MoltShieldRequestUtilsTest, MakeCheckInput) {
  std::optional<CheckInput> input =
      MakeCheckInput(GURL("https://Ads.Example/banner.js?x=1"),
                     Origin("https://www.news.test"), "script", "GET");
  ASSERT_TRUE(input.has_value());
  EXPECT_EQ("https://ads.example/banner.js?x=1", input->url);
  EXPECT_EQ("ads.example", input->host);
  EXPECT_EQ("www.news.test", input->source_host);
  EXPECT_EQ("script", input->type);
  EXPECT_TRUE(input->third_party);
  EXPECT_EQ("GET", input->method);

  input = MakeCheckInput(GURL("https://ads.example/x"), std::nullopt, "image",
                         "");
  ASSERT_TRUE(input.has_value());
  EXPECT_TRUE(input->source_host.empty());
  EXPECT_EQ("GET", input->method);

  EXPECT_FALSE(MakeCheckInput(GURL("chrome://newtab"), std::nullopt, "image",
                              "GET")
                   .has_value());
}

TEST(MoltShieldRequestUtilsTest, IsSafeParamStrip) {
  const GURL original("https://shop.example/p/1?id=7&fbclid=IwAR0#reviews");
  EXPECT_TRUE(IsSafeParamStrip(original,
                               GURL("https://shop.example/p/1?id=7#reviews")));
  EXPECT_TRUE(IsSafeParamStrip(GURL("https://a.example/?utm_source=x"),
                               GURL("https://a.example/")));
  // Anything beyond dropping query parameters is refused.
  EXPECT_FALSE(IsSafeParamStrip(original, original));
  EXPECT_FALSE(IsSafeParamStrip(
      original, GURL("https://evil.example/p/1?id=7#reviews")));
  EXPECT_FALSE(IsSafeParamStrip(
      original, GURL("http://shop.example/p/1?id=7#reviews")));
  EXPECT_FALSE(IsSafeParamStrip(
      original, GURL("https://shop.example:444/p/1?id=7#reviews")));
  EXPECT_FALSE(
      IsSafeParamStrip(original, GURL("https://shop.example/p/?id=7#reviews")));
  EXPECT_FALSE(
      IsSafeParamStrip(original, GURL("https://shop.example/p/1?id=7")));
  EXPECT_FALSE(IsSafeParamStrip(GURL("https://a.example/p"),
                                GURL("https://a.example/")));
  EXPECT_FALSE(IsSafeParamStrip(original, GURL()));
  EXPECT_FALSE(IsSafeParamStrip(GURL("ftp://a.example/?fbclid=1"),
                                GURL("ftp://a.example/")));
}

TEST(MoltShieldRequestUtilsTest, WhereParamsMayBeStripped) {
  EXPECT_TRUE(CanStripParamsFromSubresource(RequestDestination::kEmpty,
                                            /*keepalive=*/false));
  EXPECT_TRUE(
      CanStripParamsFromSubresource(RequestDestination::kScript, false));
  EXPECT_TRUE(CanStripParamsFromSubresource(RequestDestination::kImage, false));
  EXPECT_FALSE(CanStripParamsFromSubresource(RequestDestination::kEmpty,
                                             /*keepalive=*/true));
  EXPECT_FALSE(
      CanStripParamsFromSubresource(RequestDestination::kServiceWorker, false));
  EXPECT_FALSE(
      CanStripParamsFromSubresource(RequestDestination::kWorker, false));
  EXPECT_FALSE(
      CanStripParamsFromSubresource(RequestDestination::kSharedWorker, false));
  EXPECT_FALSE(
      CanStripParamsFromSubresource(RequestDestination::kDocument, false));
  EXPECT_FALSE(
      CanStripParamsFromSubresource(RequestDestination::kIframe, false));

  EXPECT_TRUE(CanStripParamsFromNavigation(
      "GET", GURL("https://a.example/?fbclid=1")));
  EXPECT_FALSE(CanStripParamsFromNavigation(
      "POST", GURL("https://a.example/?fbclid=1")));
  EXPECT_FALSE(
      CanStripParamsFromNavigation("GET", GURL("https://a.example/page")));
  EXPECT_FALSE(CanStripParamsFromNavigation(
      "GET", GURL("chrome://settings/?fbclid=1")));
}

TEST(MoltShieldRequestUtilsTest, LooksLikeFilterList) {
  EXPECT_TRUE(LooksLikeFilterList(ListWithFilters(kMinFilterLines)));
  EXPECT_FALSE(LooksLikeFilterList(ListWithFilters(kMinFilterLines - 1)));
  EXPECT_FALSE(LooksLikeFilterList(""));
  // Captive portal / error pages.
  EXPECT_FALSE(LooksLikeFilterList(
      "  <!DOCTYPE html><html><body>Sign in to Wi-Fi</body></html>\n" +
      ListWithFilters(100)));
  // Not UTF-8.
  std::string bad = ListWithFilters(100);
  bad[40] = static_cast<char>(0xff);
  EXPECT_FALSE(LooksLikeFilterList(bad));
}

TEST(MoltShieldRequestUtilsTest, ParseListMetadata) {
  ListMetadata meta = ParseListMetadata(
      "[Adblock Plus 2.0]\n! Version: 202610142210\n! Title: EasyList\n"
      "! Last modified: 14 Oct 2026 22:10 UTC\n||a^\n! Title: ignored\n");
  EXPECT_EQ("EasyList", meta.title);
  EXPECT_EQ("202610142210", meta.version);

  meta = ParseListMetadata(
      "! Title: uBlock filters\n! Last modified: %timestamp%\n||a^\n");
  EXPECT_EQ("uBlock filters", meta.title);
  EXPECT_EQ("", meta.version);

  meta = ParseListMetadata("! Title: X\n! Last modified: 1 Jan 2026\n");
  EXPECT_EQ("1 Jan 2026", meta.version);
}

TEST(MoltShieldRequestUtilsTest, NextUpdateDelay) {
  const base::Time now = base::Time::Now();
  EXPECT_EQ(kMinStartupDelay, NextUpdateDelay(base::Time(), now, {}));
  // Clock went backwards: due now.
  EXPECT_EQ(kMinStartupDelay, NextUpdateDelay(now + base::Hours(3), now, {}));
  // Overdue.
  EXPECT_EQ(kMinStartupDelay, NextUpdateDelay(now - base::Days(3), now, {}));
  EXPECT_EQ(base::Hours(22),
            NextUpdateDelay(now - base::Hours(2), now, base::TimeDelta()));
  EXPECT_EQ(base::Hours(23),
            NextUpdateDelay(now - base::Hours(2), now, base::Hours(1)));
  // Jitter is clamped to ±kMaxUpdateJitter.
  EXPECT_EQ(base::Hours(24) + kMaxUpdateJitter,
            NextUpdateDelay(now, now, base::Days(5)));
  EXPECT_EQ(base::Hours(24) - kMaxUpdateJitter,
            NextUpdateDelay(now, now, -base::Days(5)));
}

TEST(MoltShieldRequestUtilsTest, BreakageReportIsNonIdentifying) {
  ListStatus easylist;
  easylist.id = "easylist";
  easylist.enabled = true;
  easylist.cached = true;
  easylist.version = "202610142210";
  easylist.sha256_hex = "0123456789abcdef0123456789abcdef";
  ListStatus disabled;
  disabled.id = "fanboy-annoyance";
  disabled.enabled = false;

  std::string report = BuildBreakageReport(
      GURL("https://user:pw@mail.shop.example.co.uk/account/orders"
           "?id=123&email=a@b.c#frag"),
      /*shields_enabled_for_site=*/true, 7, {easylist, disabled}, "0.2.5",
      "adblock-rust-0.13.3/molt-1");
  EXPECT_NE(std::string::npos, report.find("Site: example.co.uk\n"));
  EXPECT_NE(std::string::npos, report.find("Shields: on"));
  EXPECT_NE(std::string::npos, report.find("Blocked on this page: 7"));
  EXPECT_NE(std::string::npos,
            report.find("easylist: 202610142210 sha256:0123456789ab"));
  EXPECT_EQ(std::string::npos, report.find("fanboy"));
  for (const char* secret :
       {"mail.", "shop.", "account", "orders", "id=123", "email", "a@b.c",
        "user:pw", "frag"}) {
    EXPECT_EQ(std::string::npos, report.find(secret)) << secret;
  }

  report = BuildBreakageReport(GURL("http://192.168.1.10/admin"), false, 0, {},
                               "0.2.5", "x");
  EXPECT_NE(std::string::npos, report.find("Site: (IP address)"));
  EXPECT_EQ(std::string::npos, report.find("192.168"));
  EXPECT_NE(std::string::npos, report.find("Shields: off"));
}

TEST(MoltShieldStoreTest, StateRoundTrip) {
  StoreState state;
  state.settings.enabled = false;
  state.settings.cosmetic = false;
  state.settings.consent_killer = true;
  state.settings.list_enabled["fanboy-annoyance"] = true;
  state.settings.list_enabled["easyprivacy"] = false;
  state.settings.last_update_check =
      base::Time::FromSecondsSinceUnixEpoch(1760000000);
  ListState& list = state.lists["easylist"];
  list.etag = "\"abc\"";
  list.last_modified = "Tue, 14 Oct 2026 22:10:00 GMT";
  list.sha256_hex = "ff00";
  list.size = int64_t{3} << 33;  // > 2^32, exercises int64 encoding.
  list.title = "EasyList";
  list.version = "202610142210";
  list.rule_lines = 77000;
  list.fetched_at = base::Time::FromSecondsSinceUnixEpoch(1760000100);
  list.checked_at = base::Time::FromSecondsSinceUnixEpoch(1760000200);
  list.last_error = "HTTP 503";

  StoreState parsed = ParseState(SerializeState(state));
  EXPECT_FALSE(parsed.settings.enabled);
  EXPECT_FALSE(parsed.settings.cosmetic);
  EXPECT_TRUE(parsed.settings.consent_killer);
  EXPECT_EQ(state.settings.list_enabled, parsed.settings.list_enabled);
  EXPECT_EQ(state.settings.last_update_check,
            parsed.settings.last_update_check);
  ASSERT_EQ(1u, parsed.lists.count("easylist"));
  const ListState& p = parsed.lists["easylist"];
  EXPECT_EQ(list.etag, p.etag);
  EXPECT_EQ(list.last_modified, p.last_modified);
  EXPECT_EQ(list.sha256_hex, p.sha256_hex);
  EXPECT_EQ(list.size, p.size);
  EXPECT_EQ(list.title, p.title);
  EXPECT_EQ(list.version, p.version);
  EXPECT_EQ(list.rule_lines, p.rule_lines);
  EXPECT_EQ(list.fetched_at, p.fetched_at);
  EXPECT_EQ(list.checked_at, p.checked_at);
  EXPECT_EQ(list.last_error, p.last_error);
}

TEST(MoltShieldStoreTest, ParseStateToleratesGarbage) {
  base::DictValue root;
  root.Set("settings", "not a dict");
  base::DictValue lists;
  lists.Set("easylist", 5);
  root.Set("lists", std::move(lists));
  StoreState state = ParseState(root);
  EXPECT_TRUE(state.settings.enabled);  // Defaults.
  EXPECT_TRUE(state.lists.empty());
}

TEST(MoltShieldStoreTest, ManifestRoundTripAndValidation) {
  EngineManifest manifest;
  manifest.format_key = "adblock-rust-0.13.3/molt-1";
  manifest.list_hashes["easylist"] = "aa";
  manifest.list_hashes["easyprivacy"] = "bb";
  manifest.engine_sha256 = "cc";
  std::optional<EngineManifest> parsed =
      ParseManifest(SerializeManifest(manifest));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(manifest.format_key, parsed->format_key);
  EXPECT_EQ(manifest.list_hashes, parsed->list_hashes);
  EXPECT_EQ(manifest.engine_sha256, parsed->engine_sha256);

  base::DictValue missing;
  missing.Set("format", "x");
  EXPECT_FALSE(ParseManifest(missing).has_value());
}

TEST(MoltShieldStoreTest, Sha256Hex) {
  EXPECT_EQ(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      Sha256Hex("abc"));
}

TEST(MoltShieldCatalogTest, DefaultsAndOverrides) {
  std::set<std::string> ids;
  for (const FilterListEntry& entry : GetFilterListCatalog()) {
    EXPECT_TRUE(ids.insert(entry.id).second) << "duplicate id " << entry.id;
    EXPECT_TRUE(GURL(entry.url).SchemeIs("https")) << entry.id;
    if (entry.fallback_url[0] != '\0') {
      EXPECT_TRUE(GURL(entry.fallback_url).SchemeIs("https")) << entry.id;
    }
    // Cache file names come from ids.
    EXPECT_TRUE(base::ContainsOnlyChars(
        entry.id, "abcdefghijklmnopqrstuvwxyz0123456789-"))
        << entry.id;
  }
  ASSERT_NE(nullptr, FindFilterList("easylist"));
  EXPECT_EQ(nullptr, FindFilterList("nope"));
  EXPECT_TRUE(FindFilterList("ublock-filters")->trusted);
  EXPECT_FALSE(FindFilterList("easylist")->trusted);

  StoreState state;
  std::vector<ListSpec> specs = EnabledListSpecs(state);
  std::set<std::string> enabled;
  for (const ListSpec& spec : specs) {
    enabled.insert(spec.id);
    EXPECT_TRUE(spec.expected_sha256.empty());
  }
  EXPECT_EQ((std::set<std::string>{"easylist", "easyprivacy", "ublock-filters",
                                   "ublock-privacy", "ublock-badware",
                                   "ublock-quick-fixes"}),
            enabled);

  state.settings.list_enabled["easyprivacy"] = false;
  state.settings.list_enabled["fanboy-annoyance"] = true;
  state.lists["easylist"].sha256_hex = "abc";
  enabled.clear();
  for (const ListSpec& spec : EnabledListSpecs(state)) {
    enabled.insert(spec.id);
    if (spec.id == "easylist") {
      EXPECT_EQ("abc", spec.expected_sha256);
    }
  }
  EXPECT_EQ(0u, enabled.count("easyprivacy"));
  EXPECT_EQ(1u, enabled.count("fanboy-annoyance"));
}

}  // namespace
}  // namespace molt_ai::shield
