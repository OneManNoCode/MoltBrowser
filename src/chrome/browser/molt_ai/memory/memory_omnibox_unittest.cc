// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The @memory omnibox shortcut's pure logic
// (components/omnibox/browser/molt_memory_query.h): which omnibox text is
// the shortcut's, the query it carries, molt://memory/?q= both ways, who
// gets the shortcut (each kind of profile, Memory off or paused) and how
// memory's pages become suggestion rows. Standard library only.

#include "components/omnibox/browser/molt_memory_query.h"

#include <optional>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_memory {
namespace {

constexpr char kRlo[] = "\xE2\x80\xAE";     // U+202E RIGHT-TO-LEFT OVERRIDE
constexpr char kFffd[] = "\xEF\xBF\xBD";    // U+FFFD
constexpr char kEAcute[] = "\xC3\xA9";      // é

size_t CountCharacters(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++n;
  }
  return n;
}

// ---- Shortcut input ----------------------------------------------------

TEST(MoltMemoryQueryTest, ShortcutInputIsTheKeywordAloneOrWithWords) {
  EXPECT_TRUE(IsShortcutInput(u"@memory"));
  EXPECT_TRUE(IsShortcutInput(u"@memory "));
  EXPECT_TRUE(IsShortcutInput(u"@memory miso soup"));
  EXPECT_TRUE(IsShortcutInput(u"@MEMORY miso"));
  EXPECT_TRUE(IsShortcutInput(u"@Memory\tmiso"));
  EXPECT_TRUE(IsShortcutInput(u"  @memory miso"));
  EXPECT_TRUE(IsShortcutInput(u"@memory\u3000\u5473\u564c"));  // ideographic
  EXPECT_TRUE(IsShortcutInput(u"@memory\u00A0miso"));         // NBSP
  // Full-width forms, as an East Asian IME types them.
  EXPECT_TRUE(IsShortcutInput(
      u"\uFF20\uFF4D\uFF45\uFF4D\uFF4F\uFF52\uFF59\u3000\u5473\u564c"));
  EXPECT_TRUE(IsShortcutInput(u"\uFF20memory miso"));

  EXPECT_FALSE(IsShortcutInput(u""));
  EXPECT_FALSE(IsShortcutInput(u"@"));
  EXPECT_FALSE(IsShortcutInput(u"@memo"));
  EXPECT_FALSE(IsShortcutInput(u"@memoryfoo"));
  EXPECT_FALSE(IsShortcutInput(u"@memory-foo"));
  EXPECT_FALSE(IsShortcutInput(u"memory foo"));
  EXPECT_FALSE(IsShortcutInput(u"@ memory foo"));
  EXPECT_FALSE(IsShortcutInput(u"x @memory foo"));
  EXPECT_FALSE(IsShortcutInput(u"youtube.com @memory foo"));  // keyword mode
  EXPECT_FALSE(IsShortcutInput(u"@ai what is this"));
}

TEST(MoltMemoryQueryTest, PrefixesOfTheKeywordGetAHint) {
  for (const char16_t* text : {u"@", u"@m", u"@me", u"@MeM", u"@memo",
                               u"@memor", u" @mem"}) {
    EXPECT_TRUE(IsShortcutPrefix(text)) << std::u16string(text).size();
  }
  for (const char16_t* text : {u"", u"  ", u"@memory", u"@memory foo",
                               u"@mx", u"@mem ", u"m", u"@ai"}) {
    EXPECT_FALSE(IsShortcutPrefix(text)) << std::u16string(text).size();
  }
}

