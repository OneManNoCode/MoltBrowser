// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_command.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/phonetic_key_es.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_numbers.h"
#include "chrome/browser/molt_ai/voice/spoken_phrases.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr std::array<std::string_view, kVoiceCommandKindCount> kKindNames = {
    "unknown",
    "refused",
    "open_site",
    "search",
    "find_in_page",
    "back",
    "forward",
    "reload",
    "stop_loading",
    "home",
    "next_page",
    "new_tab",
    "close_tab",
    "switch_tab",
    "scroll_down",
    "scroll_up",
    "scroll_left",
    "scroll_right",
    "scroll_to_top",
    "scroll_to_bottom",
    "zoom_in",
    "zoom_out",
    "zoom_reset",
    "play",
    "pause",
    "resume",
    "volume_up",
    "volume_down",
    "set_volume",
    "mute",
    "unmute",
    "skip_forward",
    "skip_back",
    "restart",
    "next_track",
    "previous_track",
    "full_screen",
    "exit_full_screen",
    "captions_on",
    "captions_off",
    "speed_up",
    "slow_down",
    "normal_speed",
    "skip_ad",
    "read_page",
    "read_from_here",
    "read_headings",
    "read_links",
    "read_results",
    "read_title",
    "read_selection",
    "describe_screen",
    "next_heading",
    "previous_heading",
    "next_link",
    "previous_link",
    "next_item",
    "previous_item",
    "next_paragraph",
    "previous_paragraph",
    "read_section",
    "ask_page",
    "choose",
    "show_numbers",
    "hide_numbers",
    "type",
    "new_line",
    "delete_last_word",
    "clear_field",
    "read_back",
    "submit",
    "yes",
    "no",
    "cancel",
    "stop",
    "stop_speaking",
    "undo",
    "repeat",
    "help",
    "where_am_i",
    "speak_faster",
    "speak_slower",
    "verbosity",
    "correction",
    "remember_routine",
    "forget_routine",
    "run_routine",
    "list_routines",
    "forget_word",
    "forget_learning",
    "voice_mode_on",
    "voice_mode_off",
    "set_speed",
    "start_over",
    "next_match",
    "previous_match",
    "bookmark_page",
    "remove_bookmark",
    "list_bookmarks",
    "open_bookmark",
    "list_history",
    "reopen_tab",
    "open_from_history",
    "list_downloads",
    "open_download",
    "open_settings",
    "always_listening",
    "protected_setting",
    "new_window",
    "close_window",
    "switch_window",
    "ask_ai",
    "open_ai_chat",
    "show_browser_buttons",
    "open_menu",
    "close_menu",
};

// --- Patterns ---------------------------------------------------------------

enum class SlotType : uint8_t {
  kSeconds,
  kLevel,
  kSpeed,
  kPages,
  kPosition,
  kNumber,
  kChoice,
  kPick,
  kTab,
  kSite,
  kAddress,
  kQuery,
  kText,
  kName,
};

struct SlotName {
  std::string_view name;
  SlotType type;
};

constexpr SlotName kSlotNames[] = {
    {"seconds", SlotType::kSeconds},   {"level", SlotType::kLevel},
    {"speed", SlotType::kSpeed},       {"pages", SlotType::kPages},
    {"position", SlotType::kPosition}, {"number", SlotType::kNumber},
    {"choice", SlotType::kChoice},     {"pick", SlotType::kPick},
    {"tab", SlotType::kTab},           {"site", SlotType::kSite},
    {"address", SlotType::kAddress},   {"query", SlotType::kQuery},
    {"text", SlotType::kText},         {"name", SlotType::kName},
};

// Slots that read their own words (a number, a length of time).
bool IsFixedSlot(SlotType type) {
  return type == SlotType::kSeconds || type == SlotType::kLevel ||
         type == SlotType::kSpeed || type == SlotType::kPages;
}

// Rest slots that take words up to the last place the next pattern word
// matches (free text); the others take them up to the first.
bool IsGreedySlot(SlotType type) {
  return type == SlotType::kQuery || type == SlotType::kText ||
         type == SlotType::kName;
}

struct Node {
  enum class Type : uint8_t { kWord, kGroup, kSlot };
  Type type = Type::kWord;
  bool optional = false;
  std::string_view word;
  std::vector<std::vector<Node>> alternatives;
  SlotType slot = SlotType::kQuery;
};

struct CompiledPattern {
  VoiceCommandKind kind = VoiceCommandKind::kUnknown;
  PatternContext context = PatternContext::kAlways;
  PatternVariant variant = PatternVariant::kNone;
  std::vector<Node> nodes;
};

// Groups nest at most this deep (the tables use two).
constexpr int kMaxGroupDepth = 4;

class PatternCompiler {
 public:
  explicit PatternCompiler(std::string_view pattern) : text_(pattern) {}

