// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice loop's decisions with site skills (site_skills.h) and reading
// aloud (page_reading.h), on the stored copies of the sites' pages
// (chrome/test/data/molt_voice/skills/): "search YouTube for X" fills
// YouTube's box, results are read as the skill names them, "skip the ad",
// "next page" and captions use the site's own controls, and the reading
// commands become the reader's steps, keeping its place.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "chrome/browser/molt_ai/voice/html_fixture_tree.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/site_skills.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using K = VoiceStepKind;
using Reading = VoiceEnvironment::Reading;

PageTree Fixture(std::string_view name) {
  base::FilePath root;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
  std::string html;
  EXPECT_TRUE(base::ReadFileToString(root.AppendASCII("chrome")
                                         .AppendASCII("test")
                                         .AppendASCII("data")
                                         .AppendASCII("molt_voice")
                                         .AppendASCII("skills")
                                         .AppendASCII(name),
                                     &html))
      << name;
  return test::PageTreeFromHtml(html);
}

// |name|'s map as the page controller makes it on |host|.
PageMap SiteMap(std::string_view name,
                std::string_view host,
                std::string_view path) {
  return BuildPageMapWithSkills(Fixture(name), PageMapLimits(),
                                BuiltInSkillForHost(host), nullptr, path);
}

VoiceEnvironment Env(std::string_view host) {
  VoiceEnvironment env;
  env.page_id = 1;
  env.host = std::string(host);
  env.web_page = true;
  env.can_go_back = true;
  env.now_ms = 1000000;
  return env;
}

std::string Said(const VoiceTurn& turn) {
  std::string out;
  for (const VoiceStep& step : turn.steps) {
    if (step.kind == K::kSpeak) {
      out += (out.empty() ? "" : "|") + step.text;
    }
  }
  return out;
}

std::vector<VoiceStepKind> Actions(const VoiceTurn& turn) {
  std::vector<VoiceStepKind> kinds;
  for (const VoiceStep& step : turn.steps) {
    if (step.kind != K::kSpeak && step.kind != K::kSetCurrentItem) {
      kinds.push_back(step.kind);
    }
  }
  return kinds;
}

// The item the turn's first step of |kind| acts on.
const PageItem* Target(const PageMap& map,
                       const VoiceTurn& turn,
                       VoiceStepKind kind) {
  for (const VoiceStep& step : turn.steps) {
    if (step.kind == kind) {
      return map.ItemForHandle(step.target);
    }
  }
  return nullptr;
}

VoiceTurn Say(std::string_view utterance,
              const PageMap& map,
              const VoiceEnvironment& env,
              ConversationState* state) {
  VoiceTurn turn = PlanUtterance(utterance, map, env, *state);
  *state = turn.state;
  return turn;
}

TEST(VoiceSkillsLogicTest, SearchYouTubeForFillsItsBox) {
  const PageMap home = SiteMap("youtube_home.html", "www.youtube.com", "/");
  ConversationState state;
  VoiceTurn turn =
      Say("search YouTube for queen live aid", home, Env("www.youtube.com"),
          &state);
  ASSERT_EQ(Actions(turn),
            (std::vector<VoiceStepKind>{K::kSetText, K::kPress,
                                        K::kWaitForPage}));
  EXPECT_EQ(turn.steps[0].target, home.site.search_field);
  EXPECT_EQ(turn.steps[0].text, "queen live aid");
  EXPECT_EQ(turn.steps[1].target, home.site.search_submit);
  // Elsewhere, YouTube's search page.
  turn = Say("search YouTube for queen live aid",
             SiteMap("google_results.html", "www.google.com", "/search"),
             Env("www.google.com"), &state);
  ASSERT_EQ(Actions(turn),
            (std::vector<VoiceStepKind>{K::kNavigate, K::kWaitForPage}));
  EXPECT_EQ(turn.steps[0].url,
            "https://www.youtube.com/results?search_query=queen+live+aid");
  // "Search for" on YouTube: its box too.
  turn = Say("search for bohemian rhapsody", home, Env("www.youtube.com"),
             &state);
  EXPECT_EQ(turn.steps[0].target, home.site.search_field);
}