TEST(MoltMemoryQueryTest, ShortcutQueryIsTheNormalizedWords) {
  EXPECT_EQ(ShortcutQuery(u"@memory miso soup"), "miso soup");
  EXPECT_EQ(ShortcutQuery(u"  @MEMORY   miso \t  soup  "), "miso soup");
  EXPECT_EQ(ShortcutQuery(u"@memory"), "");
  EXPECT_EQ(ShortcutQuery(u"\uFF20\uFF2D\uFF25\uFF2D\uFF2F\uFF32\uFF39 miso"),
            "miso");
  EXPECT_EQ(ShortcutQuery(u"@memory    "), "");
  EXPECT_EQ(ShortcutQuery(u"@memoryfoo"), "");
  EXPECT_EQ(ShortcutQuery(u"miso soup"), "");
  // Non-ASCII is kept; an unpaired surrogate becomes U+FFFD.
  EXPECT_EQ(ShortcutQuery(u"@memory caf\u00E9"), std::string("caf") + kEAcute);
  EXPECT_EQ(ShortcutQuery(u"@memory a\xD800" u"b"),
            std::string("a") + kFffd + "b");
  EXPECT_EQ(ShortcutQuery(u"@memory \U0001F35C ramen"),
            "\xF0\x9F\x8D\x9C ramen");
  // Bidi overrides and controls never reach the query.
  EXPECT_EQ(ShortcutQuery(u"@memory abc\u202Edef\u0007"), "abcdef");
  // Cut to 500 characters.
  const std::u16string long_text = u"@memory " + std::u16string(800, u'x');
  EXPECT_EQ(ShortcutQuery(long_text), std::string(500, 'x'));
}

// ---- Normalization -------------------------------------------------------

TEST(MoltMemoryQueryTest, NormalizeMakesTextSafeToShowAndSearch) {
  EXPECT_EQ(NormalizeQuery("  miso\n\nsoup\r\n"), "miso soup");
  EXPECT_EQ(NormalizeQuery("a\xC2\xA0\xE2\x80\xA8" "b"), "a b");  // NBSP, LS
  EXPECT_EQ(NormalizeQuery(std::string("a") + kRlo + "b"), "ab");
  EXPECT_EQ(NormalizeQuery("a\xE2\x81\xA6" "b\xE2\x81\xA9"), "ab");  // isolates
  EXPECT_EQ(NormalizeQuery("a\xE2\x80\x8F" "b"), "ab");               // RLM
  EXPECT_EQ(NormalizeQuery("\xEF\xBB\xBF" "abc"), "abc");             // BOM
  EXPECT_EQ(NormalizeQuery(std::string("a\0b", 3)), "ab");
  EXPECT_EQ(NormalizeQuery("a\x7F\xC2\x85\xC2\x9F" "b"), "a b");  // NEL is a space
  // Invalid UTF-8: one U+FFFD per bad byte; overlong and surrogate forms too.
  EXPECT_EQ(NormalizeQuery("a\xFF" "b"), std::string("a") + kFffd + "b");
  EXPECT_EQ(NormalizeQuery("\xC0\xAF"), std::string(kFffd) + kFffd);
  EXPECT_EQ(NormalizeQuery("\xED\xA0\x80"),
            std::string(kFffd) + kFffd + kFffd);
  EXPECT_EQ(NormalizeQuery("\xE2\x82"), std::string(kFffd) + kFffd);
  EXPECT_EQ(NormalizeQuery(""), "");
  EXPECT_EQ(NormalizeQuery(" \t\n "), "");
}

TEST(MoltMemoryQueryTest, NormalizeCutsOnCharactersWithoutTrailingSpace) {
  std::string many;
  for (int i = 0; i < 400; ++i) many += kEAcute;
  many += " ";
  for (int i = 0; i < 400; ++i) many += kEAcute;
  const std::string out = NormalizeQuery(many);
  EXPECT_EQ(CountCharacters(out), kMaxQueryCharacters);
  EXPECT_NE(out.back(), ' ');
  EXPECT_EQ(NormalizeText(many, 400), std::string(many, 0, 800));  // no space
  EXPECT_EQ(NormalizeText(many, 401), std::string(many, 0, 800));
  EXPECT_EQ(NormalizeText(many, 402), std::string(many, 0, 803));
  EXPECT_EQ(NormalizeText("abc", 0), "");
}

