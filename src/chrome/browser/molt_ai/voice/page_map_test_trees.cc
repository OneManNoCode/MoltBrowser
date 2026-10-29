// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/page_map_test_trees.h"

#include <array>
#include <utility>

namespace molt_ai {
namespace voice {
namespace test {

namespace {

constexpr std::array<VideoFixture, kVideoResults> kVideos = {{
    {"Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video Remastered)",
     "Queen Official", "6:00", 360, "2.1B views", 2100000000, "15 years ago"},
    {"Bohemian Rhapsody (Remastered 2011)", "Queen - Topic", "5:55", 355,
     "312M views", 312000000, "11 years ago"},
    {"Queen - Bohemian Rhapsody (Live at Rock Montreal 1981)", "Queen Official",
     "5:44", 344, "48M views", 48000000, "3 years ago"},
    {"Queen - Bohemian Rhapsody (Live Aid 1985)", "Queen Official", "2:31", 151,
     "97M views", 97000000, "7 years ago"},
    {"Bohemian Rhapsody | Muppet Music Video | The Muppets", "The Muppets",
     "4:47", 287, "88M views", 88000000, "15 years ago"},
    {"Wayne's World - Bohemian Rhapsody scene", "Paramount Movies", "3:12", 192,
     "21M views", 21000000, "9 years ago"},
    {"Bohemian Rhapsody | Official Trailer [HD] | 20th Century FOX",
     "20th Century Studios", "2:32", 152, "31M views", 31000000, "6 years ago"},
    {"Panic! At The Disco - Bohemian Rhapsody (Official Video)",
     "Fueled By Ramen", "6:08", 368, "60M views", 60000000, "9 years ago"},
    {"Bohemian Rhapsody - Pentatonix", "Pentatonix", "5:57", 357, "45M views",
     45000000, "7 years ago"},
    {"Bohemian Rhapsody | Piano Tutorial", "Piano Lessons Daily", "8:15", 495,
     "1.2M views", 1200000, "2 years ago"},
    {"Bohemian Rhapsody (Lyrics)", "Lyric Vibes", "5:59", 359, "18M views",
     18000000, "4 years ago"},
    {"Queen - Bohemian Rhapsody (Live at Wembley Stadium 1986)",
     "Queen Official", "5:58", 358, "35M views", 35000000, "5 years ago"},
    {"Bohemian Rhapsody - Rami Malek Live Aid scene", "Movieclips", "4:20", 260,
     "12M views", 12000000, "5 years ago"},
    {"How Queen wrote Bohemian Rhapsody", "Polyphonic", "11:46", 706,
     "6.4M views", 6400000, "6 years ago"},
    {"Bohemian Rhapsody - Marching Band", "Ohio State Marching Band", "4:02",
     242, "9.1M views", 9100000, "8 years ago"},
    {"Bohemian Rhapsody on Classical Guitar", "Marcin", "6:12", 372,
     "14M views", 14000000, "3 years ago"},
    {"Bohemian Rhapsody - Kanye West at Glastonbury 2015", "BBC Music", "1:45",
     105, "8.3M views", 8300000, "9 years ago"},
    {"Bohemian Rhapsody - Queen + Adam Lambert (Rock in Rio 2015)",
     "Queen Official", "6:21", 381, "5.2M views", 5200000, "1 year ago"},
    {"The Story of Bohemian Rhapsody (BBC documentary)", "BBC", "58:43", 3523,
     "2.7M views", 2700000, "10 months ago"},
    {"Bohemian Rhapsody but it's lofi", "Lofi Covers", "1:02:15", 3735,
     "940K views", 940000, "2 weeks ago"},
}};

std::string Slug(std::string_view channel) {
  std::string out;
  for (char c : channel) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out.push_back(c);
    } else if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    }
  }
  return out;
}

// Marks |index| and everything below it as off screen.
void MarkOffscreen(TreeBuilder& b, uint32_t index) {
  std::vector<uint32_t> stack = {index};
  while (!stack.empty()) {
    const uint32_t i = stack.back();
    stack.pop_back();
    b.node(i).states |= kStateOffscreen;
    for (uint32_t child : b.node(i).children) {
      stack.push_back(child);
    }
  }
}

// One ytd-video-renderer.
uint32_t AddVideo(TreeBuilder& b, uint32_t parent, int index, float y) {
  const VideoFixture& v = kVideos[index];
  const std::string watch = "/watch?v=video" + std::to_string(index + 1);
  const std::string channel_url = "/@" + Slug(v.channel);
  const uint32_t renderer = b.Wrapper(parent);  // ytd-video-renderer
  b.SetBounds(renderer, 264, y, 1000, 220);
  const uint32_t dismissible = b.Wrapper(renderer);
  // The thumbnail: an image link with the duration overlaid.
  const uint32_t thumbnail = b.Wrapper(dismissible);  // ytd-thumbnail
  const uint32_t thumb_link =
      b.Add(thumbnail, PageRole::kLink, "", kStateFocusable);
  b.node(thumb_link).url = watch;
  b.SetBounds(thumb_link, 264, y, 360, 202);
  b.Add(thumb_link, PageRole::kImage, "", kStateIgnored);  // alt=""
  const uint32_t overlays = b.Wrapper(thumb_link);
  b.Text(b.Wrapper(overlays), v.duration);
  // The text column.
  const uint32_t text_wrapper = b.Wrapper(dismissible);
  const uint32_t meta = b.Wrapper(text_wrapper);
  const uint32_t h3 = b.Add(meta, PageRole::kHeading, "", 0);
  b.node(h3).heading_level = 3;
  // The title link's aria-label says everything; its text is the title.
  const std::string aria = std::string(v.title) + " by " +
                           std::string(v.channel) + " " + std::string(v.views) +
                           " " + std::string(v.age);
  b.node(h3).name = aria;
  const uint32_t title = b.Link(h3, v.title, watch, aria);
  b.node(title).description = std::string(v.title);  // the title attribute
  b.SetBounds(title, 640, y + 4, 600, 26);
  const uint32_t meta_block = b.Wrapper(meta);  // ytd-video-meta-block
  const uint32_t line = b.Wrapper(meta_block);  // #metadata-line
  b.Text(b.Wrapper(line), v.views);
  b.Text(b.Wrapper(line), v.age);
  // The channel: an avatar link (aria-label only) and the name link.
  const uint32_t channel_info = b.Wrapper(text_wrapper);
  const uint32_t avatar =
      b.Add(channel_info, PageRole::kLink,
            "Go to channel " + std::string(v.channel), kStateFocusable);
  b.node(avatar).url = channel_url;
  b.Add(avatar, PageRole::kImage, "", kStateIgnored);
  const uint32_t channel_name = b.Wrapper(channel_info);  // ytd-channel-name
  b.Link(b.Wrapper(channel_name), v.channel, channel_url);
  if (index % 3 == 0) {
    b.Text(b.Wrapper(text_wrapper),
           "Taken from the album A Night at the Opera" +
               std::string(index == 0 ? " \xE2\x80\xA2 Remastered in 4K" : ""));
  }
  if (index % 4 == 0) {
    const uint32_t badges = b.Wrapper(text_wrapper);
    b.Text(b.Wrapper(badges), "4K");
    b.Text(b.Wrapper(badges), "CC");
  }
  // The action menu.
  const uint32_t menu = b.Wrapper(renderer);
  b.Button(menu, "Action menu");
  if (y >= kViewportHeight) {
    MarkOffscreen(b, renderer);
  }
  return renderer;
}

}  // namespace

