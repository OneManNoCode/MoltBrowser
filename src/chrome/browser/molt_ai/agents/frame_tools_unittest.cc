// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/frame_tools.h"

#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace frame_tools {
namespace {

constexpr char kShop[] = "https://shop.example";
constexpr char kNews[] = "https://news.example";
constexpr char kWidget[] = "https://widget.example";
constexpr char kPay[] = "https://pay.example";

base::Value Json(const std::string& json) {
  std::optional<base::Value> v =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  EXPECT_TRUE(v.has_value()) << json;
  return v ? std::move(*v) : base::Value();
}

TEST(FrameToolsTest, PairOriginsAreSerializedWebOrigins) {
  for (const char* good :
       {"https://widget.example", "http://localhost:8080", "https://a.b-c.d",
        "https://xn--bcher-kva.example", "https://[::1]:8443",
        "https://192.168.0.1", "https://my_host.example:1"}) {
    EXPECT_TRUE(IsPairOrigin(good)) << good;
  }
  for (const char* bad :
       {"", "null", "https://", "https://widget.example/",
        "https://widget.example/path", "https://Widget.example",
        "https://user@widget.example", "https://widget.example:",
        "https://widget.example:123456", "https://widget.example:8a",
        "https://.widget.example", "https://widget.example.",
        "https://wid..get.example", "ftp://widget.example",
        "file:///etc", "chrome://settings", "javascript:alert(1)",
        "https://widget.example?x", "https://widget.example#x",
        "https://wid get.example", "https://[::1", "https://[]",
        "https://[::1]x", "https://[::g]", "https://widget.example\n",
        "https://wi\xE2\x80\xAE" "dget.example", " https://widget.example"}) {
    EXPECT_FALSE(IsPairOrigin(bad)) << bad;
  }
  EXPECT_FALSE(IsPairOrigin("https://" + std::string(kMaxOriginBytes, 'a')));
}

// The permission matrix: per (main origin, frame origin) pair, never for
// the frame's origin everywhere, nor for the main origin's every frame.
TEST(FrameToolsTest, PermissionsArePerPair) {
  PermissionTable t;
  EXPECT_EQ(t.Get(kShop, kWidget), Grant::kNone);
  ASSERT_TRUE(t.Allow(kShop, kWidget, Grant::kSession, 1000));
  EXPECT_EQ(t.Get(kShop, kWidget), Grant::kSession);
  // Another page with the same frame, another frame on the same page, the
  // pair the other way round: still not allowed.
  EXPECT_EQ(t.Get(kNews, kWidget), Grant::kNone);
  EXPECT_EQ(t.Get(kShop, kPay), Grant::kNone);
  EXPECT_EQ(t.Get(kWidget, kShop), Grant::kNone);
  // Same site is not same origin.
  EXPECT_EQ(t.Get("https://www.shop.example", kWidget), Grant::kNone);
  EXPECT_EQ(t.Get(kShop, "https://cdn.widget.example"), Grant::kNone);
  EXPECT_EQ(t.Get(kShop, "http://widget.example"), Grant::kNone);
  EXPECT_EQ(t.AllowedFrames(kShop), std::vector<std::string>{kWidget});
  EXPECT_TRUE(t.AllowedFrames(kNews).empty());
  EXPECT_TRUE(t.AllowedFrames(kWidget).empty());
}

TEST(FrameToolsTest, SessionAndAlways) {
  PermissionTable t;
  ASSERT_TRUE(t.Allow(kShop, kWidget, Grant::kSession, 1));
  // "Always" upgrades a session answer; a session answer never downgrades
  // "always".
  ASSERT_TRUE(t.Allow(kShop, kWidget, Grant::kAlways, 2));
  EXPECT_EQ(t.Get(kShop, kWidget), Grant::kAlways);
  ASSERT_TRUE(t.Allow(kShop, kWidget, Grant::kSession, 3));
  EXPECT_EQ(t.Get(kShop, kWidget), Grant::kAlways);
  ASSERT_TRUE(t.Allow(kShop, kPay, Grant::kSession, 4));
  // Only "always" is stored.
  const base::ListValue stored = t.ToStored();
  ASSERT_EQ(stored.size(), 1u);
  const PermissionTable back = PermissionTable::FromStored(base::Value(stored.Clone()));
  EXPECT_EQ(back.Get(kShop, kWidget), Grant::kAlways);
  EXPECT_EQ(back.Get(kShop, kPay), Grant::kNone);
  ASSERT_EQ(back.List().size(), 1u);
  EXPECT_EQ(back.List()[0].since_ms, 2);
  // kNone is no answer to store.
  EXPECT_FALSE(t.Allow(kNews, kWidget, Grant::kNone, 5));
  EXPECT_EQ(t.Get(kNews, kWidget), Grant::kNone);
}

TEST(FrameToolsTest, RefusesWhatIsNotAPair) {
  PermissionTable t;
  EXPECT_FALSE(t.Allow(kShop, kShop, Grant::kAlways, 1));
  EXPECT_FALSE(t.Allow("null", kWidget, Grant::kSession, 1));
  EXPECT_FALSE(t.Allow(kShop, "null", Grant::kSession, 1));
  EXPECT_FALSE(t.Allow(kShop, "https://widget.example/", Grant::kSession, 1));
  EXPECT_FALSE(t.Allow("chrome://settings", kWidget, Grant::kSession, 1));
  EXPECT_EQ(t.size(), 0u);
}

TEST(FrameToolsTest, RemovesPairs) {
  PermissionTable t;
  t.Allow(kShop, kWidget, Grant::kAlways, 1);
  t.Allow(kShop, kPay, Grant::kSession, 1);
  t.Allow(kNews, kWidget, Grant::kSession, 1);
  EXPECT_FALSE(t.Remove(kWidget, kShop));
  EXPECT_TRUE(t.Remove(kShop, kWidget));
  EXPECT_EQ(t.Get(kShop, kWidget), Grant::kNone);
  EXPECT_EQ(t.Get(kNews, kWidget), Grant::kSession);
  const std::vector<Pair> list = t.List();
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0].main_origin, kNews);
  EXPECT_EQ(list[1].frame_origin, kPay);
  t.RemoveAll();
  EXPECT_EQ(t.size(), 0u);
}

