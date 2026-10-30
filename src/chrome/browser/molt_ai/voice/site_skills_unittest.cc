// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/site_skills.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "chrome/browser/molt_ai/voice/html_fixture_tree.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_test_trees.h"
#include "chrome/browser/molt_ai/voice/voice_candidates.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

// A stored page under chrome/test/data/molt_voice/skills/, as its tree.
PageTree Fixture(std::string_view name) {
  base::FilePath root;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
  const base::FilePath path = root.AppendASCII("chrome")
                                  .AppendASCII("test")
                                  .AppendASCII("data")
                                  .AppendASCII("molt_voice")
                                  .AppendASCII("skills")
                                  .AppendASCII(name);
  std::string html;
  EXPECT_TRUE(base::ReadFileToString(path, &html)) << name;
  return test::PageTreeFromHtml(html);
}

const SiteSkill& Skill(std::string_view id) {
  for (const SiteSkill& skill : BuiltInSiteSkills()) {
    if (skill.id == id) {
      return skill;
    }
  }
  ADD_FAILURE() << "no skill " << id;
  return BuiltInSiteSkills().front();
}

PageMap MapWith(const PageTree& tree,
                std::string_view skill,
                std::string_view path,
                SkillReport* report) {
  return BuildPageMapWithSkills(tree, PageMapLimits(), &Skill(skill), nullptr,
                                path, report);
}

const PageItem* Item(const PageMap& map, const PageNodeHandle& handle) {
  return handle.valid() ? map.ItemForHandle(handle) : nullptr;
}

std::vector<std::string> Titles(const PageList& list) {
  std::vector<std::string> titles;
  for (const PageListEntry& entry : list.entries) {
    titles.push_back(entry.title);
  }
  return titles;
}

// --- The format -------------------------------------------------------------

TEST(SiteSkillsTest, TheBuiltInSkillsParse) {
  const SkillParseResult parsed = ParseSiteSkills(BuiltInSiteSkillsJson());
  EXPECT_TRUE(parsed.errors.empty()) << parsed.errors.front();
  std::vector<std::string> ids;
  for (const SiteSkill& skill : parsed.skills) {
    ids.push_back(skill.id);
    EXPECT_GT(skill.version, 0);
    EXPECT_FALSE(skill.search_field.empty()) << skill.id;
  }
  EXPECT_EQ(ids, (std::vector<std::string>{"youtube", "google", "duckduckgo",
                                           "bing", "wikipedia"}));
  EXPECT_EQ(BuiltInSiteSkills().size(), 5u);
  EXPECT_EQ(Skill("youtube").Label(), "youtube@1");
}

TEST(SiteSkillsTest, HostsTheyApplyTo) {
  EXPECT_EQ(BuiltInSkillForHost("www.youtube.com"), &Skill("youtube"));
  EXPECT_EQ(BuiltInSkillForHost("YouTube.com"), &Skill("youtube"));
  EXPECT_EQ(BuiltInSkillForHost("m.youtube.com"), nullptr);
  EXPECT_EQ(BuiltInSkillForHost("music.youtube.com"), nullptr);
  EXPECT_EQ(BuiltInSkillForHost("notyoutube.com"), nullptr);
  EXPECT_EQ(BuiltInSkillForHost("www.google.co.uk"), &Skill("google"));
  // Google's other services aren't its search pages.
  EXPECT_EQ(BuiltInSkillForHost("mail.google.com"), nullptr);
  EXPECT_EQ(BuiltInSkillForHost("duckduckgo.com"), &Skill("duckduckgo"));
  EXPECT_EQ(BuiltInSkillForHost("www.bing.com"), &Skill("bing"));
  EXPECT_EQ(BuiltInSkillForHost("en.wikipedia.org"), &Skill("wikipedia"));
  EXPECT_EQ(BuiltInSkillForHost("de.m.wikipedia.org"), &Skill("wikipedia"));
  EXPECT_EQ(BuiltInSkillForHost("wikipedia.org.example"), nullptr);
  EXPECT_EQ(BuiltInSkillForHost(""), nullptr);
}

TEST(SiteSkillsTest, ScopedSkillHost) {
  {
    ScopedSkillHostForTesting scoped("127.0.0.1", "youtube");
    EXPECT_EQ(BuiltInSkillForHost("127.0.0.1"), &Skill("youtube"));
  }
  EXPECT_EQ(BuiltInSkillForHost("127.0.0.1"), nullptr);
}