  // nullopt when |pattern| is malformed.
  std::optional<std::vector<Node>> Compile() {
    std::vector<Node> nodes;
    if (!ReadSequence(nodes, /*depth=*/0, /*allow_slots=*/true) ||
        at_ != text_.size()) {
      return std::nullopt;
    }
    if (nodes.empty())
      return std::nullopt;
    // A rest slot ends the pattern or is followed by a required word.
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].type != Node::Type::kSlot || IsFixedSlot(nodes[i].slot)) {
        continue;
      }
      if (i + 1 < nodes.size() &&
          (nodes[i + 1].type == Node::Type::kSlot || nodes[i + 1].optional)) {
        return std::nullopt;
      }
    }
    return nodes;
  }

 private:
  void SkipSpaces() {
    while (at_ < text_.size() && text_[at_] == ' ')
      ++at_;
  }

  static bool IsWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  }

  // Elements up to the end, '|', ')' or ']'.
  bool ReadSequence(std::vector<Node>& out, int depth, bool allow_slots) {
    SkipSpaces();
    while (at_ < text_.size() && text_[at_] != '|' && text_[at_] != ')' &&
           text_[at_] != ']') {
      Node node;
      const char c = text_[at_];
      if (c == '(' || c == '[') {
        if (depth >= kMaxGroupDepth)
          return false;
        const char close = c == '(' ? ')' : ']';
        ++at_;
        node.type = Node::Type::kGroup;
        node.optional = c == '[';
        while (true) {
          std::vector<Node> alternative;
          if (!ReadSequence(alternative, depth + 1, false) ||
              alternative.empty()) {
            return false;
          }
          node.alternatives.push_back(std::move(alternative));
          if (at_ >= text_.size())
            return false;
          if (text_[at_] == '|') {
            ++at_;
            continue;
          }
          if (text_[at_] != close)
            return false;
          ++at_;
          break;
        }
      } else if (c == '{') {
        if (!allow_slots)
          return false;
        const size_t end = text_.find('}', at_);
        if (end == std::string_view::npos)
          return false;
        std::string_view name = text_.substr(at_ + 1, end - at_ - 1);
        at_ = end + 1;
        node.type = Node::Type::kSlot;
        if (!name.empty() && name.back() == '?') {
          node.optional = true;
          name.remove_suffix(1);
        }
        bool known = false;
        for (const SlotName& slot : kSlotNames) {
          if (slot.name == name) {
            node.slot = slot.type;
            known = true;
          }
        }
        if (!known || (node.optional && !IsFixedSlot(node.slot))) {
          return false;
        }
      } else if (IsWordChar(c)) {
        const size_t begin = at_;
        while (at_ < text_.size() && IsWordChar(text_[at_]))
          ++at_;
        node.type = Node::Type::kWord;
        node.word = text_.substr(begin, at_ - begin);
      } else {
        return false;
      }
      out.push_back(std::move(node));
      SkipSpaces();
    }
    return true;
  }

  std::string_view text_;
  size_t at_ = 0;
};

const std::vector<CompiledPattern>& CompiledPatterns(Language language) {
  const auto compile = [](Language lang) {
    auto* compiled = new std::vector<CompiledPattern>();
    for (const CommandPattern& pattern : CommandPatterns(lang)) {
      std::optional<std::vector<Node>> nodes =
          PatternCompiler(pattern.pattern).Compile();
      // A malformed pattern never matches (the unit tests reject it).
      if (!nodes)
        continue;
      CompiledPattern c;
      c.kind = pattern.kind;
      c.context = pattern.context;
      c.variant = pattern.variant;
      c.nodes = std::move(*nodes);
      compiled->push_back(std::move(c));
    }
    return compiled;
  };
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish: {
      // Built once, never destroyed.
      static const std::vector<CompiledPattern>* const spanish =
          compile(Language::kSpanish);
      return *spanish;
    }
  }
  // Built once, never destroyed.
  static const std::vector<CompiledPattern>* const english =
      compile(Language::kEnglish);
  return *english;
}

// Words matched by |node| at |at|, or -1. Groups take their longest
// alternative; nothing backtracks. Recursion follows the pattern's
// nesting (at most kMaxGroupDepth), never the input.
int MatchNode(const Node& node, const WordList& words, size_t at);

int MatchSequence(const std::vector<Node>& sequence,
                  const WordList& words,
                  size_t at) {
  size_t pos = at;
  for (const Node& node : sequence) {
    int taken = MatchNode(node, words, pos);
    if (taken < 0) {
      if (!node.optional)
        return -1;
      taken = 0;
    }
    pos += static_cast<size_t>(taken);
  }
  return static_cast<int>(pos - at);
}

int MatchNode(const Node& node, const WordList& words, size_t at) {
  switch (node.type) {
    case Node::Type::kWord:
      return (at < words.size() && words[at] == node.word) ? 1 : -1;
    case Node::Type::kGroup: {
      int best = -1;
      for (const std::vector<Node>& alternative : node.alternatives) {
        best = std::max(best, MatchSequence(alternative, words, at));
      }
      return best;
    }
    case Node::Type::kSlot:
      return -1;
  }
  return -1;
}

// --- Slots
// --------------------------------------------------------------------

struct Captures {
  std::optional<int> seconds;
  std::optional<int> level;
  std::optional<int> rate;
  ScrollAmount scroll = ScrollAmount::kDefault;
  int count = 1;
  std::optional<ChoiceRef> choice;
  // Free text and site slots: [begin, end) in the cleaned words.
  std::optional<std::pair<size_t, size_t>> text;
  std::optional<std::pair<size_t, size_t>> site;
  bool text_ends_pattern = false;
  bool text_before_site = false;
};

std::optional<std::pair<int, size_t>> ReadLevel(const WordList& words,
                                                size_t at,
                                                const LanguagePhrases& p) {
  size_t best_length = 0;
  int best_value = 0;
  for (const ValueWord& entry : p.volume_words) {
    const size_t length = PhraseLengthAt(entry.word, words, at);
    if (length > best_length) {
      best_length = length;
      best_value = entry.value;
    }
  }
  if (best_length)
    return std::make_pair(best_value, best_length);
  NumberOptions options;
  options.ordinals = false;
  options.homophones = true;
  const std::optional<SpokenNumber> number = ReadNumber(words, at, p, options);
  if (!number)
    return std::nullopt;
  size_t length = number->words;
  int64_t value = number->value;
  if (const size_t n = LongestPhraseAt(p.percent_words, words, at + length)) {
    length += n;
    if (value > 100)
      return std::nullopt;
  } else if (const size_t m =
                 LongestPhraseAt(p.out_of_ten, words, at + length)) {
    length += m;
    if (value > 10)
      return std::nullopt;
    value *= 10;
  } else if (value <= 10) {
    value *= 10;  // "volume 5": a scale of ten
  } else if (value > 100) {
    return std::nullopt;
  }
  return std::make_pair(static_cast<int>(value), length);
}