TreeBuilder::TreeBuilder(std::string title) {
  tree_.title = std::move(title);
}

TreeBuilder::~TreeBuilder() = default;

void TreeBuilder::SetTree(int32_t tree) {
  tree_index_ = tree;
}

uint32_t TreeBuilder::Add(uint32_t parent,
                          PageRole role,
                          std::string_view name,
                          uint32_t states) {
  PageNode node;
  node.handle.tree = tree_index_;
  node.handle.node = next_id_[tree_index_ & 7]++;
  node.role = role;
  node.name = std::string(name);
  node.states = states;
  const uint32_t index = static_cast<uint32_t>(tree_.nodes.size());
  tree_.nodes.push_back(std::move(node));
  if (parent != kRoot) {
    tree_.nodes[parent].children.push_back(index);
  }
  return index;
}

uint32_t TreeBuilder::Wrapper(uint32_t parent) {
  return Add(parent, PageRole::kGeneric, "", kStateIgnored);
}

uint32_t TreeBuilder::Text(uint32_t parent, std::string_view text) {
  return Add(parent, PageRole::kStaticText, text);
}

uint32_t TreeBuilder::Link(uint32_t parent,
                           std::string_view text,
                           std::string_view url,
                           std::string_view label) {
  const uint32_t link = Add(parent, PageRole::kLink,
                            label.empty() ? text : label, kStateFocusable);
  tree_.nodes[link].url = std::string(url);
  Text(link, text);
  return link;
}