std::string OneSkill(std::string_view body) {
  return R"({"format": 1, "skills": [{"id": "a", "version": 1, "name": "A",
             "hosts": ["a.example"], )" +
         std::string(body) + "}]}";
}

TEST(SiteSkillsTest, AnythingUnexpectedRefusesTheSkill) {
  const char* bad[] = {
      R"("search": {"field": [{"rol": "button"}]})",
      R"("search": {"field": [{"role": "buton"}]})",
      R"("search": {"field": [{"role": ""}]})",
      R"("search": {"field": [{"role": "button|"}]})",
      R"("search": {"field": [{"inside": {"inside": {"role": "main"}}}]})",
      R"("search": {"field": [{"has": {"has": {"role": "main"}}}]})",
      R"("search": {"field": []})",
      R"("search": {"field": [{}]})",
      R"("search": {"box": [{"role": "button"}]})",
      R"("search": {"field": [{"name": "..."}]})",
      R"("search": {"field": [{"id": "0123456789012345678901234567890123456789012345678901234567890123456789"}]})",
      R"("search": {"field": [{"level": 0}]})",
      R"("search": {"field": [{"level": "3-2"}]})",
      R"("next_page_scrolls": "yes")",
      R"("pages": [{"path": "results", "expect": ["search"]}])",
      R"("pages": [{"path": "/r", "expect": ["everything"]}])",
      R"("unknown": 1)",
  };
  for (const char* body : bad) {
    const SkillParseResult parsed = ParseSiteSkills(OneSkill(body));
    EXPECT_TRUE(parsed.skills.empty()) << body;
    EXPECT_EQ(parsed.errors.size(), 1u) << body;
  }
  // Too many alternatives, or choices.
  std::string many = R"("search": {"field": [)";
  for (int i = 0; i < 13; ++i) {
    many += std::string(i ? "," : "") + R"({"role": "button"})";
  }
  EXPECT_TRUE(ParseSiteSkills(OneSkill(many + "]}")).skills.empty());
  EXPECT_TRUE(ParseSiteSkills(OneSkill(
                  R"("search": {"field": [{"id": "a|b|c|d|e|f|g|h|i|j|k|l|m"}]})"))
                  .skills.empty());
  // A good one parses.
  const SkillParseResult good = ParseSiteSkills(OneSkill(
      R"("search": {"field": [{"role": "searchbox|textfield", "name_has": "Search!",
                              "inside": {"role": "search"}}]})"));
  ASSERT_EQ(good.skills.size(), 1u);
  const SkillSelector& selector = good.skills[0].search_field[0];
  EXPECT_EQ(selector.name_has, (std::vector<std::string>{"search"}));
  EXPECT_EQ(selector.roles,
            (uint64_t{1} << static_cast<int>(PageRole::kSearchBox)) |
                (uint64_t{1} << static_cast<int>(PageRole::kTextField)));
  ASSERT_EQ(selector.inside.size(), 1u);
}

TEST(SiteSkillsTest, TheFileItself) {
  EXPECT_FALSE(ParseSiteSkills("").errors.empty());
  EXPECT_FALSE(ParseSiteSkills("[]").errors.empty());
  EXPECT_FALSE(ParseSiteSkills(R"({"format": 2, "skills": []})").errors.empty());
  EXPECT_FALSE(
      ParseSiteSkills(R"({"format": 1, "skills": [], "x": 1})").errors.empty());
  EXPECT_FALSE(ParseSiteSkills(std::string(kMaxSkillsJsonBytes + 1, ' '))
                   .errors.empty());
  // One bad skill leaves the others; a duplicate id is refused.
  const SkillParseResult mixed = ParseSiteSkills(R"({"format": 1, "skills": [
      {"id": "a", "version": 1, "name": "A", "hosts": ["a.example"]},
      {"id": "b", "version": 1, "name": "B", "hosts": ["B.example"]},
      {"id": "a", "version": 2, "name": "A2", "hosts": ["a2.example"]},
      {"id": "c", "version": 1, "name": "C"}]})");
  ASSERT_EQ(mixed.skills.size(), 1u);
  EXPECT_EQ(mixed.skills[0].id, "a");
  EXPECT_EQ(mixed.errors.size(), 3u);
}

// --- The sites --------------------------------------------------------------