TEST(FrameToolsTest, CapsHold) {
  PermissionTable t;
  for (size_t i = 0; i < kMaxFramesPerSite + 4; ++i) {
    const bool ok = t.Allow(kShop,
                            "https://w" + base::NumberToString(i) + ".example",
                            Grant::kSession, 1);
    EXPECT_EQ(ok, i < kMaxFramesPerSite) << i;
  }
  EXPECT_EQ(t.AllowedFrames(kShop).size(), kMaxFramesPerSite);
  PermissionTable many;
  size_t allowed = 0;
  for (size_t i = 0; i < kMaxPairs + 10; ++i) {
    allowed += many.Allow("https://s" + base::NumberToString(i) + ".example",
                          kWidget, Grant::kAlways, 1);
  }
  EXPECT_EQ(allowed, kMaxPairs);
  // Session answers have their own cap.
  EXPECT_TRUE(many.Allow(kShop, kWidget, Grant::kSession, 1));
}

TEST(FrameToolsTest, ReadsOnlyWellFormedStoredPairs) {
  const PermissionTable t = PermissionTable::FromStored(Json(R"([
    {"main": "https://shop.example", "frame": "https://widget.example",
     "since": 1700000000000},
    {"main": "https://shop.example", "frame": "https://widget.example",
     "since": 5},
    {"main": "https://news.example", "frame": "https://widget.example",
     "since": 3, "extra": 1},
    {"main": "https://news.example", "frame": "https://news.example",
     "since": 3},
    {"main": "https://news.example", "frame": "null", "since": 3},
    {"main": "https://news.example", "frame": "https://pay.example",
     "since": -1},
    {"main": "https://news.example", "frame": "https://pay.example",
     "since": 1.5},
    {"main": "https://news.example", "frame": "https://pay.example",
     "since": "3"},
    {"main": "https://news.example", "frame": "https://pay.example"},
    {"main": 1, "frame": "https://pay.example", "since": 3},
    [], "x", null,
    {"main": "https://news.example", "frame": "https://pay.example",
     "since": 7}
  ])"));
  const std::vector<Pair> list = t.List();
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list[0].main_origin, kNews);
  EXPECT_EQ(list[0].frame_origin, kPay);
  EXPECT_EQ(list[0].since_ms, 7);
  EXPECT_EQ(list[1].frame_origin, kWidget);
  EXPECT_EQ(list[1].since_ms, 1700000000000);
  EXPECT_EQ(list[1].grant, Grant::kAlways);
  for (const char* bad : {"{}", "null", "\"x\"", "1", "[[]]"}) {
    EXPECT_EQ(PermissionTable::FromStored(Json(bad)).size(), 0u) << bad;
  }
}

