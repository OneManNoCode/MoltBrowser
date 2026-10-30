// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The built-in site skills, as data (site_skills.h has the format,
// docs/design/VOICE_SKILLS.md how to add a site and refresh its test
// pages). The browser parses this as it would any skill file, with caps,
// and site_skills_unittest.cc checks that every skill here parses and finds
// its parts on its site's test pages (chrome/test/data/molt_voice/skills/).
// Bump a skill's version whenever it changes.

#include <string_view>

#include "chrome/browser/molt_ai/voice/site_skills.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr char kBuiltInSiteSkills[] = R"JSON({
  "format": 1,
  "about": "MoltBrowser's built-in site skills (docs/design/VOICE_SKILLS.md).",
  "skills": [
    {
      "id": "youtube",
      "version": 1,
      "name": "YouTube",
      "hosts": ["youtube.com"],
      "except_hosts": ["m.youtube.com", "music.youtube.com",
                       "studio.youtube.com", "tv.youtube.com"],
      "search": {
        "field": [
          {"role": "combobox|searchbox|textfield", "name_has": "search",
           "inside": {"role": "search"}},
          {"role": "combobox|searchbox|textfield", "id": "search"}
        ],
        "submit": [
          {"role": "button", "name": "search", "inside": {"role": "search"}},
          {"role": "button", "id": "search-icon-legacy"}
        ]
      },
      "results": {
        "label": "Videos",
        "title": [
          {"role": "link", "id": "video-title|video-title-link"},
          {"role": "link", "class": "yt-lockup-metadata-view-model__title|yt-lockup-metadata-view-model-wiz__title"}
        ]
      },
      "player": {
        "media": [
          {"role": "video", "class": "html5-main-video"},
          {"role": "video", "inside": {"id": "movie_player"}}
        ],
        "skip_ad": [
          {"role": "button", "class": "ytp-skip-ad-button|ytp-ad-skip-button-modern|ytp-ad-skip-button"},
          {"role": "button", "name_has": "skip", "inside": {"id": "movie_player"}}
        ],
        "captions": [{"role": "button", "class": "ytp-subtitles-button"}],
        "fullscreen": [{"role": "button", "class": "ytp-fullscreen-button"}]
      },
      "next_page_scrolls": true,
      "pages": [
        {"path": "/results", "expect": ["search", "results"]},
        {"path": "/watch", "expect": ["search", "media"]}
      ]
    },
    {
      "id": "google",
      "version": 1,
      "name": "Google",
      "hosts": ["google.com", "google.co.uk", "google.ca", "google.com.au",
                "google.co.in", "google.de", "google.fr", "google.es",
                "google.it", "google.nl", "google.ie", "google.co.nz"],
      "search": {
        "field": [
          {"role": "combobox|searchbox|textfield", "name": "search",
           "inside": {"role": "search"}},
          {"role": "combobox|searchbox|textfield", "id": "APjFqb"}
        ],
        "submit": [
          {"role": "button", "name": "search|google search",
           "inside": {"role": "search"}}
        ]
      },
      "results": {
        "label": "Results",
        "within": [{"role": "main"}],
        "title": [
          {"role": "link", "has": {"role": "heading", "level": 3},
           "url_not": "/aclk|googleadservices"}
        ]
      },
      "next_page": [
        {"role": "link", "id": "pnnext"},
        {"role": "link|button", "name": "next|next page|more results"}
      ],
      "pages": [{"path": "/search", "expect": ["search", "results"]}]
    },
    {
      "id": "duckduckgo",
      "version": 1,
      "name": "DuckDuckGo",
      "hosts": ["duckduckgo.com"],
      "search": {
        "field": [
          {"role": "combobox|searchbox|textfield",
           "id": "search_form_input|searchbox_input"},
          {"role": "combobox|searchbox|textfield", "name_has": "search",
           "inside": {"role": "search"}}
        ],
        "submit": [
          {"role": "button", "id": "search_button|search_button_homepage"},
          {"role": "button", "name": "search", "inside": {"role": "search"}}
        ]
      },
      "results": {
        "label": "Results",
        "title": [
          {"role": "link", "inside": {"role": "heading", "level": 2},
           "url_not": "duckduckgo.com/y.js"}
        ]
      },
      "next_page": [
        {"role": "button", "id": "more-results"},
        {"role": "button|link", "name": "more results"}
      ],
      "pages": [{"path": "/", "expect": ["search"]}]
    },
    {
      "id": "bing",
      "version": 1,
      "name": "Bing",
      "hosts": ["bing.com"],
      "search": {
        "field": [
          {"role": "combobox|searchbox|textfield|textarea", "id": "sb_form_q"},
          {"role": "combobox|searchbox|textfield", "name_has": "search",
           "inside": {"role": "search"}}
        ]
      },
      "results": {
        "label": "Results",
        "within": [{"id": "b_results"}, {"role": "main"}],
        "title": [
          {"role": "link", "inside": {"role": "heading", "level": 2},
           "url_not": "/aclk"}
        ]
      },
      "next_page": [
        {"role": "link", "class": "sb_pagN"},
        {"role": "link", "name": "next page|next"}
      ],
      "pages": [{"path": "/search", "expect": ["search", "results"]}]
    },
    {
      "id": "wikipedia",
      "version": 1,
      "name": "Wikipedia",
      "domains": ["wikipedia.org"],
      "search": {
        "field": [
          {"role": "searchbox|textfield|combobox", "id": "searchInput"},
          {"role": "searchbox|textfield|combobox",
           "name_has": "search wikipedia"}
        ],
        "submit": [
          {"role": "button", "name": "search", "inside": {"role": "search"}}
        ]
      },
      "results": {
        "label": "Articles",
        "within": [{"class": "mw-search-results"}],
        "title": [
          {"role": "link", "inside": {"class": "mw-search-result-heading"}}
        ]
      },
      "next_page": [
        {"role": "link", "class": "mw-nextlink"},
        {"role": "link", "name_has": "next 20"}
      ],
      "reading": {
        "content": [{"id": "mw-content-text"}, {"role": "main"}],
        "skip": [
          {"role": "link", "url_has": "action=edit"},
          {"role": "link", "url_has": "#cite_note"},
          {"class": "mw-editsection|infobox|navbox|sidebar|metadata|ambox|reflist|references|mw-jump-link|catlinks|printfooter"}
        ]
      },
      "pages": [
        {"path": "/wiki/", "expect": ["search", "content"]},
        {"path": "/w/index.php", "expect": ["search"]}
      ]
    }
  ]
})JSON";

}  // namespace

std::string_view BuiltInSiteSkillsJson() {
  return kBuiltInSiteSkills;
}

}  // namespace voice
}  // namespace molt_ai