TEST(SiteSkillsTest, YouTubeResults) {
  const PageTree tree = Fixture("youtube_results.html");
  SkillReport report;
  const PageMap map = MapWith(tree, "youtube", "/results", &report);
  EXPECT_EQ(report.skill, "youtube@1");
  EXPECT_EQ(report.missing, 0u);
  EXPECT_EQ(map.site.source, "youtube@1");

  const PageItem* field = Item(map, map.site.search_field);
  ASSERT_TRUE(field);
  EXPECT_EQ(field->kind, ItemKind::kTextField);
  EXPECT_EQ(field->label, "Search");
  const PageItem* submit = Item(map, map.site.search_submit);
  ASSERT_TRUE(submit);
  EXPECT_EQ(submit->label, "Search");
  EXPECT_TRUE(map.site.next_page_scrolls);

  ASSERT_TRUE(map.site.results);
  const PageList& results = map.lists[0];
  EXPECT_TRUE(results.from_site);
  EXPECT_EQ(results.label, "Videos");
  // The eight results and the shelf's two, in reading order.
  ASSERT_EQ(results.entries.size(), 10u);
  EXPECT_EQ(report.results, 10u);
  const PageListEntry& first = results.entries[0];
  EXPECT_EQ(first.title,
            "Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video Remastered)");
  EXPECT_EQ(first.channel, "Queen Official");
  EXPECT_EQ(first.duration_seconds, 359);
  EXPECT_EQ(first.date, "15 years ago");
  ASSERT_FALSE(first.counts.empty());
  EXPECT_EQ(first.counts[0].unit, "views");
  EXPECT_EQ(results.entries[4].title, "Bohemian Rhapsody on Classical Guitar");
  EXPECT_EQ(results.entries[6].title,
            "Bohemian Rhapsody | Muppet Music Video | The Muppets");
  // Each entry's action is its title link, which the map knows as its entry.
  for (size_t e = 0; e < results.entries.size(); ++e) {
    const PageItem* link = map.ItemByNumber(results.entries[e].number);
    ASSERT_TRUE(link) << e;
    EXPECT_EQ(link->kind, ItemKind::kLink);
    EXPECT_EQ(link->list, 0);
    EXPECT_EQ(link->entry, static_cast<int>(e));
  }
  // No other list repeats them.
  for (size_t l = 1; l < map.lists.size(); ++l) {
    EXPECT_FALSE(map.lists[l].from_site);
    for (const PageListEntry& entry : map.lists[l].entries) {
      EXPECT_NE(entry.title, first.title);
    }
  }

  // Read as the design reads them.
  const std::vector<std::string> said =
      ListSpeech(CandidatesFromList(map, results, false), "result", "results",
                 Verbosity::kNormal, kNormalResultsRead);
  std::string all;
  for (const std::string& chunk : said) {
    all += chunk + " ";
  }
  EXPECT_NE(all.find("10 results."), std::string::npos) << all;
  EXPECT_NE(all.find("1: Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video "
                     "Remastered), Queen Official, 6 minutes."),
            std::string::npos)
      << all;
}

TEST(SiteSkillsTest, YouTubeWithoutTheSkill) {
  // The generic map on the same page: its results list may differ, and the
  // site's parts are left empty.
  const PageMap map = BuildPageMap(Fixture("youtube_results.html"));
  EXPECT_TRUE(map.site.source.empty());
  EXPECT_FALSE(map.site.results);
  EXPECT_FALSE(map.site.search_field.valid());
  for (const PageList& list : map.lists) {
    EXPECT_FALSE(list.from_site);
  }
}

TEST(SiteSkillsTest, YouTubeHome) {
  SkillReport report;
  const PageMap map =
      MapWith(Fixture("youtube_home.html"), "youtube", "/", &report);
  ASSERT_TRUE(map.site.results);
  EXPECT_EQ(map.lists[0].entries.size(), 4u);
  EXPECT_TRUE(map.site.search_field.valid());
}

TEST(SiteSkillsTest, YouTubeWatch) {
  SkillReport report;
  const PageMap map =
      MapWith(Fixture("youtube_watch.html"), "youtube", "/watch", &report);
  EXPECT_EQ(report.missing, 0u);
  const PageItem* media = Item(map, map.site.media);
  ASSERT_TRUE(media);
  EXPECT_EQ(media->kind, ItemKind::kMedia);
  const PageItem* skip = Item(map, map.site.skip_ad);
  ASSERT_TRUE(skip);
  EXPECT_EQ(skip->label, "Skip");
  const PageItem* captions = Item(map, map.site.captions);
  ASSERT_TRUE(captions);
  EXPECT_EQ(captions->label, "Subtitles/closed captions (c)");
  EXPECT_FALSE(captions->checked);
  const PageItem* fullscreen = Item(map, map.site.fullscreen);
  ASSERT_TRUE(fullscreen);
  EXPECT_EQ(fullscreen->label, "Full screen (f)");
}