TEST(FrameToolsTest, StoredListsAreCapped) {
  base::ListValue stored;
  for (size_t i = 0; i < kMaxStoredEntriesRead + 50; ++i) {
    base::DictValue entry;
    entry.Set("main", "https://s" + base::NumberToString(i) + ".example");
    entry.Set("frame", kWidget);
    entry.Set("since", 1.0);
    stored.Append(std::move(entry));
  }
  const PermissionTable t = PermissionTable::FromStored(base::Value(std::move(stored)));
  EXPECT_EQ(t.size(), kMaxPairs);
}

TEST(FrameToolsTest, AsksInTheBrowsersWords) {
  EXPECT_EQ(QuestionText(kShop, kWidget, 3),
            "https://widget.example, a frame on https://shop.example, offers "
            "3 tools. Let the agent see them on https://shop.example? It asks "
            "you before each call.");
  EXPECT_NE(QuestionText(kShop, kWidget, 1).find("offers 1 tool."),
            std::string::npos);
  // Shown visibly whatever they are.
  const std::string odd =
      QuestionText(kShop, "https://a\xE2\x80\xAE" "b\"c.example", 2);
  EXPECT_EQ(odd.find("\xE2\x80\xAE"), std::string::npos);
  EXPECT_NE(odd.find("\\\""), std::string::npos);
}

TEST(FrameToolsTest, ParsesAnswers) {
  EXPECT_EQ(ParseAnswer("not_now"), Grant::kNone);
  EXPECT_EQ(ParseAnswer("session"), Grant::kSession);
  EXPECT_EQ(ParseAnswer("always"), Grant::kAlways);
  for (const char* bad : {"", "Always", "allow", "true", "session ", "1"})
    EXPECT_FALSE(ParseAnswer(bad).has_value()) << bad;
}

TEST(FrameToolsTest, NoSecretsGoToAnotherSitesFrame) {
  for (const char* ok :
       {R"({})", R"({"q":"blue shoes","qty":2})", R"({"tags":["a","b"]})",
        R"({"zip":"10115","gift":true})", R"({"note":null})"}) {
    EXPECT_FALSE(ArgumentsLookSecret(ok)) << ok;
  }
  for (const char* secret :
       {R"({"password":"x"})", R"({"pin":"1"})", R"({"card_number":"1"})", R"({"cc-csc":"1"})",
        R"({"q":"my password is hunter2"})", R"({"q":"4111 1111 1111 1111"})",
        R"({"code":"493817"})", R"({"n":4111111111111111})",
        R"({"n":493817})", R"({"tags":["ok","4111111111111111"]})",
        R"({"q":"sk_live_0123456789abcdef"})",
        R"({"q":"DE89 3704 0044 0532 0130 00"})",
        R"({"q":"pass​word: x"})", R"({"o":{"a":1}})",
        R"({"l":[["x"]]})", R"([])", R"("x")", R"(not json)"}) {
    EXPECT_TRUE(ArgumentsLookSecret(secret)) << secret;
  }
}

}  // namespace
}  // namespace frame_tools
}  // namespace molt_ai