uint32_t TreeBuilder::Heading(uint32_t parent,
                              int level,
                              std::string_view text) {
  const uint32_t heading = Add(parent, PageRole::kHeading, text);
  tree_.nodes[heading].heading_level = level;
  Text(heading, text);
  return heading;
}

uint32_t TreeBuilder::Button(uint32_t parent,
                             std::string_view name,
                             uint32_t states) {
  return Add(parent, PageRole::kButton, name,
             states | kStateFocusable | kStateHasDefaultAction);
}

PageNode& TreeBuilder::node(uint32_t index) {
  return tree_.nodes[index];
}

void TreeBuilder::SetBounds(uint32_t index,
                            float x,
                            float y,
                            float width,
                            float height) {
  tree_.nodes[index].bounds = {x, y, width, height};
}

void TreeBuilder::AppendChild(uint32_t parent, uint32_t child) {
  tree_.nodes[parent].children.push_back(child);
}

void TreeBuilder::SetFocus(uint32_t index) {
  tree_.focus = tree_.nodes[index].handle;
  tree_.nodes[index].states |= kStateFocused;
}

PageTree TreeBuilder::Take() {
  return std::move(tree_);
}

const VideoFixture& VideoResult(int index) {
  return kVideos[index];
}

PageTree YouTubeLikeResults() {
  TreeBuilder b("bohemian rhapsody - YouTube");
  const uint32_t root = b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea,
                              "bohemian rhapsody - YouTube", kStateFocusable);
  b.SetBounds(root, 0, 0, kViewportWidth, kViewportHeight);
  const uint32_t app = b.Wrapper(root);  // ytd-app

  // The masthead.
  const uint32_t masthead = b.Wrapper(b.Wrapper(app));
  b.SetBounds(masthead, 0, 0, kViewportWidth, 56);
  b.Button(masthead, "Guide");
  const uint32_t logo =
      b.Add(masthead, PageRole::kLink, "YouTube Home", kStateFocusable);
  b.node(logo).url = "/";
  const uint32_t searchbox = b.Wrapper(masthead);  // ytd-searchbox
  const uint32_t form = b.Add(searchbox, PageRole::kForm, "", 0);
  const uint32_t field = b.Add(b.Wrapper(form), PageRole::kComboBox, "Search",
                               kStateFocusable | kStateEditable);
  b.node(field).value = "bohemian rhapsody";
  b.node(field).placeholder = "Search";
  b.node(field).input_type = "text";
  b.node(field).html_tag = "input";
  b.SetBounds(field, 400, 8, 480, 40);
  b.Button(searchbox, "Clear search query");
  b.Button(searchbox, "Search");
  b.Button(masthead, "Search with your voice");
  b.Button(masthead, "Settings");
  b.Link(masthead, "Sign in", "https://accounts.example/signin");

  // The guide: two sections of links.
  const uint32_t guide = b.Wrapper(b.Wrapper(app));
  b.SetBounds(guide, 0, 56, 240, 744);
  const char* const kGuide[2][4] = {
      {"Home", "Shorts", "Subscriptions", nullptr},
      {"History", "Playlists", "Watch later", "Liked videos"}};
  for (int s = 0; s < 2; ++s) {
    const uint32_t section = b.Wrapper(guide);
    if (s == 1) {
      b.Heading(section, 3, "You");
    }
    const uint32_t items = b.Wrapper(section);
    for (int e = 0; e < 4 && kGuide[s][e]; ++e) {
      const uint32_t entry = b.Wrapper(items);  // ytd-guide-entry-renderer
      b.SetBounds(entry, 12, 64 + 40.0f * (s * 4 + e), 216, 40);
      const uint32_t link =
          b.Add(entry, PageRole::kLink, kGuide[s][e], kStateFocusable);
      b.node(link).url = std::string("/feed/") + kGuide[s][e];
      b.Add(b.Wrapper(link), PageRole::kImage, "", kStateIgnored);
      b.Text(link, kGuide[s][e]);
    }
  }

  // The search results page.
  const uint32_t page = b.Wrapper(b.Wrapper(app));  // ytd-page-manager
  const uint32_t search = b.Wrapper(page);          // ytd-search
  const uint32_t chips = b.Wrapper(b.Wrapper(search));
  b.SetBounds(chips, 264, 56, 1000, 56);
  for (const char* chip : {"All", "Shorts", "Videos", "Unwatched", "Watched",
                           "Recently uploaded"}) {
    const uint32_t wrapper = b.Wrapper(chips);
    b.SetBounds(wrapper, 264, 64, 90, 32);
    b.Text(b.Button(wrapper, chip), chip);
  }
  const uint32_t sections = b.Wrapper(search);  // ytd-section-list-renderer
  const uint32_t contents = b.Wrapper(b.Wrapper(sections));
  float y = 112;
  for (int i = 0; i < kVideoResults; ++i) {
    AddVideo(b, contents, i, y);
    y += 230;
    if (i == 4) {
      // A shelf of other videos between the results.
      const uint32_t shelf = b.Wrapper(contents);  // ytd-shelf-renderer
      b.SetBounds(shelf, 264, y, 1000, 320);
      b.Heading(shelf, 2, "Latest from Queen Official");
      const uint32_t row = b.Wrapper(b.Wrapper(shelf));
      for (int k = 0; k < 4; ++k) {
        const uint32_t card = b.Wrapper(row);  // ytd-grid-video-renderer
        b.SetBounds(card, 264 + 250.0f * k, y + 40, 240, 260);
        const std::string url = "/watch?v=shelf" + std::to_string(k + 1);
        const uint32_t thumb =
            b.Add(card, PageRole::kLink, "", kStateFocusable);
        b.node(thumb).url = url;
        b.Text(b.Wrapper(thumb), "3:3" + std::to_string(k));
        const uint32_t heading = b.Add(card, PageRole::kHeading, "", 0);
        b.node(heading).heading_level = 3;
        b.Link(heading, "Queen - Shelf video " + std::to_string(k + 1), url);
        b.Link(b.Wrapper(card), "Queen Official", "/@queenofficial");
        b.Text(b.Wrapper(card), std::to_string(k + 1) + ".1M views");
        b.Text(b.Wrapper(card), std::to_string(k + 1) + " months ago");
      }
      if (y >= kViewportHeight) {
        MarkOffscreen(b, shelf);
      }
      y += 330;
    }
  }
  return b.Take();
}