TEST(SiteSkillsTest, Google) {
  SkillReport report;
  const PageMap map =
      MapWith(Fixture("google_results.html"), "google", "/search", &report);
  EXPECT_EQ(report.missing, 0u);
  const PageItem* field = Item(map, map.site.search_field);
  ASSERT_TRUE(field);
  EXPECT_EQ(field->value, "bohemian rhapsody");
  ASSERT_TRUE(map.site.results);
  // The sponsored result is left out.
  EXPECT_EQ(Titles(map.lists[0]),
            (std::vector<std::string>{
                "Bohemian Rhapsody - Wikipedia",
                "Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video "
                "Remastered)",
                "Bohemian Rhapsody (film) - Wikipedia",
                "Bohemian Rhapsody Lyrics",
                "The story behind Bohemian Rhapsody",
                "Bohemian Rhapsody - Queen | Song Info",
            }));
  EXPECT_EQ(map.lists[0].entries[0].channel, "Wikipedia");
  const PageItem* next = Item(map, map.site.next_page);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->kind, ItemKind::kLink);
  EXPECT_EQ(next->label, "Next");
}

TEST(SiteSkillsTest, DuckDuckGo) {
  SkillReport report;
  const PageMap map =
      MapWith(Fixture("duckduckgo_results.html"), "duckduckgo", "/", &report);
  EXPECT_EQ(report.missing, 0u);
  EXPECT_TRUE(Item(map, map.site.search_field));
  EXPECT_TRUE(Item(map, map.site.search_submit));
  ASSERT_TRUE(map.site.results);
  ASSERT_EQ(map.lists[0].entries.size(), 5u);
  EXPECT_EQ(map.lists[0].entries[0].title, "Bohemian Rhapsody - Wikipedia");
  const PageItem* more = Item(map, map.site.next_page);
  ASSERT_TRUE(more);
  EXPECT_EQ(more->kind, ItemKind::kButton);
  EXPECT_EQ(more->label, "More results");
}

TEST(SiteSkillsTest, Bing) {
  SkillReport report;
  const PageMap map =
      MapWith(Fixture("bing_results.html"), "bing", "/search", &report);
  EXPECT_EQ(report.missing, 0u);
  EXPECT_TRUE(Item(map, map.site.search_field));
  ASSERT_TRUE(map.site.results);
  ASSERT_EQ(map.lists[0].entries.size(), 4u);
  EXPECT_EQ(map.lists[0].entries[1].title,
            "Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video Remastered) "
            "- YouTube");
  const PageItem* next = Item(map, map.site.next_page);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->label, "Next page");
}

TEST(SiteSkillsTest, WikipediaSearch) {
  SkillReport report;
  const PageMap map = MapWith(Fixture("wikipedia_search.html"), "wikipedia",
                              "/w/index.php", &report);
  EXPECT_EQ(report.missing, 0u);
  ASSERT_TRUE(map.site.results);
  EXPECT_EQ(Titles(map.lists[0]),
            (std::vector<std::string>{"Queen (band)", "Queen Elizabeth II",
                                      "Queen (chess)",
                                      "Queen of the Night aria"}));
  const PageItem* next = Item(map, map.site.next_page);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->label, "next 20");
}

TEST(SiteSkillsTest, WikipediaArticle) {
  const PageTree tree = Fixture("wikipedia_article.html");
  SkillReport report;
  const PageMap map = MapWith(tree, "wikipedia", "/wiki/Queen_(band)", &report);
  EXPECT_EQ(report.missing, 0u);
  const PageItem* field = Item(map, map.site.search_field);
  ASSERT_TRUE(field);
  EXPECT_EQ(field->label, "Search Wikipedia");
  EXPECT_FALSE(map.site.results);

  const SkillReadingScope scope =
      ReadingScopeFor(Skill("wikipedia"), tree, PageMapLimits());
  ASSERT_TRUE(scope.root.valid());
  // The article's main landmark (mw-content-text wraps blocks only, so the
  // tree has no id for it).
  bool root_is_main = false;
  for (const PageNode& node : tree.nodes) {
    if (node.handle == scope.root) {
      root_is_main = node.role == PageRole::kMain;
    }
  }
  EXPECT_TRUE(root_is_main);
  // Edit links (4), reference marks (2), the infobox, the references, the
  // navbox, the categories and the jump link.
  EXPECT_EQ(scope.skip.size(), 11u);
}