TEST(VoiceSkillsLogicTest, TheResultsAsTheSkillNamesThem) {
  const PageMap results =
      SiteMap("youtube_results.html", "www.youtube.com", "/results");
  ConversationState state;
  const VoiceEnvironment env = Env("www.youtube.com");
  VoiceTurn turn = Say("read the results", results, env, &state);
  const std::string said = Said(turn);
  EXPECT_EQ(said.find("10 results."), 0u) << said;
  EXPECT_NE(said.find("1: Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video "
                      "Remastered), Queen Official, 6 minutes."),
            std::string::npos)
      << said;
  // "Play the fourth one" plays the fourth video.
  turn = Say("play the fourth one", results, env, &state);
  ASSERT_EQ(Actions(turn),
            (std::vector<VoiceStepKind>{K::kPress, K::kWaitForPage}));
  const PageItem* link = Target(results, turn, K::kPress);
  ASSERT_TRUE(link);
  EXPECT_EQ(link->number, results.lists[0].entries[3].number);
  // The fifth is the shelf's first: what the page shows next.
  turn = Say("play number 5", results, env, &state);
  link = Target(results, turn, K::kPress);
  ASSERT_TRUE(link);
  EXPECT_EQ(link->number, results.lists[0].entries[4].number);
  // More results come as the page scrolls.
  turn = Say("next page", results, env, &state);
  ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kScrollPage}));
  EXPECT_NE(Said(turn).find("more results as you scroll"), std::string::npos);
}

TEST(VoiceSkillsLogicTest, NextPageOnSearchEngines) {
  ConversationState state;
  const PageMap google =
      SiteMap("google_results.html", "www.google.com", "/search");
  VoiceTurn turn = Say("next page", google, Env("www.google.com"), &state);
  ASSERT_EQ(Actions(turn),
            (std::vector<VoiceStepKind>{K::kPress, K::kWaitForPage}));
  EXPECT_EQ(turn.steps[0].target, google.site.next_page);
  const PageMap ddg =
      SiteMap("duckduckgo_results.html", "duckduckgo.com", "/");
  turn = Say("more results", ddg, Env("duckduckgo.com"), &state);
  ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kPress}));
  EXPECT_EQ(turn.steps[0].target, ddg.site.next_page);
  const PageMap bing = SiteMap("bing_results.html", "www.bing.com", "/search");
  turn = Say("next page", bing, Env("www.bing.com"), &state);
  EXPECT_EQ(turn.steps[0].target, bing.site.next_page);
}

TEST(VoiceSkillsLogicTest, ThePlayersControls) {
  const PageMap watch =
      SiteMap("youtube_watch.html", "www.youtube.com", "/watch");
  ConversationState state;
  VoiceEnvironment env = Env("www.youtube.com");
  VoiceTurn turn = Say("skip the ad", watch, env, &state);
  ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kPress}));
  EXPECT_EQ(turn.steps[0].target, watch.site.skip_ad);
  EXPECT_EQ(Said(turn), "Skipped the ad.");
  turn = Say("turn on captions", watch, env, &state);
  ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kPress}));
  EXPECT_EQ(turn.steps[0].target, watch.site.captions);
  turn = Say("full screen", watch, env, &state);
  EXPECT_EQ(turn.steps[0].target, watch.site.fullscreen);
  EXPECT_EQ(turn.steps[0].intent, activation::Intent::kFullScreen);
  turn = Say("play", watch, env, &state);
  ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kPlayMedia}));
  EXPECT_EQ(turn.steps[0].target, watch.site.media);

  // Without a skip button (or a skill): said so.
  turn = Say("skip the ad",
             SiteMap("youtube_results.html", "www.youtube.com", "/results"),
             env, &state);
  EXPECT_TRUE(Actions(turn).empty());
  EXPECT_EQ(Said(turn).find("There's no skip button now."), 0u);
  // A disabled one can't be pressed yet.
  PageMap waiting = watch;
  for (PageItem& item : waiting.items) {
    if (item.handle == waiting.site.skip_ad) {
      item.disabled = true;
    }
  }
  turn = Say("skip the ad", waiting, env, &state);
  EXPECT_TRUE(Actions(turn).empty());
}