PageTree GoogleLikeResults() {
  TreeBuilder b("bohemian rhapsody - Google Search");
  const uint32_t root =
      b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea,
            "bohemian rhapsody - Google Search", kStateFocusable);
  b.SetBounds(root, 0, 0, kViewportWidth, kViewportHeight);
  const uint32_t body = b.Wrapper(root);

  // The search form.
  const uint32_t search = b.Add(body, PageRole::kSearch, "", 0);
  const uint32_t form = b.Add(search, PageRole::kForm, "", 0);
  const uint32_t field =
      b.Add(b.Wrapper(form), PageRole::kComboBox, "Search",
            kStateFocusable | kStateEditable | kStateMultiline);
  b.node(field).value = "bohemian rhapsody";
  b.node(field).html_tag = "textarea";
  b.Button(form, "Clear");
  b.Button(form, "Search by voice");
  b.Button(form, "Search by image");
  const uint32_t submit = b.Button(form, "Google Search");
  b.node(submit).input_type = "submit";
  b.node(submit).html_tag = "input";

  // The filters.
  const uint32_t filters =
      b.Add(body, PageRole::kNavigation, "Filters and topics", 0);
  for (const char* f : {"All", "Images", "Videos", "News", "Shopping"}) {
    const uint32_t wrapper = b.Wrapper(filters);
    b.Link(wrapper, f, std::string("/search?tbm=") + f);
  }

  // The results.
  const uint32_t main = b.Add(body, PageRole::kMain, "", 0);
  b.Heading(main, 1, "Search Results");
  const uint32_t rso = b.Wrapper(b.Wrapper(main));
  static constexpr struct {
    const char* title;
    const char* site;
    const char* url;
    const char* snippet;
    const char* date;
  } kResults[kGoogleResults] = {
      {"Bohemian Rhapsody", "Wikipedia",
       "https://en.wikipedia.org/wiki/Bohemian_Rhapsody",
       "\"Bohemian Rhapsody\" is a song by the British rock band Queen.",
       nullptr},
      {"Queen - Bohemian Rhapsody (Official Video Remastered)", "YouTube",
       "https://www.youtube.com/watch?v=video1",
       "Taken from A Night At The Opera, 1975.", "Aug 1, 2008"},
      {"Bohemian Rhapsody (film)", "Wikipedia",
       "https://en.wikipedia.org/wiki/Bohemian_Rhapsody_(film)",
       "A 2018 biographical film about Queen.", nullptr},
      {"Bohemian Rhapsody Lyrics", "Genius",
       "https://genius.com/Queen-bohemian-rhapsody-lyrics",
       "Is this the real life? Is this just fantasy?", nullptr},
      {"The story behind Bohemian Rhapsody", "BBC",
       "https://www.bbc.co.uk/music/bohemian-rhapsody",
       "How Freddie Mercury wrote the six-minute epic.", "Oct 31, 2015"},
      {"Bohemian Rhapsody - Queen | Song Info", "AllMusic",
       "https://www.allmusic.com/song/bohemian-rhapsody",
       "Song review and credits.", nullptr},
      {"Queen's Bohemian Rhapsody: the making of a classic", "The Guardian",
       "https://www.theguardian.com/music/bohemian-rhapsody",
       "Fifty years on, a look back.", "Oct 30, 2025"},
      {"Bohemian Rhapsody chords", "Ultimate Guitar",
       "https://tabs.ultimate-guitar.com/tab/queen/bohemian-rhapsody",
       "Chords and tabs.", nullptr},
      {"Bohemian Rhapsody on Spotify", "Spotify",
       "https://open.spotify.com/track/bohemian-rhapsody",
       "Queen \xC2\xB7 Song \xC2\xB7 1975", nullptr},
      {"Bohemian Rhapsody | Queen Official", "Queen Online",
       "https://www.queenonline.com/bohemian-rhapsody", "The official site.",
       nullptr},
  };
  for (int i = 0; i < kGoogleResults; ++i) {
    const auto& r = kResults[i];
    const uint32_t outer = b.Wrapper(rso);  // div.MjjYud
    const uint32_t g = b.Wrapper(outer);    // div.g
    b.SetBounds(outer, 180, 160 + 130.0f * i, 650, 120);
    const uint32_t top = b.Wrapper(g);
    const std::string host =
        std::string(r.url).substr(8, std::string(r.url).find('/', 8) - 8);
    const std::string cite = "https://" + host + " \xE2\x80\xBA " + "music";
    const uint32_t link = b.Add(
        top, PageRole::kLink, std::string(r.title) + " " + r.site + " " + cite,
        kStateFocusable);
    b.node(link).url = r.url;
    b.Heading(link, 3, r.title);
    const uint32_t source = b.Wrapper(link);
    b.Add(source, PageRole::kImage, "", kStateIgnored);  // the favicon
    b.Text(source, r.site);
    b.Text(source, cite);
    b.Button(top, "About this result");
    const uint32_t snippet = b.Wrapper(g);
    if (r.date) {
      b.Text(snippet, r.date);
      b.Text(snippet, "\xE2\x80\x94");
    }
    b.Text(snippet, r.snippet);
    if (160 + 130.0f * i >= kViewportHeight) {
      MarkOffscreen(b, outer);
    }
    if (i == 2) {
      // People also ask: four questions that expand.
      const uint32_t paa = b.Wrapper(rso);
      b.Heading(paa, 2, "People also ask");
      const uint32_t questions = b.Wrapper(paa);
      for (const char* q :
           {"What is the true meaning of Bohemian Rhapsody?",
            "Who wrote Bohemian Rhapsody?", "How long is Bohemian Rhapsody?",
            "Why is Bohemian Rhapsody so famous?"}) {
        const uint32_t wrapper = b.Wrapper(questions);
        b.Text(b.Button(wrapper, q, kStateCollapsed), q);
      }
      MarkOffscreen(b, paa);
    }
  }

  const uint32_t footer = b.Add(body, PageRole::kContentInfo, "", 0);
  for (const char* f : {"Help", "Send feedback", "Privacy", "Terms"}) {
    b.Link(b.Wrapper(footer), f, std::string("/") + f);
  }
  MarkOffscreen(b, footer);
  return b.Take();
}

