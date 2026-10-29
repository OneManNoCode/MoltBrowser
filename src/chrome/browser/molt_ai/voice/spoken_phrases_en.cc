// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// English: the words voice mode knows (spoken_phrases.h). Folded as
// spoken_text.h folds words: lower case, no apostrophes ("whats").

#include <algorithm>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_phrases.h"

namespace molt_ai {
namespace voice {

namespace {

using K = NumberWordKind;

constexpr NumberWord kNumbers[] = {
    {"zero", K::kUnit, 0},
    {"one", K::kUnit, 1},
    {"two", K::kUnit, 2},
    {"three", K::kUnit, 3},
    {"four", K::kUnit, 4},
    {"five", K::kUnit, 5},
    {"six", K::kUnit, 6},
    {"seven", K::kUnit, 7},
    {"eight", K::kUnit, 8},
    {"nine", K::kUnit, 9},
    {"ten", K::kTeen, 10},
    {"eleven", K::kTeen, 11},
    {"twelve", K::kTeen, 12},
    {"thirteen", K::kTeen, 13},
    {"fourteen", K::kTeen, 14},
    {"fifteen", K::kTeen, 15},
    {"sixteen", K::kTeen, 16},
    {"seventeen", K::kTeen, 17},
    {"eighteen", K::kTeen, 18},
    {"nineteen", K::kTeen, 19},
    {"twenty", K::kTens, 20},
    {"thirty", K::kTens, 30},
    {"forty", K::kTens, 40},
    {"fifty", K::kTens, 50},
    {"sixty", K::kTens, 60},
    {"seventy", K::kTens, 70},
    {"eighty", K::kTens, 80},
    {"ninety", K::kTens, 90},
    {"hundred", K::kHundred, 100},
    {"thousand", K::kThousand, 1000},
    // Ordinals.
    {"first", K::kUnit, 1, true},
    {"second", K::kUnit, 2, true},
    {"third", K::kUnit, 3, true},
    {"fourth", K::kUnit, 4, true},
    {"fifth", K::kUnit, 5, true},
    {"sixth", K::kUnit, 6, true},
    {"seventh", K::kUnit, 7, true},
    {"eighth", K::kUnit, 8, true},
    {"ninth", K::kUnit, 9, true},
    {"tenth", K::kTeen, 10, true},
    {"eleventh", K::kTeen, 11, true},
    {"twelfth", K::kTeen, 12, true},
    {"thirteenth", K::kTeen, 13, true},
    {"fourteenth", K::kTeen, 14, true},
    {"fifteenth", K::kTeen, 15, true},
    {"sixteenth", K::kTeen, 16, true},
    {"seventeenth", K::kTeen, 17, true},
    {"eighteenth", K::kTeen, 18, true},
    {"nineteenth", K::kTeen, 19, true},
    {"twentieth", K::kTens, 20, true},
    {"thirtieth", K::kTens, 30, true},
    {"fortieth", K::kTens, 40, true},
    {"fiftieth", K::kTens, 50, true},
    {"sixtieth", K::kTens, 60, true},
    {"seventieth", K::kTens, 70, true},
    {"eightieth", K::kTens, 80, true},
    {"ninetieth", K::kTens, 90, true},
    {"hundredth", K::kHundred, 100, true},
    {"thousandth", K::kThousand, 1000, true},
    // What recognizers write for a number they heard, read as one only
    // where a number is expected: "number for", "click to", "tab too".
    {"won", K::kUnit, 1, false, true},
    {"to", K::kUnit, 2, false, true},
    {"too", K::kUnit, 2, false, true},
    {"for", K::kUnit, 4, false, true},
    {"fore", K::kUnit, 4, false, true},
    {"ate", K::kUnit, 8, false, true},
    {"forth", K::kUnit, 4, true, true},
};

constexpr TimeUnitWord kTimeUnits[] = {
    {"second", 1}, {"seconds", 1}, {"sec", 1},      {"secs", 1},
    {"s", 1},      {"minute", 60}, {"minutes", 60}, {"min", 60},
    {"mins", 60},  {"hour", 3600}, {"hours", 3600}, {"hr", 3600},
    {"hrs", 3600},
};

constexpr ChoiceRoleMask kListish = 0;  // names an item, not its role

constexpr RoleWord kRoleWords[] = {
    {"one", kListish},
    {"ones", kListish},
    {"result", kListish},
    {"results", kListish},
    {"item", kListish},
    {"items", kListish},
    {"entry", kListish},
    {"option", kListish},
    {"choice", kListish},
    {"thing", kListish},
    {"video", RoleBit(ChoiceRole::kVideo)},
    {"videos", RoleBit(ChoiceRole::kVideo)},
    {"clip", RoleBit(ChoiceRole::kVideo)},
    {"song", RoleBit(ChoiceRole::kAudio)},
    {"track", RoleBit(ChoiceRole::kAudio)},
    {"episode", RoleBit(ChoiceRole::kAudio) | RoleBit(ChoiceRole::kVideo)},
    {"podcast", RoleBit(ChoiceRole::kAudio)},
    {"link", RoleBit(ChoiceRole::kLink)},
    {"links", RoleBit(ChoiceRole::kLink)},
    {"button", RoleBit(ChoiceRole::kButton)},
    {"field", RoleBit(ChoiceRole::kTextField)},
    {"box", RoleBit(ChoiceRole::kTextField) | RoleBit(ChoiceRole::kCheckBox)},
    {"checkbox", RoleBit(ChoiceRole::kCheckBox)},
    {"dropdown", RoleBit(ChoiceRole::kListBox)},
    {"menu", RoleBit(ChoiceRole::kListBox)},
    {"tab", RoleBit(ChoiceRole::kTab)},
    {"image", RoleBit(ChoiceRole::kImage)},
    {"picture", RoleBit(ChoiceRole::kImage)},
    {"photo", RoleBit(ChoiceRole::kImage)},
    {"heading", RoleBit(ChoiceRole::kHeading)},
    {"article", RoleBit(ChoiceRole::kArticle)},
    {"story", RoleBit(ChoiceRole::kArticle)},
    {"post", RoleBit(ChoiceRole::kArticle)},
    {"product", RoleBit(ChoiceRole::kProduct)},
};

constexpr std::string_view kChoiceStopwords[] = {
    "a",     "about",  "an",    "and",     "called",   "is",    "it",
    "its",   "named",  "or",    "says",    "said",     "that",  "the",
    "this",  "titled", "which", "labeled", "labelled", "thats", "whats",
    "where", "one",    "ones",  "there",   "those",    "these",
};

// Kept when an item has them ("Log in" / "Log out", "Live at Wembley"),
// and weightless when none does ("the one with the guitar").
constexpr std::string_view kChoiceParticles[] = {
    "at", "for", "in", "into", "of", "off", "on", "out", "to", "up", "with",
};

}  // namespace

LanguagePhrases MakeEnglishPhrases() {
  LanguagePhrases p;
  p.language = Language::kEnglish;

  p.numbers.assign(std::begin(kNumbers), std::end(kNumbers));
  std::sort(
      p.numbers.begin(), p.numbers.end(),
      [](const NumberWord& a, const NumberWord& b) { return a.word < b.word; });
  p.number_and = {"and"};
  p.number_article = {"a", "an"};
  p.ordinal_suffixes = {"st", "nd", "rd", "th"};
  p.year_zero = {"oh", "o"};
  p.number_prefixes = {"number", "num", "nr", "numero"};

  p.last_words = {"last", "final", "bottom"};
  p.from_end_links = {"to", "from"};
  p.end_phrases = {"from the end", "from the bottom", "from the last"};
  p.second_to_last_phrases = {"next to last",        "penultimate",
                              "last but one",        "one before last",
                              "one before the last", "second to last",
                              "second last"};
  p.position_intensifiers = {"very"};

  p.time_units.assign(std::begin(kTimeUnits), std::end(kTimeUnits));
  p.half_words = {"half"};

  p.articles = {"the", "a", "an"};
  p.current_phrases = {"this one",
                       "that one",
                       "this",
                       "that",
                       "it",
                       "the same one",
                       "same one",
                       "the current one",
                       "current one",
                       "the selected one",
                       "the one i chose",
                       "the one selected",
                       "this item",
                       "that item"};
  p.next_phrases = {"next one",           "the next one",
                    "the one below",      "one below",
                    "the one after",      "the one after this",
                    "the one after that", "the following one",
                    "the one underneath", "the next item"};
  p.previous_phrases = {
      "previous one",        "the previous one",  "the one above",
      "one above",           "the one before",    "the one before this",
      "the one before that", "the preceding one", "the previous item"};
  p.by_cues = {"by",
               "uploaded by",
               "posted by",
               "written by",
               "sung by",
               "performed by",
               "made by",
               "sold by",
               "from the channel",
               "on the channel",
               "from channel"};
  p.from_cues = {"from", "published by", "published in", "released in"};
  p.choice_stopwords.assign(std::begin(kChoiceStopwords),
                            std::end(kChoiceStopwords));
  std::sort(p.choice_stopwords.begin(), p.choice_stopwords.end());
  p.choice_stopwords.erase(
      std::unique(p.choice_stopwords.begin(), p.choice_stopwords.end()),
      p.choice_stopwords.end());
  p.choice_particles.assign(std::begin(kChoiceParticles),
                            std::end(kChoiceParticles));
  std::sort(p.choice_particles.begin(), p.choice_particles.end());
  p.role_words.assign(std::begin(kRoleWords), std::end(kRoleWords));
  std::sort(
      p.role_words.begin(), p.role_words.end(),
      [](const RoleWord& a, const RoleWord& b) { return a.word < b.word; });

  p.fillers = {"um",  "uh",  "uhm", "umm", "uhh", "er",
               "erm", "hmm", "mm",  "mhm", "ah",  "eh"};
  p.leading_phrases = {"please",
                       "kindly",
                       "can you",
                       "could you",
                       "would you",
                       "will you",
                       "can you just",
                       "could you just",
                       "i want to",
                       "i wanna",
                       "i want you to",
                       "i would like to",
                       "i would like you to",
                       "id like to",
                       "id like you to",
                       "i need to",
                       "i need you to",
                       "go ahead and",
                       "lets",
                       "let us",
                       "just",
                       "now",
                       "and",
                       "then",
                       "and then",
                       "so"};
  p.trailing_phrases = {"please", "thanks", "thank you", "thank you very much"};
  p.trailing_if_command = {"now", "right now", "for me"};
  p.leading_if_more = {"ok",    "okay", "alright", "all right",
                       "right", "hey",  "oh"};
  p.wake_greetings = {"hey", "hi", "ok", "okay", "hello", "a", "hay"};
  p.default_wake_phrase = "hey molt";

  p.volume_words = {
      {"half", 50},     {"halfway", 50},         {"max", 100},
      {"maximum", 100}, {"full", 100},           {"min", 0},
      {"minimum", 0},   {"all the way up", 100}, {"all the way down", 0},
      {"loudest", 100}};
  p.percent_words = {"percent", "per cent", "pc"};
  p.out_of_ten = {"out of ten", "out of 10"};
  p.page_words = {"page", "pages", "screen", "screens", "screenful"};
  p.scroll_half = {"half a page", "half page",    "half a screen", "a bit",
                   "a little",    "a little bit", "slightly",      "a tad"};
  p.scroll_page = {"a page",     "one page",    "a screen",
                   "one screen", "a screenful", "page"};

  // Not "point" ("PowerPoint") or "dash" alone in a name: only words no
  // site name is made of.
  p.address_words = {
      {"dot", '.'}, {"slash", '/'}, {"hyphen", '-'}, {"underscore", '_'}};
  p.site_suffixes = {"website", "web site", "site",     "homepage", "home page",
                     "page",    "app",      "web page", "webpage"};
  p.site_prefixes = {"the"};
  return p;
}

}  // namespace voice
}  // namespace molt_ai