TEST(MoltMemoryQueryTest, NormalizeIsIdempotent) {
  const std::vector<std::string> inputs = {
      "  a  b ", "x\xFFy", std::string(kRlo) + " q", "\xC2\xA0", "abc",
      std::string(600, 'z'), "a \xE3\x80\x80 b"};
  for (const std::string& in : inputs) {
    const std::string once = NormalizeQuery(in);
    EXPECT_EQ(NormalizeQuery(once), once) << in;
  }
}

// ---- molt://memory/?q= -----------------------------------------------------

TEST(MoltMemoryQueryTest, QueryParameterIsDecodedAndNormalized) {
  EXPECT_EQ(QueryFromUrlQuery("q=miso+soup"), "miso soup");
  EXPECT_EQ(QueryFromUrlQuery("q=miso%20soup"), "miso soup");
  EXPECT_EQ(QueryFromUrlQuery("a=1&q=first&q=second"), "first");
  EXPECT_EQ(QueryFromUrlQuery("&&q=x&"), "x");
  EXPECT_EQ(QueryFromUrlQuery("%71=spelled"), "spelled");
  EXPECT_EQ(QueryFromUrlQuery("q=a%26b%3Dc"), "a&b=c");
  EXPECT_EQ(QueryFromUrlQuery("q=caf%C3%A9"), std::string("caf") + kEAcute);
  // Malformed escapes stay as they are; bad bytes become U+FFFD.
  EXPECT_EQ(QueryFromUrlQuery("q=100%"), "100%");
  EXPECT_EQ(QueryFromUrlQuery("q=%zz%4"), "%zz%4");
  EXPECT_EQ(QueryFromUrlQuery("q=%FF"), kFffd);
  // Nothing hostile survives decoding.
  EXPECT_EQ(QueryFromUrlQuery("q=%E2%80%AEevil%00%0A%1Bx"), "evil x");
  EXPECT_EQ(QueryFromUrlQuery("q=%3Cimg%20src%3Dx%3E"), "<img src=x>");

  EXPECT_EQ(QueryFromUrlQuery(""), std::nullopt);
  EXPECT_EQ(QueryFromUrlQuery("q"), std::nullopt);
  EXPECT_EQ(QueryFromUrlQuery("q="), std::nullopt);
  EXPECT_EQ(QueryFromUrlQuery("q=+++"), std::nullopt);
  EXPECT_EQ(QueryFromUrlQuery("q=&q=later"), std::nullopt);  // first q wins
  EXPECT_EQ(QueryFromUrlQuery("Q=x"), std::nullopt);
  EXPECT_EQ(QueryFromUrlQuery("qq=x&xq=y"), std::nullopt);
  EXPECT_EQ(QueryFromUrlQuery("query=x"), std::nullopt);

  const std::string long_value = "q=" + std::string(5000, 'a');
  EXPECT_EQ(QueryFromUrlQuery(long_value), std::string(500, 'a'));
}

TEST(MoltMemoryQueryTest, SearchUrlEncodesEverythingButUnreserved) {
  EXPECT_EQ(MemorySearchUrl("miso soup"),
            "chrome://molt-memory/?q=miso%20soup");
  EXPECT_EQ(MemorySearchUrl("a&b=c+d/#?"),
            "chrome://molt-memory/?q=a%26b%3Dc%2Bd%2F%23%3F");
  EXPECT_EQ(MemorySearchUrl(std::string("caf") + kEAcute),
            "chrome://molt-memory/?q=caf%C3%A9");
  EXPECT_EQ(MemorySearchUrl("A-z_0.9~"), "chrome://molt-memory/?q=A-z_0.9~");
  EXPECT_EQ(MemorySearchUrl(""), kMemoryPageUrl);
  EXPECT_EQ(MemorySearchUrl(" \n"), kMemoryPageUrl);
  EXPECT_EQ(MemorySearchUrl(std::string("  x") + kRlo + "  "),
            "chrome://molt-memory/?q=x");
}