PageTree WikipediaArticle() {
  TreeBuilder b("Bohemian Rhapsody - Wikipedia");
  const uint32_t root = b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea,
                              "Bohemian Rhapsody - Wikipedia", kStateFocusable);
  b.SetBounds(root, 0, 0, kViewportWidth, kViewportHeight);
  b.Link(root, "Jump to content", "#bodyContent");

  const uint32_t banner = b.Add(root, PageRole::kBanner, "", 0);
  b.Button(banner, "Main menu");
  b.Link(banner, "Wikipedia The Free Encyclopedia", "/wiki/Main_Page");
  const uint32_t search = b.Add(banner, PageRole::kSearch, "", 0);
  const uint32_t field = b.Add(search, PageRole::kSearchBox, "Search Wikipedia",
                               kStateFocusable | kStateEditable);
  b.node(field).placeholder = "Search Wikipedia";
  b.node(field).input_type = "search";
  b.Button(search, "Search");

  const uint32_t site_nav = b.Add(root, PageRole::kNavigation, "Site", 0);
  b.SetBounds(site_nav, 0, 120, 176, 400);
  const uint32_t site_list = b.Add(site_nav, PageRole::kList, "", 0);
  for (const char* item : {"Main page", "Contents", "Current events",
                           "Random article", "About Wikipedia"}) {
    const uint32_t li = b.Add(site_list, PageRole::kListItem, "", 0);
    b.SetBounds(li, 8, 0, 160, 20);
    b.Link(li, item, std::string("/wiki/") + item);
  }

  const uint32_t main = b.Add(root, PageRole::kMain, "", 0);
  b.SetBounds(main, 176, 60, 1000, 3000);
  b.Heading(main, 1, "Bohemian Rhapsody");
  const uint32_t toc = b.Add(main, PageRole::kNavigation, "Contents", 0);
  const uint32_t toc_list = b.Add(toc, PageRole::kList, "", 0);
  for (const char* section : {"(Top)", "Background", "Composition", "Release",
                              "Legacy", "References"}) {
    const uint32_t li = b.Add(toc_list, PageRole::kListItem, "", 0);
    b.SetBounds(li, 8, 0, 160, 20);
    b.Link(li, section, std::string("#") + section);
  }
  const uint32_t p1 = b.Add(main, PageRole::kParagraph, "", 0);
  b.Text(p1, "\"Bohemian Rhapsody\" is a song by the British rock band ");
  b.Link(p1, "Queen", "/wiki/Queen_(band)");
  b.Text(p1, ", released as the lead single from their fourth album, ");
  b.Link(p1, "A Night at the Opera", "/wiki/A_Night_at_the_Opera");
  b.Text(p1, " (1975). Written by lead singer ");
  b.Link(p1, "Freddie Mercury", "/wiki/Freddie_Mercury");
  b.Text(p1, ", the song is a six-minute suite.");

  // The infobox.
  const uint32_t infobox = b.Add(main, PageRole::kTable, "", 0);
  const uint32_t header = b.Add(infobox, PageRole::kRow, "", 0);
  b.Text(b.Add(header, PageRole::kColumnHeader, "Bohemian Rhapsody", 0),
         "Bohemian Rhapsody");
  const char* const kInfo[][2] = {{"Released", "31 October 1975"},
                                  {"Genre", "Progressive rock"},
                                  {"Length", "5:55"},
                                  {"Label", "EMI"}};
  for (const auto& row_text : kInfo) {
    const uint32_t row = b.Add(infobox, PageRole::kRow, "", 0);
    b.Text(b.Add(row, PageRole::kRowHeader, row_text[0], 0), row_text[0]);
    b.Text(b.Add(row, PageRole::kCell, "", 0), row_text[1]);
  }

  b.Heading(main, 2, "Background");
  const uint32_t p2 = b.Add(main, PageRole::kParagraph, "", 0);
  b.Text(p2, "Mercury began writing the song in the late 1960s.");
  b.Heading(main, 2, "Composition");
  b.Heading(main, 3, "Ballad");
  b.Heading(main, 3, "Opera");
  b.Heading(main, 2, "References");
  const uint32_t refs = b.Add(main, PageRole::kList, "", 0);
  for (int i = 1; i <= 6; ++i) {
    const uint32_t li = b.Add(refs, PageRole::kListItem, "", 0);
    b.Text(li, "Reference " + std::to_string(i) + ".");
    b.Link(li, "Source " + std::to_string(i),
           "https://example.org/ref" + std::to_string(i));
    MarkOffscreen(b, li);
  }

  const uint32_t footer = b.Add(root, PageRole::kContentInfo, "", 0);
  const uint32_t footer_list = b.Add(footer, PageRole::kList, "", 0);
  for (const char* item :
       {"Privacy policy", "About Wikipedia", "Disclaimers", "Contact"}) {
    b.Link(b.Add(footer_list, PageRole::kListItem, "", 0), item,
           std::string("/wiki/") + item);
  }
  MarkOffscreen(b, footer);
  return b.Take();
}