struct Pages {
  ScrollAmount amount = ScrollAmount::kDefault;
  int count = 1;
  size_t words = 0;
};

std::optional<Pages> ReadPages(const WordList& words,
                               size_t at,
                               const LanguagePhrases& p) {
  const size_t half = LongestPhraseAt(p.scroll_half, words, at);
  const size_t page = LongestPhraseAt(p.scroll_page, words, at);
  if (half && half >= page)
    return Pages{ScrollAmount::kHalfPage, 1, half};
  if (page)
    return Pages{ScrollAmount::kPage, 1, page};
  NumberOptions options;
  options.ordinals = false;
  options.homophones = true;
  const std::optional<SpokenNumber> number = ReadNumber(words, at, p, options);
  if (!number || number->value < 1 || number->value > 50)
    return std::nullopt;
  if (at + number->words >= words.size() ||
      !HasPhrase(p.page_words, words[at + number->words])) {
    return std::nullopt;
  }
  return Pages{ScrollAmount::kPage, static_cast<int>(number->value),
               number->words + 1};
}

struct Site {
  std::string host;
  bool address = false;
};

// The words of the utterance, cleaned up, with where each came from.
struct Utterance {
  std::string_view source;
  NormalizedText normalized;
  WordList words;               // cleaned words
  std::vector<size_t> index;    // cleaned word -> normalized word
  std::vector<size_t> fillers;  // normalized indices dropped as fillers
};

// --- {speed} -----------------------------------------------------------------

// The words a playback speed is said with, per language (folded).
struct SpeedWords {
  // Before the decimals: "one point five".
  std::vector<std::string_view> point;
  // After a whole number: "one and a half".
  std::vector<std::string_view> and_half;
  // After the speed: "two times", "2 x".
  std::vector<std::string_view> times;
  // Speeds said as a word: "double", "half" (hundredths).
  std::vector<std::pair<std::string_view, int>> named;
};

const SpeedWords& SpeedWordsFor(Language language) {
  static const SpeedWords* const english =
      new SpeedWords{{"point", "dot"},
                     {"and a half"},
                     {"times", "x", "times faster", "times speed"},
                     {{"double", 200}, {"twice", 200}, {"half", 50}}};
  static const SpeedWords* const spanish =
      new SpeedWords{{"coma", "punto"},
                     {"y medio", "y media"},
                     {"veces", "x", "por"},
                     {{"doble", 200},
                      {"el doble", 200},
                      {"la mitad", 50},
                      {"mitad", 50},
                      {"media", 50}}};
  return language == Language::kSpanish ? *spanish : *english;
}