TEST(VoiceSkillsLogicTest, ASitePartMustBeThePagesOwnItem) {
  PageMap watch = SiteMap("youtube_watch.html", "www.youtube.com", "/watch");
  ConversationState state;
  // A part naming nothing, or another tree's node, isn't used.
  watch.site.skip_ad = PageNodeHandle{7, 12345};
  watch.site.search_field = watch.site.captions;  // not a text field
  VoiceTurn turn = Say("skip the ad", watch, Env("www.youtube.com"), &state);
  EXPECT_TRUE(Actions(turn).empty());
  EXPECT_EQ(FindSearchField(watch)->label, "Search");
}

TEST(VoiceSkillsLogicTest, ReadingCommandsAreTheReadersSteps) {
  const PageMap article =
      SiteMap("wikipedia_article.html", "en.wikipedia.org", "/wiki/Queen");
  ConversationState state;
  VoiceEnvironment env = Env("en.wikipedia.org");
  struct {
    std::string_view said;
    ReadOp op;
    std::string_view text;
  } const kCases[] = {
      {"read the page", ReadOp::kPage, ""},
      {"read from here", ReadOp::kFromHere, ""},
      {"continue reading", ReadOp::kFromHere, ""},
      {"read the selection", ReadOp::kSelection, ""},
      {"read the section on history", ReadOp::kSection, "history"},
      {"next heading", ReadOp::kNextHeading, ""},
      {"previous heading", ReadOp::kPreviousHeading, ""},
      {"next paragraph", ReadOp::kNextParagraph, ""},
      {"previous paragraph", ReadOp::kPreviousParagraph, ""},
  };
  for (const auto& c : kCases) {
    SCOPED_TRACE(c.said);
    const VoiceTurn turn = Say(c.said, article, env, &state);
    ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kRead}));
    EXPECT_EQ(turn.steps.back().read, c.op);
    EXPECT_EQ(turn.steps.back().text, c.text);
  }
  // Not in MoltNet windows.
  env.moltnet = true;
  EXPECT_TRUE(Actions(Say("read the page", article, env, &state)).empty());
}

TEST(VoiceSkillsLogicTest, PauseContinueAndRateWhileReading) {
  const PageMap article =
      SiteMap("wikipedia_article.html", "en.wikipedia.org", "/wiki/Queen");
  ConversationState state;
  VoiceEnvironment env = Env("en.wikipedia.org");
  env.reading = Reading::kReading;
  // "Pause" while it reads: its place is kept (the command stopped it).
  VoiceTurn turn = Say("pause", article, env, &state);
  EXPECT_TRUE(Actions(turn).empty());
  EXPECT_EQ(Said(turn), "Paused. Say continue reading to go on.");
  turn = Say("pause reading", article, env, &state);
  EXPECT_TRUE(Actions(turn).empty());
  // "Continue" afterwards goes on reading, not playing.
  env.reading = Reading::kStopped;
  turn = Say("continue", article, env, &state);
  ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kRead}));
  EXPECT_EQ(turn.steps.back().read, ReadOp::kResume);
  // ...unless something has played since.
  env.media_session = true;
  turn = Say("continue", article, env, &state);
  EXPECT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kMedia}));
  env.media_session = false;

  // "Faster" while reading: the reading's pace, then it goes on.
  env.reading = Reading::kReading;
  const double before = state.speech_rate;
  turn = Say("faster", article, env, &state);
  EXPECT_GT(state.speech_rate, before);
  EXPECT_EQ(Said(turn), "Faster.");
  ASSERT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kRead}));
  EXPECT_EQ(turn.steps.back().read, ReadOp::kResume);
  turn = Say("read slower", article, env, &state);
  EXPECT_EQ(state.speech_rate, before);
  EXPECT_EQ(Actions(turn), (std::vector<VoiceStepKind>{K::kRead}));
  // Stopped: the rate changes and the reading waits for "continue".
  env.reading = Reading::kStopped;
  turn = Say("slower", article, env, &state);
  EXPECT_LT(state.speech_rate, before);
  EXPECT_TRUE(Actions(turn).empty());
  // Not reading: "faster" is still the playback speed.
  env.reading = Reading::kNone;
  turn = Say("faster", article, env, &state);
  EXPECT_EQ(Said(turn), "Changing the playback speed comes later.");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