PageTree PasswordForm() {
  TreeBuilder b("Sign in - Example");
  const uint32_t root = b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea,
                              "Sign in - Example", kStateFocusable);
  const uint32_t main = b.Add(root, PageRole::kMain, "", 0);
  const uint32_t form = b.Add(main, PageRole::kForm, "Sign in", 0);
  const uint32_t field_states = kStateFocusable | kStateEditable;

  const uint32_t email =
      b.Add(form, PageRole::kTextField, "Email", field_states | kStateRequired);
  b.node(email).input_type = "email";
  b.node(email).autocomplete = "username";
  b.node(email).value = "ana@example.com";

  const uint32_t password = b.Add(form, PageRole::kTextField, "Password",
                                  field_states | kStateProtected);
  b.node(password).input_type = "password";
  b.node(password).autocomplete = "current-password";
  b.node(password).value = "hunter2-hunter2";

  const uint32_t otp = b.Add(form, PageRole::kTextField, "Code", field_states);
  b.node(otp).input_type = "text";
  b.node(otp).autocomplete = "one-time-code";
  b.node(otp).value = "123456";

  const uint32_t card = b.Add(form, PageRole::kTextField, "Card", field_states);
  b.node(card).input_type = "text";
  b.node(card).autocomplete = "billing cc-number";
  b.node(card).value = "4111 1111 1111 1111";

  // A PIN field with no type or autocomplete that says so: only its label.
  const uint32_t pin = b.Add(form, PageRole::kTextField, "", field_states);
  b.node(pin).input_type = "tel";
  b.node(pin).placeholder = "Your PIN";
  b.node(pin).value = "4321";

  b.Add(form, PageRole::kCheckBox, "Remember me",
        kStateFocusable | kStateChecked | kStateHasDefaultAction);
  const uint32_t submit = b.Button(form, "Sign in");
  b.node(submit).html_tag = "button";
  b.Link(main, "Forgot password?", "/reset");
  b.SetFocus(email);
  return b.Take();
}

PageTree CrossOriginFrames() {
  TreeBuilder b("Host page");
  const uint32_t root = b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea,
                              "Host page", kStateFocusable);
  b.Heading(root, 1, "Embedded content");
  b.Link(root, "Home", "https://host.example/");
  const uint32_t iframe = b.Add(root, PageRole::kIframe, "Widget", 0);
  // The frame's own tree.
  b.SetTree(1);
  const uint32_t frame_root =
      b.Add(TreeBuilder::kRoot, PageRole::kRootWebArea, "Widget", 0);
  b.Button(frame_root, "Like");
  b.Link(frame_root, "Share", "https://widget.example/share");
  b.SetTree(0);
  b.AppendChild(iframe, frame_root);
  b.Button(root, "After frame");
  return b.Take();
}

}  // namespace test
}  // namespace voice
}  // namespace molt_ai