// A word of digits and an "x" before or after them ("2x", "x2"), as a
// recognizer writes a speed: the digits' value.
std::optional<int> DigitsWithX(std::string_view word) {
  if (word.size() < 2 || word.size() > 3) {
    return std::nullopt;
  }
  std::string_view digits;
  if (word.front() == 'x') {
    digits = word.substr(1);
  } else if (word.back() == 'x') {
    digits = word.substr(0, word.size() - 1);
  } else {
    return std::nullopt;
  }
  int value = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

// The hundredths a fraction's digits stand for: "5" -> 50, "25" -> 25.
std::optional<int> FractionDigits(std::string_view word) {
  if (word.empty() || word.size() > 2) {
    return std::nullopt;
  }
  int value = 0;
  for (char c : word) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  return word.size() == 1 ? value * 10 : value;
}

// A playback speed at |at| of |u|'s words: hundredths and the words it
// took. "1.5", "1,5" (as written), "one point five", "one point two five",
// "one and a half", "2x", "x2", "1.5x", "two times", "double", "half".
// Outside kMinSpokenRate..kMaxSpokenRate it is no speed.
std::optional<std::pair<int, size_t>> ReadSpeed(const Utterance& u,
                                                size_t at,
                                                const LanguagePhrases& p) {
  const WordList& words = u.words;
  if (at >= words.size()) {
    return std::nullopt;
  }
  const SpeedWords& w = SpeedWordsFor(p.language);
  const auto in_range = [](int64_t rate) {
    return rate >= kMinSpokenRate && rate <= kMaxSpokenRate;
  };
  // A trailing "times" or "x" (and an "x" before: "x 2").
  const auto with_times =
      [&](int rate, size_t taken) -> std::optional<std::pair<int, size_t>> {
    if (!in_range(rate)) {
      return std::nullopt;
    }
    taken += LongestPhraseAt(w.times, words, at + taken);
    return std::make_pair(rate, taken);
  };
  size_t best_named = 0;
  int named_rate = 0;
  for (const auto& [phrase, rate] : w.named) {
    const size_t n = PhraseLengthAt(phrase, words, at);
    if (n > best_named) {
      best_named = n;
      named_rate = rate;
    }
  }
  if (best_named) {
    return std::make_pair(named_rate, best_named);
  }
  if (const std::optional<int> x = DigitsWithX(words[at])) {
    return in_range(int64_t{*x} * 100)
               ? std::optional<std::pair<int, size_t>>(
                     std::make_pair(*x * 100, size_t{1}))
               : std::nullopt;
  }
  size_t start = at;
  if (words[at] == "x" && at + 1 < words.size()) {
    ++start;  // "x 2"
  }
  NumberOptions options;
  options.ordinals = false;
  options.homophones = false;
  const std::optional<SpokenNumber> whole =
      ReadNumber(words, start, p, options);
  if (!whole || whole->value > kMaxSpokenRate / 100) {
    return std::nullopt;
  }
  const int64_t base = whole->value * 100;
  size_t taken = start - at + whole->words;
  const size_t next = at + taken;
  // "1.5" and "1,5" as written: digits, the mark, digits, with nothing
  // between.
  if (next < words.size() && whole->words == 1 && start == at) {
    const SpokenWord& left = u.normalized.words[u.index[at]];
    const SpokenWord& right = u.normalized.words[u.index[next]];
    if (left.numeric && (left.trailing == '.' || left.trailing == ',') &&
        right.source_begin == left.source_end + 1) {
      std::string_view digits = words[next];
      const bool x_after = !digits.empty() && digits.back() == 'x';
      if (x_after) {
        digits.remove_suffix(1);
      }
      if (const std::optional<int> fraction = FractionDigits(digits)) {
        if (x_after) {
          return in_range(base + *fraction)
                     ? std::optional<std::pair<int, size_t>>(std::make_pair(
                           static_cast<int>(base + *fraction), taken + 1))
                     : std::nullopt;
        }
        return with_times(static_cast<int>(base + *fraction), taken + 1);
      }
    }
  }
  // "one point five", "one point two five", "uno coma cinco".
  if (const size_t point = LongestPhraseAt(w.point, words, next)) {
    const size_t first = next + point;
    if (first < words.size()) {
      if (const std::optional<int> digits = FractionDigits(words[first])) {
        return with_times(static_cast<int>(base + *digits), taken + point + 1);
      }
      const std::optional<SpokenNumber> tenths =
          ReadNumber(words, first, p, options);
      if (tenths && tenths->words == 1 && tenths->value <= 9) {
        int64_t fraction = tenths->value * 10;
        size_t used = 1;
        const std::optional<SpokenNumber> hundredths =
            first + 1 < words.size() ? ReadNumber(words, first + 1, p, options)
                                     : std::nullopt;
        if (hundredths && hundredths->words == 1 && hundredths->value <= 9) {
          fraction += hundredths->value;
          used = 2;
        }
        return with_times(static_cast<int>(base + fraction),
                          taken + point + used);
      }
      if (tenths && tenths->value >= 10 && tenths->value <= 99) {
        return with_times(static_cast<int>(base + tenths->value),
                          taken + point + tenths->words);
      }
    }
    return std::nullopt;
  }
  // "one and a half", "uno y medio".
  if (const size_t half = LongestPhraseAt(w.and_half, words, next)) {
    return with_times(static_cast<int>(base + 50), taken + half);
  }
  return with_times(static_cast<int>(base), taken);
}

std::optional<Site> ReadSite(const Utterance& u,
                             size_t begin,
                             size_t end,
                             bool address_only,
                             const LanguagePhrases& p) {
  if (begin >= end)
    return std::nullopt;
  // A dot, spoken or written, makes it an address.
  bool address = false;
  for (size_t i = begin; i < end; ++i) {
    const SpokenWord& word = u.normalized.words[u.index[i]];
    const bool glued = i > begin && u.index[i] == u.index[i - 1] + 1;
    // "user@example.com", "host:8080": not a site name or host.
    if (glued && (word.joiner == '@' || word.joiner == ':')) {
      return std::nullopt;
    }
    if (glued && word.joiner == '.')
      address = true;
    for (const AddressWord& entry : p.address_words) {
      if (entry.mark == '.' && entry.word == u.words[i])
        address = true;
    }
  }
  if (address_only && !address)
    return std::nullopt;
  if (address) {
    // An address is one run: each word glued to the one before ("node.js"),
    // after a spoken mark ("dot com"), or a spelled letter after another
    // ("w w w"). "search for node.js tutorials" is not one.
    for (size_t i = begin + 1; i < end; ++i) {
      const bool glued = u.index[i] == u.index[i - 1] + 1 &&
                         u.normalized.words[u.index[i]].joiner;
      bool after_mark = false;
      bool marks_here = false;
      for (const AddressWord& entry : p.address_words) {
        after_mark = after_mark || entry.word == u.words[i - 1];
        marks_here = marks_here || entry.word == u.words[i];
      }
      const bool letters = u.words[i].size() == 1 && u.words[i - 1].size() == 1;
      if (!glued && !after_mark && !marks_here && !letters) {
        return std::nullopt;
      }
    }
  }
  if (!address) {
    // "the New York Times website" -> "new york times"; "la web de
    // YouTube" -> "youtube".
    if (const size_t n = LongestPhraseAt(p.site_prefixes, u.words, begin);
        n && end - begin > n) {
      begin += n;
    }
    for (std::string_view suffix : p.site_suffixes) {
      const size_t n = static_cast<size_t>(
          std::count(suffix.begin(), suffix.end(), ' ') + 1);
      if (end - begin > n && PhraseLengthAt(suffix, u.words, end - n) == n) {
        end -= n;
        break;
      }
    }
    // A name is a few words, and not a reference to something on screen
    // ("the live one", "the third result").
    if (end - begin > 5)
      return std::nullopt;
    for (size_t i = begin; i < end; ++i) {
      if (FindRoleWord(p, u.words[i]))
        return std::nullopt;
    }
    if (ReadPosition(u.words, begin, p, false))
      return std::nullopt;
  }
  Site site;
  site.address = address;
  bool pending_mark = false;
  char mark = 0;
  for (size_t i = begin; i < end; ++i) {
    const std::string_view w = u.words[i];
    char spoken_mark = 0;
    if (address) {
      for (const AddressWord& entry : p.address_words) {
        if (entry.word == w)
          spoken_mark = entry.mark;
      }
    }
    const SpokenWord& word = u.normalized.words[u.index[i]];
    const bool glued = i > begin && u.index[i] == u.index[i - 1] + 1;
    if (spoken_mark) {
      // A spoken mark glued to the word before by a written one
      // ("example.slash") is two marks in a row too.
      if (site.host.empty() || pending_mark || (glued && word.joiner))
        return std::nullopt;
      pending_mark = true;
      mark = spoken_mark;
      continue;
    }
    if (pending_mark) {
      // A spoken mark and a written one together ("slash.") are two marks
      // in a row: not an address.
      if (glued && word.joiner)
        return std::nullopt;
      site.host.push_back(mark);
      pending_mark = false;
    } else if (glued && word.joiner) {
      site.host.push_back(word.joiner);
    }
    site.host.append(w);
  }
  if (pending_mark || site.host.empty() || site.host.size() > kMaxSiteBytes) {
    return std::nullopt;
  }
  // No empty label, no mark at either end.
  const auto is_mark = [](char c) {
    return c == '.' || c == '/' || c == '-' || c == '_';
  };
  if (is_mark(site.host.front()) || is_mark(site.host.back())) {
    return std::nullopt;
  }
  for (size_t i = 1; i < site.host.size(); ++i) {
    if (is_mark(site.host[i]) && is_mark(site.host[i - 1]))
      return std::nullopt;
  }
  return site;
}

std::optional<ChoiceRef> ReadChoice(const Utterance& u,
                                    size_t begin,
                                    size_t end,
                                    SlotType type,
                                    Language language) {
  const WordList span(u.words.begin() + static_cast<std::ptrdiff_t>(begin),
                      u.words.begin() + static_cast<std::ptrdiff_t>(end));
  const bool bare = type != SlotType::kPosition;
  std::optional<ChoiceRef> ref = ParseChoiceRefWords(span, language, bare);
  if (!ref)
    return std::nullopt;
  if (type == SlotType::kPosition || type == SlotType::kNumber) {
    if (ref->kind == ChoiceRef::Kind::kDescription)
      return std::nullopt;
  }
  if (type == SlotType::kPick && ref->kind == ChoiceRef::Kind::kDescription &&
      ref->words.size() + ref->by_words.size() + ref->from_words.size() > 6) {
    return std::nullopt;  // a sentence, not a description
  }
  return ref;
}

// --- Matching a pattern
// -------------------------------------------------------

// |u.words| holds the words a pattern must take, all of them.
bool MatchPattern(const CompiledPattern& pattern,
                  const Utterance& u,
                  Language language,
                  const LanguagePhrases& p,
                  Captures& captures) {
  const WordList& words = u.words;
  const size_t end = words.size();
  size_t pos = 0;
  for (size_t i = 0; i < pattern.nodes.size(); ++i) {
    const Node& node = pattern.nodes[i];
    if (node.type != Node::Type::kSlot) {
      int taken = MatchNode(node, words, pos);
      if (taken < 0) {
        if (!node.optional)
          return false;
        taken = 0;
      }
      pos += static_cast<size_t>(taken);
      continue;
    }
    if (IsFixedSlot(node.slot)) {
      size_t taken = 0;
      bool ok = false;
      if (node.slot == SlotType::kSeconds) {
        if (const std::optional<SpokenDuration> d =
                ReadDuration(words, pos, p)) {
          captures.seconds = d->seconds;
          taken = d->words;
          ok = true;
        }
      } else if (node.slot == SlotType::kLevel) {
        if (const auto level = ReadLevel(words, pos, p)) {
          captures.level = level->first;
          taken = level->second;
          ok = true;
        }
      } else if (node.slot == SlotType::kSpeed) {
        if (const auto rate = ReadSpeed(u, pos, p)) {
          captures.rate = rate->first;
          taken = rate->second;
          ok = true;
        }
      } else if (const std::optional<Pages> pages = ReadPages(words, pos, p)) {
        captures.scroll = pages->amount;
        captures.count = pages->count;
        taken = pages->words;
        ok = true;
      }
      if (!ok && !node.optional)
        return false;
      pos += taken;
      continue;
    }
    // A rest slot: up to the next pattern word, or the end.
    size_t slot_end = end;
    if (i + 1 < pattern.nodes.size()) {
      const Node& next = pattern.nodes[i + 1];
      std::optional<size_t> found;
      for (size_t at = pos + 1; at < end; ++at) {
        if (MatchNode(next, words, at) > 0) {
          found = at;
          if (!IsGreedySlot(node.slot))
            break;
        }
      }
      if (!found)
        return false;
      slot_end = *found;
    }
    if (slot_end <= pos)
      return false;
    switch (node.slot) {
      case SlotType::kPosition:
      case SlotType::kNumber:
      case SlotType::kChoice:
      case SlotType::kPick:
      case SlotType::kTab: {
        std::optional<ChoiceRef> ref =
            ReadChoice(u, pos, slot_end, node.slot, language);
        if (!ref)
          return false;
        captures.choice = std::move(ref);
        break;
      }
      case SlotType::kSite:
      case SlotType::kAddress:
        if (!ReadSite(u, pos, slot_end, node.slot == SlotType::kAddress, p)) {
          return false;
        }
        captures.site = std::make_pair(pos, slot_end);
        captures.text_before_site = captures.text.has_value();
        break;
      case SlotType::kQuery:
      case SlotType::kText:
      case SlotType::kName:
        captures.text = std::make_pair(pos, slot_end);
        captures.text_ends_pattern = i + 1 == pattern.nodes.size();
        break;
      case SlotType::kSeconds:
      case SlotType::kLevel:
      case SlotType::kSpeed:
      case SlotType::kPages:
        return false;
    }
    pos = slot_end;
  }
  return pos == end;
}

bool ContextAllows(PatternContext context, const ParseContext& parse) {
  switch (context) {
    case PatternContext::kAlways:
      return true;
    case PatternContext::kYesNo:
      return parse.expecting_yes_no;
    case PatternContext::kChoice:
      return parse.expecting_choice;
    case PatternContext::kTextField:
      return parse.text_field_focused;
    case PatternContext::kSite:
      return parse.expecting_site;
  }
  return false;
}

// Normalized indices [first, last] of cleaned words [begin, end).
std::string SpanText(const Utterance& u, size_t begin, size_t end) {
  if (begin >= end)
    return std::string();
  return SourceText(u.source, u.normalized, u.index[begin], u.index[end - 1],
                    u.fillers);
}

bool SoundsAlike(std::string_view a,
                 std::string_view b,
                 const LanguagePhrases& p) {
  if (a == b)
    return true;
  if (a.size() < 3 || b.size() < 3)
    return false;
  if (p.wake_sound_margin > 0) {
    const std::string key_a = PhoneticKeyFor(p.language, a);
    const std::string key_b = PhoneticKeyFor(p.language, b);
    return !key_a.empty() && !key_b.empty() &&
           PhoneticDistance(key_a, key_b, p.wake_sound_margin) <=
               p.wake_sound_margin;
  }
  const std::string key = PhoneticKey(a);
  return !key.empty() && key == PhoneticKey(b);
}

// A one-word name the recognizer ran into its greeting ("Himalt", "him
// old", "Himaled" for "hey molt"): the first word, or the first two
// joined, sounds like a greeting and the name joined, to within one
// half-edit of the phonetic key (far from words like "himalaya" or
// "highmount"). Its length in words, or 0.
size_t RunTogetherWakeLength(const WordList& words,
                             const WordList& name,
                             const LanguagePhrases& p) {
  if (name.size() != 1 || words.empty() ||
      p.language != Language::kEnglish) {
    return 0;
  }
  for (size_t take = 1; take <= 2 && take <= words.size(); ++take) {
    std::string joined;
    for (size_t k = 0; k < take; ++k) {
      joined.append(words[k]);
    }
    if (joined.size() < 5 || joined.size() > 24) {
      continue;
    }
    const std::string key = PhoneticKey(joined);
    if (key.empty()) {
      continue;
    }
    for (std::string_view greeting : p.wake_greetings) {
      const std::string target =
          PhoneticKey(std::string(greeting) + std::string(name[0]));
      if (!target.empty() && PhoneticDistance(key, target, 1) <= 1) {
        return take;
      }
    }
  }
  return 0;
}

// Removes a leading wake phrase from |words| (the user's, or the
// language's default), with sound-alikes of its name ("hey mold"), or run
// together with its greeting ("Himalt").
size_t WakePhraseLength(const WordList& words,
                        std::string_view wake_phrase,
                        const LanguagePhrases& p) {
  const NormalizedText wake = NormalizeSpokenText(
      wake_phrase.empty() ? p.default_wake_phrase : wake_phrase, {});
  WordList name;
  for (size_t i = 0; i < wake.words.size(); ++i) {
    const std::string_view w = wake.Word(i);
    if (name.empty() && HasPhrase(p.wake_greetings, w))
      continue;
    name.push_back(w);
  }
  if (name.empty() || name.size() > 6)
    return 0;
  size_t at = 0;
  // Up to two greetings ("ok hey molt").
  while (at < 2 && at < words.size() &&
         HasPhrase(p.wake_greetings, words[at])) {
    ++at;
  }
  bool matched = at + name.size() <= words.size();
  for (size_t i = 0; matched && i < name.size(); ++i) {
    matched = SoundsAlike(words[at + i], name[i], p);
  }
  if (matched)
    return at + name.size();
  return RunTogetherWakeLength(words, name, p);
}

// kAskPage: which question, and its words. A topic ("what does it say
// about the battery") is the {query} the pattern took, as is the question
// after "ask the page"; any other question is the words the pattern
// matched, as heard ("who wrote this"), so the model is asked what the
// user asked.
void SetPageQuestion(PatternVariant variant,
                     const Utterance& u,
                     VoiceCommand& command) {
  switch (variant) {
    case PatternVariant::kAskMainPoints:
      command.page_question = PageQuestionKind::kMainPoints;
      break;
    case PatternVariant::kAskTopic:
      command.page_question = PageQuestionKind::kTopic;
      break;
    case PatternVariant::kAskQuestion:
      command.page_question = PageQuestionKind::kQuestion;
      break;
    case PatternVariant::kAskMore:
      command.page_question = PageQuestionKind::kMore;
      break;
    default:
      command.page_question = PageQuestionKind::kAbout;
      break;
  }
  if (command.text.empty()) {
    command.text =
        CleanSpokenText(SpanText(u, 0, u.words.size()), kMaxCommandTextBytes);
  }
}

// What CleanUp() leaves.
struct Cleaned {
  // The words the wake phrase took (normalized indices before it).
  size_t wake = 0;
  // Where the words end before the politeness that free text keeps.
  size_t soft_end = 0;
};

// Fills |u| from |utterance| (valid UTF-8, within kMaxUtteranceBytes): the
// wake phrase, fillers wherever they are, politeness and preambles at the
// start ("could you please", "okay") and at the end ("please"; "now"
// except in free text). |u.words| holds exactly the words the patterns
// take; |u.index| keeps the rest for free text that ends the utterance.
Cleaned CleanUp(std::string_view utterance,
                const ParseContext& context,
                const LanguagePhrases& p,
                Utterance& u) {
  Cleaned cleaned;
  u.source = utterance;
  NormalizeOptions options;
  options.marks = true;
  options.max_words = kMaxUtteranceBytes;
  u.normalized = NormalizeSpokenText(utterance, options);

  // The wake phrase, then fillers wherever they are.
  WordList all;
  for (size_t i = 0; i < u.normalized.words.size(); ++i) {
    all.push_back(u.normalized.Word(i));
  }
  cleaned.wake = WakePhraseLength(all, context.wake_phrase, p);
  for (size_t i = cleaned.wake; i < all.size(); ++i) {
    if (HasPhrase(p.fillers, all[i])) {
      u.fillers.push_back(i);
      continue;
    }
    u.words.push_back(all[i]);
    u.index.push_back(i);
  }

  // Politeness and preambles at the start ("could you please", "okay").
  size_t begin = 0;
  bool changed = true;
  while (changed && begin < u.words.size()) {
    changed = false;
    if (const size_t n = LongestPhraseAt(p.leading_phrases, u.words, begin)) {
      begin += n;
      changed = true;
    } else if (const size_t m =
                   LongestPhraseAt(p.leading_if_more, u.words, begin)) {
      if (begin + m < u.words.size()) {
        begin += m;
        changed = true;
      }
    }
  }
  if (begin > 0) {
    u.words.erase(u.words.begin(),
                  u.words.begin() + static_cast<std::ptrdiff_t>(begin));
    u.index.erase(u.index.begin(),
                  u.index.begin() + static_cast<std::ptrdiff_t>(begin));
  }
  // ... and at the end ("please"; "now" except in free text).
  const auto strip_end = [&](size_t end,
                             const std::vector<std::string_view>& phrases) {
    bool more = true;
    while (more && end > 1) {
      more = false;
      for (std::string_view phrase : phrases) {
        const size_t n = static_cast<size_t>(
            std::count(phrase.begin(), phrase.end(), ' ') + 1);
        if (end > n && PhraseLengthAt(phrase, u.words, end - n) == n) {
          end -= n;
          more = true;
          break;
        }
      }
    }
    return end;
  };
  cleaned.soft_end = strip_end(u.words.size(), p.trailing_phrases);
  size_t end = cleaned.soft_end;
  for (size_t before = end + 1; before != end;) {
    before = end;
    end = strip_end(end, p.trailing_if_command);
    end = strip_end(end, p.trailing_phrases);
  }
  u.words.resize(end);
  return cleaned;
}

}  // namespace