TEST(SiteSkillsTest, ABrokenSkillFallsBackAndIsReported) {
  const SkillParseResult parsed = ParseSiteSkills(R"({"format": 1, "skills": [
      {"id": "stale", "version": 3, "name": "Stale", "hosts": ["s.example"],
       "search": {"field": [{"role": "searchbox", "id": "gone"}]},
       "results": {"title": [{"role": "link", "id": "gone-too"}]},
       "pages": [{"path": "/results", "expect": ["search", "results"]}]}]})");
  ASSERT_EQ(parsed.skills.size(), 1u);
  const PageTree tree = Fixture("youtube_results.html");
  SkillReport report;
  const PageMap map = BuildPageMapWithSkills(
      tree, PageMapLimits(), &parsed.skills[0], nullptr, "/results", &report);
  EXPECT_EQ(report.missing, kPartSearch | kPartResults);
  EXPECT_EQ(report.from_skill, 0u);
  EXPECT_TRUE(map.site.source.empty());
  // The generic map, as without a skill.
  const PageMap generic = BuildPageMap(tree);
  ASSERT_EQ(map.lists.size(), generic.lists.size());
  ASSERT_FALSE(map.lists.empty());
  EXPECT_EQ(Titles(map.lists[0]), Titles(generic.lists[0]));
  // On another page, nothing is expected.
  BuildPageMapWithSkills(tree, PageMapLimits(), &parsed.skills[0], nullptr,
                         "/watch", &report);
  EXPECT_EQ(report.missing, 0u);
}

TEST(SiteSkillsTest, AHintFillsWhatTheSkillMisses) {
  // A skill that knows only the search box, and a learned hint that knows
  // the results (in the same form).
  const SkillParseResult parsed = ParseSiteSkills(R"({"format": 1, "skills": [
      {"id": "partial", "version": 1, "name": "P", "hosts": ["p.example"],
       "search": {"field": [{"role": "combobox", "name": "search"}]}},
      {"id": "hint", "version": 1, "name": "H", "hosts": ["p.example"],
       "search": {"field": [{"role": "textfield"}]},
       "results": {"title": [{"role": "link", "id": "video-title"}]}}]})");
  ASSERT_EQ(parsed.skills.size(), 2u);
  SkillReport report;
  const PageMap map = BuildPageMapWithSkills(
      Fixture("youtube_results.html"), PageMapLimits(), &parsed.skills[0],
      &parsed.skills[1], "/", &report);
  EXPECT_EQ(report.from_skill, kPartSearch);
  EXPECT_EQ(report.from_hint, kPartResults);
  EXPECT_EQ(map.site.source, "partial@1");
  ASSERT_TRUE(map.site.results);
  EXPECT_EQ(map.lists[0].entries.size(), 10u);
}

TEST(SiteSkillsTest, NeverAFramesNode) {
  // A frame (another site's) holds a search box and title links the skill
  // would name.
  test::TreeBuilder b("Page");
  const uint32_t root = b.Add(test::TreeBuilder::kRoot, PageRole::kRootWebArea);
  b.Link(root, "Home", "https://p.example/");
  const uint32_t host = b.Add(root, PageRole::kIframe);
  b.SetTree(1);
  const uint32_t frame_root = b.Add(host, PageRole::kRootWebArea);
  const uint32_t field = b.Add(frame_root, PageRole::kSearchBox, "Search",
                               kStateFocusable | kStateEditable);
  b.node(field).html_id = "search";
  for (int i = 0; i < 3; ++i) {
    const uint32_t li = b.Add(frame_root, PageRole::kListItem);
    const uint32_t link = b.Link(li, "Ad video", "https://ads.example/");
    b.node(link).html_id = "video-title";
  }
  SkillReport report;
  const PageMap map =
      MapWith(b.Take(), "youtube", "/results", &report);
  EXPECT_FALSE(map.site.search_field.valid());
  EXPECT_FALSE(map.site.results);
  EXPECT_EQ(report.missing, kPartSearch | kPartResults);
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
