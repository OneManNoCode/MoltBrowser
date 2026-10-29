// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Hand-built page trees for the page map's tests: what Chromium's
// accessibility tree holds for a YouTube-like search results page (custom
// elements and divs, not tidy ARIA), a Google-like results page, a
// Wikipedia article, a sign-in form with a password field and a page with
// a cross-origin frame. Ignored nodes (custom elements, wrapper divs) are in
// the trees as Blink includes them: as ignored generic containers, kept for
// structure.
//
// Test-only; std-only.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_TEST_TREES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_TEST_TREES_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/page_map.h"

namespace molt_ai {
namespace voice {
namespace test {

// Builds a PageTree node by node. Node ids count up from 1 in each tree.
class TreeBuilder {
 public:
  explicit TreeBuilder(std::string title = std::string());
  ~TreeBuilder();

  // Nodes added from now on belong to tree |tree| (a frame's tree).
  void SetTree(int32_t tree);

  // Adds a node under |parent| (kRoot for the first node); returns its
  // index.
  static constexpr uint32_t kRoot = 0xFFFFFFFFu;
  uint32_t Add(uint32_t parent,
               PageRole role,
               std::string_view name = std::string_view(),
               uint32_t states = 0);
  // An ignored generic container (a custom element or wrapper div).
  uint32_t Wrapper(uint32_t parent);
  uint32_t Text(uint32_t parent, std::string_view text);
  // A link with visible text (a static text child); |label| is its
  // accessible name when it differs (an aria-label), else the text.
  uint32_t Link(uint32_t parent,
                std::string_view text,
                std::string_view url,
                std::string_view label = std::string_view());
  uint32_t Heading(uint32_t parent, int level, std::string_view text);
  uint32_t Button(uint32_t parent,
                  std::string_view name,
                  uint32_t states = 0);

  PageNode& node(uint32_t index);
  void SetBounds(uint32_t index, float x, float y, float width, float height);
  // Links |child| (already added elsewhere, e.g. a frame's root) under
  // |parent| too.
  void AppendChild(uint32_t parent, uint32_t child);
  void SetFocus(uint32_t index);

  size_t size() const { return tree_.nodes.size(); }
  const PageTree& tree() const { return tree_; }
  PageTree Take();

 private:
  PageTree tree_;
  int32_t tree_index_ = 0;
  int32_t next_id_[8] = {1, 1, 1, 1, 1, 1, 1, 1};
};

// The viewport the fixtures lay out in: 1280 by 800.
inline constexpr float kViewportWidth = 1280;
inline constexpr float kViewportHeight = 800;

// What the YouTube-like fixture's 20 results are.
struct VideoFixture {
  std::string_view title;
  std::string_view channel;
  std::string_view duration;
  int duration_seconds;
  std::string_view views;
  int64_t view_count;
  std::string_view age;
};
inline constexpr int kVideoResults = 20;
const VideoFixture& VideoResult(int index);  // 0-based

// youtube.com/results?search_query=bohemian+rhapsody, as its accessibility
// tree reads: the masthead with the search box, the guide (a column of
// links), the filter chips, 20 video renderers (thumbnail link with the
// duration, h3 > title link with a long aria-label, channel avatar and name
// links, metadata line, description, badges, an action menu button) and a
// shelf of four other videos after the fifth result. Results below the
// first three are off screen.
PageTree YouTubeLikeResults();

// google.com/search?q=bohemian+rhapsody: the search form, the filter
// links, ten results (a link holding the h3 title, the site name and the
// breadcrumb address; an "About this result" button; a snippet, some with
// a date) and a "People also ask" block, in the main landmark.
inline constexpr int kGoogleResults = 10;
PageTree GoogleLikeResults();

// en.wikipedia.org/wiki/Bohemian_Rhapsody: a banner with a search
// landmark, the site navigation, the article (headings, paragraphs with
// inline links, the table of contents, an infobox table, references) and
// the footer.
PageTree WikipediaArticle();

// A sign-in form: an email field, a password field (protected, with a
// value), a one-time code field, a card number field, a PIN field named
// only by its label, a "Remember me" check box, a submit button and a
// "Forgot password?" link.
PageTree PasswordForm();

// A page with a frame from another site: the host's link, the frame's
// button and link (tree 1), then a button after the frame.
PageTree CrossOriginFrames();

}  // namespace test
}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_PAGE_MAP_TEST_TREES_H_