std::string CommandWords(std::string_view utterance,
                         const ParseContext& context) {
  if (utterance.size() > kMaxUtteranceBytes || !IsValidUtf8(utterance)) {
    return std::string();
  }
  Utterance u;
  CleanUp(utterance, context, PhrasesFor(context.language), u);
  std::string out;
  for (std::string_view word : u.words) {
    if (!out.empty()) {
      out += ' ';
    }
    out += word;
  }
  return out;
}

VoiceCommand::VoiceCommand() = default;
VoiceCommand::~VoiceCommand() = default;
VoiceCommand::VoiceCommand(const VoiceCommand&) = default;
VoiceCommand& VoiceCommand::operator=(const VoiceCommand&) = default;
VoiceCommand::VoiceCommand(VoiceCommand&&) = default;
VoiceCommand& VoiceCommand::operator=(VoiceCommand&&) = default;

std::string_view VoiceCommandKindName(VoiceCommandKind kind) {
  const size_t i = static_cast<size_t>(kind);
  return i < kKindNames.size() ? kKindNames[i] : std::string_view();
}

bool ActsOnTheBrowser(VoiceCommandKind kind) {
  using K = VoiceCommandKind;
  switch (kind) {
    case K::kBookmarkPage:
    case K::kRemoveBookmark:
    case K::kListBookmarks:
    case K::kOpenBookmark:
    case K::kListHistory:
    case K::kReopenTab:
    case K::kOpenFromHistory:
    case K::kListDownloads:
    case K::kOpenDownload:
    case K::kOpenSettings:
    case K::kAlwaysListening:
    case K::kProtectedSetting:
    case K::kNewWindow:
    case K::kCloseWindow:
    case K::kSwitchWindow:
    case K::kAskAI:
    case K::kOpenAIChat:
    case K::kShowBrowserButtons:
    case K::kOpenMenu:
    case K::kCloseMenu:
    // Starting over drops a question the browser waits on.
    case K::kStartOver:
      return true;
    default:
      return false;
  }
}