TEST(MoltMemoryQueryTest, SearchUrlRoundTrips) {
  const std::vector<std::string> queries = {
      "miso soup", "a+b", "100%", "%41", "q=x&q=y", "<script>", "\xFF\xFE",
      std::string("rtl ") + kRlo + "x", "\xE5\x91\xB3\xE5\x99\x8C",
      std::string(700, 'k'), "tab\tand\nnewline"};
  const std::string prefix = std::string(kMemoryPageUrl) + "?";
  for (const std::string& q : queries) {
    const std::string url = MemorySearchUrl(q);
    ASSERT_EQ(url.compare(0, prefix.size(), prefix), 0) << url;
    EXPECT_EQ(QueryFromUrlQuery(url.substr(prefix.size())),
              NormalizeQuery(q))
        << q;
  }
}

// ---- Who gets the shortcut -------------------------------------------------

struct ProfileCase {
  const char* name;
  bool off_the_record;
  bool guest;
  // Whether the profile has an AI memory (MemoryServiceFactory builds one
  // for regular profiles only).
  bool has_memory;
};

// How each kind of profile looks to the omnibox.
constexpr ProfileCase kProfiles[] = {
    {"regular", false, false, true},
    {"incognito", true, false, false},
    {"guest", true, true, false},
    {"moltnet", true, false, false},  // a non-primary off-the-record profile
    {"system", false, false, false},
    {"other off-the-record", true, false, false},
    // Defence in depth: even if a memory were found for one of these.
    {"incognito with a memory", true, false, true},
    {"guest with a memory", false, true, true},
};

TEST(MoltMemoryQueryTest, OnlyARegularProfileWithMemoryGetsTheShortcut) {
  for (const ProfileCase& p : kProfiles) {
    const bool regular = !p.off_the_record && !p.guest && p.has_memory;
    for (bool enabled : {false, true}) {
      for (bool paused : {false, true}) {
        const ShortcutState state = ShortcutStateFor(
            p.off_the_record, p.guest, p.has_memory, enabled, paused);
        ShortcutState want = ShortcutState::kHidden;
        if (regular) {
          want = !enabled ? ShortcutState::kOff
                 : paused ? ShortcutState::kPaused
                          : ShortcutState::kReady;
        }
        EXPECT_EQ(state, want) << p.name << " enabled=" << enabled
                               << " paused=" << paused;
      }
    }
  }
}

TEST(MoltMemoryQueryTest, MemoryOffOrPausedNeverSearches) {
  EXPECT_EQ(ShortcutStateFor(false, false, true, false, false),
            ShortcutState::kOff);
  EXPECT_EQ(ShortcutStateFor(false, false, true, false, true),
            ShortcutState::kOff);  // off wins over paused
  EXPECT_EQ(ShortcutStateFor(false, false, true, true, true),
            ShortcutState::kPaused);
  EXPECT_EQ(ShortcutStateFor(false, false, true, true, false),
            ShortcutState::kReady);
}

// ---- Suggestion rows -------------------------------------------------------

TEST(MoltMemoryQueryTest, OnlyHttpUrlsAreSuggestible) {
  for (const char* url :
       {"http://a.example/", "https://a.example/p?q=1#f", "HTTPS://A.EXAMPLE",
        "https://a.example:8443/x", "http://127.0.0.1/"}) {
    EXPECT_TRUE(IsSuggestibleUrl(url)) << url;
  }
  for (const char* url :
       {"", "javascript:alert(1)", "data:text/html,x", "file:///etc/passwd",
        "chrome://settings", "molt://memory", "about:blank", "http://",
        "https:///path", "https://?q", "http://#f", "https://a b.example/",
        "https://a.example/\x01", "https://a.example/\x7F",
        "https://\xC3\xA9.example/", " https://a.example/", "ftp://a.example/",
        "https:/a.example", "http:\\\\a.example"}) {
    EXPECT_FALSE(IsSuggestibleUrl(url)) << url;
  }
  EXPECT_TRUE(IsSuggestibleUrl("https://a.example/" + std::string(4000, 'a')));
  EXPECT_FALSE(IsSuggestibleUrl("https://a.example/" + std::string(5000, 'a')));
}