bool IsValidCommandPattern(std::string_view pattern) {
  return PatternCompiler(pattern).Compile().has_value();
}

const std::vector<CommandPattern>& CommandPatterns(Language language) {
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish: {
      static const std::vector<CommandPattern>* const spanish =
          new std::vector<CommandPattern>(MakeSpanishCommandPatterns());
      return *spanish;
    }
  }
  static const std::vector<CommandPattern>* const english =
      new std::vector<CommandPattern>(MakeEnglishCommandPatterns());
  return *english;
}

const std::vector<CommandInfo>& CommandInfos(Language language) {
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish: {
      static const std::vector<CommandInfo>* const spanish =
          new std::vector<CommandInfo>(MakeSpanishCommandInfos());
      return *spanish;
    }
  }
  static const std::vector<CommandInfo>* const english =
      new std::vector<CommandInfo>(MakeEnglishCommandInfos());
  return *english;
}

VoiceCommand ParseVoiceCommand(std::string_view utterance,
                               const ParseContext& context) {
  VoiceCommand command;
  if (utterance.size() > kMaxUtteranceBytes) {
    command.kind = VoiceCommandKind::kRefused;
    command.refusal = RefusalReason::kTooLong;
    return command;
  }
  if (!IsValidUtf8(utterance)) {
    command.kind = VoiceCommandKind::kRefused;
    command.refusal = RefusalReason::kInvalidUtf8;
    return command;
  }
  const LanguagePhrases& p = PhrasesFor(context.language);

  Utterance u;
  const Cleaned cleaned = CleanUp(utterance, context, p, u);
  const size_t wake = cleaned.wake;
  const size_t soft_end = cleaned.soft_end;
  const NormalizedText& normalized = u.normalized;
  const size_t all_size = normalized.words.size();
  if (wake < all_size) {
    command.heard = CleanSpokenText(
        SourceText(utterance, normalized, wake, all_size - 1, {}),
        kMaxUtteranceBytes);
  }
  if (u.words.empty())
    return command;  // nothing but the wake phrase or fillers

  for (const CompiledPattern& pattern : CompiledPatterns(context.language)) {
    if (!ContextAllows(pattern.context, context))
      continue;
    Captures captures;
    if (!MatchPattern(pattern, u, context.language, p, captures)) {
      continue;
    }
    command.kind = pattern.kind;
    if (captures.text) {
      size_t text_end = captures.text->second;
      // Free text that ends the utterance keeps "now", "for me".
      if (captures.text_ends_pattern)
        text_end = soft_end;
      command.text = SpanText(u, captures.text->first, text_end);
      const size_t cap = (pattern.kind == VoiceCommandKind::kRememberRoutine ||
                          pattern.kind == VoiceCommandKind::kForgetRoutine ||
                          pattern.kind == VoiceCommandKind::kRunRoutine ||
                          pattern.kind == VoiceCommandKind::kForgetWord)
                             ? kMaxRoutineNameBytes
                             : kMaxCommandTextBytes;
      if (command.text.empty() || command.text.size() > cap) {
        // Too long to act on: the planner gets it, never a cut-down copy.
        command = VoiceCommand();
        command.heard = CleanSpokenText(
            SourceText(utterance, u.normalized, wake, all_size - 1, {}),
            kMaxUtteranceBytes);
        return command;
      }
    }
    if (captures.site) {
      const std::optional<Site> site =
          ReadSite(u, captures.site->first, captures.site->second,
                   /*address_only=*/false, p);
      if (site) {
        command.site = site->host;
        command.site_is_address = site->address;
      }
      command.site_words =
          SpanText(u, captures.site->first, captures.site->second);
      if (captures.text && captures.text_before_site) {
        command.whole_query =
            SpanText(u, captures.text->first, captures.site->second);
      }
    }
    if (captures.choice)
      command.choice = std::move(*captures.choice);
    if (captures.seconds)
      command.seconds = *captures.seconds;
    if (captures.level)
      command.level = *captures.level;
    if (captures.rate)
      command.rate = *captures.rate;
    command.scroll = captures.scroll;
    command.count = captures.count;
    switch (pattern.variant) {
      case PatternVariant::kNone:
        break;
      case PatternVariant::kNewTab:
        command.new_tab = true;
        break;
      case PatternVariant::kNextTab:
      case PatternVariant::kPreviousTab:
        command.choice = ChoiceRef();
        command.choice.kind = ChoiceRef::Kind::kRelative;
        command.choice.relative = pattern.variant == PatternVariant::kNextTab
                                      ? ChoiceRef::Relative::kNext
                                      : ChoiceRef::Relative::kPrevious;
        break;
      case PatternVariant::kPageStep:
        command.scroll = ScrollAmount::kPage;
        command.count = 1;
        break;
      case PatternVariant::kVerbOpen:
        command.verb = ChoiceVerb::kOpen;
        break;
      case PatternVariant::kVerbPlay:
        command.verb = ChoiceVerb::kPlay;
        break;
      case PatternVariant::kVerbClick:
        command.verb = ChoiceVerb::kClick;
        break;
      case PatternVariant::kVerbSelect:
        command.verb = ChoiceVerb::kSelect;
        break;
      case PatternVariant::kVerbRead:
        command.verb = ChoiceVerb::kRead;
        break;
      case PatternVariant::kVerbDescribe:
        command.verb = ChoiceVerb::kDescribe;
        break;
      case PatternVariant::kScopeCurrentSite:
        command.scope = SearchScope::kCurrentSite;
        break;
      case PatternVariant::kScopeDefaultEngine:
        command.scope = SearchScope::kDefaultEngine;
        break;
      case PatternVariant::kScopeNamedSite:
        command.scope = SearchScope::kNamedSite;
        break;
      case PatternVariant::kBrief:
        command.verbosity = Verbosity::kBrief;
        break;
      case PatternVariant::kNormal:
        command.verbosity = Verbosity::kNormal;
        break;
      case PatternVariant::kFull:
        command.verbosity = Verbosity::kFull;
        break;
      case PatternVariant::kMore:
        command.verbosity = Verbosity::kMore;
        break;
      case PatternVariant::kLess:
        command.verbosity = Verbosity::kLess;
        break;
      case PatternVariant::kAskAbout:
      case PatternVariant::kAskMainPoints:
      case PatternVariant::kAskTopic:
      case PatternVariant::kAskQuestion:
      case PatternVariant::kAskMore:
        SetPageQuestion(pattern.variant, u, command);
        break;
      case PatternVariant::kSettingsPrivacy:
        command.settings_page = SettingsPage::kPrivacy;
        break;
      case PatternVariant::kSettingsAI:
        command.settings_page = SettingsPage::kAI;
        break;
      case PatternVariant::kSettingsVoice:
        command.settings_page = SettingsPage::kVoice;
        break;
      case PatternVariant::kSettingsMoltNet:
        command.settings_page = SettingsPage::kMoltNet;
        break;
      case PatternVariant::kWindowPrivate:
        command.window = WindowKind::kPrivate;
        break;
      case PatternVariant::kWindowMoltNet:
        command.window = WindowKind::kMoltNet;
        break;
      case PatternVariant::kOff:
        command.on = false;
        break;
      case PatternVariant::kWorkflowTrust:
        command.protected_setting = ProtectedSetting::kWorkflowTrust;
        break;
      case PatternVariant::kNewest:
        // The newest is the first the browser reads out.
        command.choice = ChoiceRef();
        command.choice.kind = ChoiceRef::Kind::kPosition;
        command.choice.ordinal = 1;
        break;
    }
    return command;
  }
  // A verb heard glued to the next word ("clickname a color", as the
  // bundled English recognizer hears "click name a colour"): once more,
  // apart. Only when nothing understood it, a remainder of at least four
  // letters ("clicking" stays as it is), and once (the retry starts with
  // the verb alone).
  const std::string_view first = u.words[0];
  for (std::string_view verb : p.glued_verbs) {
    if (first.size() < verb.size() + 4 || !first.starts_with(verb)) {
      continue;
    }
    std::string apart = std::string(verb) + " " +
                        std::string(first.substr(verb.size()));
    for (size_t i = 1; i < u.words.size(); ++i) {
      apart += " ";
      apart += u.words[i];
    }
    VoiceCommand again = ParseVoiceCommand(apart, context);
    if (again.kind != VoiceCommandKind::kUnknown &&
        again.kind != VoiceCommandKind::kRefused) {
      again.heard = command.heard;
      return again;
    }
    break;
  }
  return command;  // kUnknown: the planner gets |heard|
}

}  // namespace voice
}  // namespace molt_ai