TEST(MoltMemoryQueryTest, RowsKeepOrderDropBadUrlsAndDuplicates) {
  std::vector<MemoryHit> hits = {
      {"javascript:alert(1)", "evil"},
      {"https://a.example/miso", "Miso soup"},
      {"https://b.example/", "B"},
      {"https://a.example/miso", "duplicate"},
      {"data:text/html,<b>x</b>", "data"},
      {"http://c.example/", "C"},
  };
  std::vector<SuggestionRow> rows = SuggestionRows(hits, 5);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].url, "https://a.example/miso");
  EXPECT_EQ(rows[0].title, "Miso soup");
  EXPECT_EQ(rows[1].url, "https://b.example/");
  EXPECT_EQ(rows[2].url, "http://c.example/");
  EXPECT_EQ(rows[0].relevance, kFirstHitRelevance);
  EXPECT_EQ(rows[1].relevance, kFirstHitRelevance - kHitRelevanceStep);
  EXPECT_LT(rows[0].relevance, kSearchRowRelevance);
  EXPECT_GT(rows[0].relevance, rows[1].relevance);
  EXPECT_GT(rows[1].relevance, rows[2].relevance);

  EXPECT_EQ(SuggestionRows(hits, 1).size(), 1u);
  EXPECT_TRUE(SuggestionRows(hits, 0).empty());
  EXPECT_TRUE(SuggestionRows({}, 5).empty());
}

TEST(MoltMemoryQueryTest, TitlesBecomeDisplaySafeText) {
  std::vector<MemoryHit> hits = {
      {"https://a.example/1", "  Line one\nline\ttwo  "},
      {"https://a.example/2", std::string("moc.elgoog") + kRlo + "evil"},
      {"https://a.example/3", "<script>alert(1)</script>"},
      {"https://a.example/4", "\n\t "},
      {"https://a.example/5", std::string(1000, 't')},
      {"https://a.example/6", "bad\xFF\x1B[31mcolour"},
  };
  std::vector<SuggestionRow> rows = SuggestionRows(hits, 10);
  ASSERT_EQ(rows.size(), 6u);
  EXPECT_EQ(rows[0].title, "Line one line two");
  EXPECT_EQ(rows[1].title, "moc.elgoogevil");  // no override to reorder it
  // Markup stays text: the omnibox never parses it.
  EXPECT_EQ(rows[2].title, "<script>alert(1)</script>");
  EXPECT_EQ(rows[3].title, "https://a.example/4");  // no title: the URL
  EXPECT_EQ(rows[4].title, std::string(kMaxTitleCharacters, 't'));
  EXPECT_EQ(rows[5].title, std::string("bad") + kFffd + "[31mcolour");
}

TEST(MoltMemoryQueryTest, ManyRowsKeepPositiveRelevance) {
  std::vector<MemoryHit> hits;
  for (int i = 0; i < 300; ++i) {
    hits.push_back({"https://a.example/" + std::to_string(i), "t"});
  }
  std::vector<SuggestionRow> rows = SuggestionRows(hits, 300);
  ASSERT_EQ(rows.size(), 300u);
  for (size_t i = 1; i < rows.size(); ++i) {
    EXPECT_GE(rows[i].relevance, 1);
    EXPECT_LE(rows[i].relevance, rows[i - 1].relevance);
  }
}

}  // namespace
}  // namespace molt_memory
