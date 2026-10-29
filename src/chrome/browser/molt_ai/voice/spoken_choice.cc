// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_choice.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/spoken_numbers.h"
#include "chrome/browser/molt_ai/voice/spoken_phrases.h"

namespace molt_ai {
namespace voice {

namespace {

// --- Scoring constants (see spoken_choice.h for the outline) ---------------

// A word match this good counts as the word being on the item, for the
// "how many items have this word" count that weighs words: an exact or
// plural spelling, or a sound-alike within about one close edit.
constexpr double kGoodMatch = 0.75;
// The weight of a word every item has. Not 0: such a word still says the
// user is talking about these items, it just can't pick one.
constexpr double kSharedWordWeight = 0.25;
// The weight of a word no item has (a misrecognition, or a word about
// something the items don't say): half a distinguishing word. It lowers
// every item's score alike, enough that two stray words in three keep a
// single matching word under kMinChoiceScore.
constexpr double kUnmatchedWordWeight = 0.5;
// The least a "by" word weighs when some item matches it: the user named
// the author on purpose, to rule others out, even when most items share it.
constexpr double kByCueWeight = 0.6;
// "by X" matched in a title rather than an author field ("Queen - Bohemian
// Rhapsody" with no channel shown) counts a little less.
constexpr double kByInTitle = 0.8;
// Words joined across a recognizer's split or merge match a little less
// than the same words spelled alike.
constexpr double kJoinedWords = 0.95;
// When some item has the word as spelled, other items' sound-alikes of it
// count this much of their score: the recognizer is biased toward the
// words on screen (docs/design/TOUCHLESS_BROWSING.md section 6.1), so a
// word it wrote that is on screen most likely means that item. Without
// it, "AirPods" would be close to "Earbuds" (the same key).
constexpr double kSoundAlikeWhenSpelled = 0.6;
// A match at least this good is a spelling match (exact, plural, or
// words joined as spelled), allowing for the table's rounding.
constexpr double kSpelledMatch = 0.94;
// Spelling matches.
constexpr double kExactWord = 1.0;
constexpr double kPluralWord = 0.95;
// Words shorter than this only match exactly: short keys collide too much.
constexpr size_t kMinSoundAlikeLetters = 3;
// A reference word shorter than this is not compared with two item words
// joined: a merge of two words is at least two short words long ("signin").
constexpr size_t kMinMergedLetters = 5;
// With an ordinal ("the second live one"), the items the description
// matches are those within this much of the best.
constexpr double kOrdinalBand = 0.25;
// Words read per title and per field.
constexpr size_t kMaxTitleWords = 64;
constexpr size_t kMaxFieldWords = 24;
// Distinct words compared across all items; words past it match nothing.
constexpr uint32_t kMaxVocabulary = 32768;
constexpr uint32_t kNoWord = UINT32_MAX;

enum Group : uint8_t { kDescription = 0, kBy = 1, kFrom = 2 };

// Segments of an item's text.
enum SegmentKind : uint8_t {
  kTitleSegment = 1,
  kAuthorSegment = 2,  // kAuthor and kSource fields
  kOtherSegment = 4,
};

// How many of each sound class a key has (phonetic_key.h's 13 classes).
using SoundCounts = std::array<uint8_t, 13>;

int SoundClass(char c) {
  switch (c) {
    case 'a':
      return 0;
    case 'P':
      return 1;
    case 'T':
      return 2;
    case 'K':
      return 3;
    case 'S':
      return 4;
    case 'X':
      return 5;
    case 'M':
      return 6;
    case 'N':
      return 7;
    case 'L':
      return 8;
    case 'R':
      return 9;
    case 'W':
      return 10;
    case 'Y':
      return 11;
    case 'H':
      return 12;
    default:
      return -1;
  }
}

SoundCounts CountSounds(std::string_view key) {
  SoundCounts counts{};
  for (char c : key) {
    const int k = SoundClass(c);
    if (k >= 0 && counts[static_cast<size_t>(k)] < 255) {
      ++counts[static_cast<size_t>(k)];
    }
  }
  return counts;
}

// A lower bound on PhoneticDistance(): every half-edit changes at most two
// class counts (a close substitution, at cost 1, moves a count from one
// class to another; an insertion or deletion costs at least 1 and changes
// one). So the distance is at least half the counts' L1 difference.
// |b_extra| is added to |b| (two words joined); class |b_seam| loses the
// sound the join collapsed (-1 for none).
int SoundsLowerBound(const SoundCounts& a,
                     const SoundCounts& b,
                     const SoundCounts* b_extra,
                     int b_seam) {
  int difference = 0;
  for (size_t k = 0; k < a.size(); ++k) {
    int other = b[k];
    if (b_extra)
      other += (*b_extra)[k];
    if (static_cast<int>(k) == b_seam)
      --other;
    difference += std::abs(static_cast<int>(a[k]) - other);
  }
  return (difference + 1) / 2;
}

// A word or joined words with what the comparisons need.
struct Sound {
  std::string text;
  std::string key;
  // A number word's digits ("eight" -> "8"); "" otherwise.
  std::string digits;
  SoundCounts counts{};
  bool numeric = false;
  bool sound_alike = false;  // long enough, and has a key

  static Sound Of(std::string text) {
    Sound s;
    s.numeric = !text.empty() &&
                std::all_of(text.begin(), text.end(),
                            [](char c) { return c >= '0' && c <= '9'; });
    s.key = s.numeric ? std::string() : PhoneticKey(text);
    s.counts = CountSounds(s.key);
    s.sound_alike = !s.key.empty() && text.size() >= kMinSoundAlikeLetters;
    s.text = std::move(text);
    return s;
  }
};

bool PluralOf(std::string_view a, std::string_view b) {
  // |a| is |b| plus "s" or "es".
  if (b.size() < 3 || a.size() <= b.size())
    return false;
  if (a.substr(0, b.size()) != b)
    return false;
  const std::string_view tail = a.substr(b.size());
  return tail == "s" || tail == "es";
}

// Spelling, then sound.
double Similarity(const Sound& ref, const Sound& word) {
  if (ref.text == word.text)
    return kExactWord;
  if (word.numeric && !ref.digits.empty() && ref.digits == word.text) {
    return kExactWord;
  }
  if (ref.numeric || word.numeric)
    return 0;
  if (PluralOf(ref.text, word.text) || PluralOf(word.text, ref.text)) {
    return kPluralWord;
  }
  if (!ref.sound_alike || !word.sound_alike)
    return 0;
  // Cheap bounds before the edit distance: each length difference needs
  // an insertion or deletion, and the sound counts must be close.
  const size_t longer = std::max(ref.key.size(), word.key.size());
  const int bound = PhoneticBound(longer);
  const size_t shorter = std::min(ref.key.size(), word.key.size());
  if (static_cast<int>(longer - shorter) > bound)
    return 0;
  if (SoundsLowerBound(ref.counts, word.counts, nullptr, -1) > bound)
    return 0;
  return PhoneticSimilarity(ref.key, word.key);
}

// A reference word against two adjacent item words read as one ("signin"
// for "Sign in", "rhapsody" for a title's "Rap City"). Filters on length
// and sound classes first; builds the joined key on the stack.
double JoinedSimilarity(const Sound& ref,
                        const Sound& left,
                        const Sound& right) {
  const size_t text_size = left.text.size() + right.text.size();
  if (ref.text.size() == text_size &&
      ref.text.compare(0, left.text.size(), left.text) == 0 &&
      ref.text.compare(left.text.size(), std::string::npos, right.text) == 0) {
    return kExactWord;
  }
  if (!ref.sound_alike || !left.sound_alike || !right.sound_alike)
    return 0;
  // A word a recognizer split or merged keeps its first sound.
  if (!SameOrCloseSounds(ref.key.front(), left.key.front()))
    return 0;
  const bool seam = left.key.back() == right.key.front();
  const size_t joined_size =
      left.key.size() + right.key.size() - (seam ? 1 : 0);
  const size_t key_size = ref.key.size();
  const int bound = PhoneticBound(std::max(joined_size, key_size));
  const size_t gap =
      joined_size > key_size ? joined_size - key_size : key_size - joined_size;
  if (static_cast<int>(gap) > bound)
    return 0;
  if (SoundsLowerBound(ref.counts, left.counts, &right.counts,
                       seam ? SoundClass(right.key.front()) : -1) > bound) {
    return 0;
  }
  std::array<char, 2 * kMaxPhoneticWordBytes + 8> joined;
  if (joined_size > joined.size())
    return 0;
  std::copy(left.key.begin(), left.key.end(), joined.begin());
  std::copy(right.key.begin() + (seam ? 1 : 0), right.key.end(),
            joined.begin() + static_cast<std::ptrdiff_t>(left.key.size()));
  return PhoneticSimilarity(ref.key,
                            std::string_view(joined.data(), joined_size));
}

// --- Reading the reference -----------------------------------------------

bool IsSingleLetter(std::string_view word) {
  return word.size() == 1 && word[0] >= 'a' && word[0] <= 'z';
}

// The description words of |words[begin, end)|: stopwords dropped, role
// words collected into |roles| (when |roles| is given), spelled letters
// joined ("b b c" -> "bbc"), number words as digits.
std::vector<std::string> DescriptionWords(const WordList& all_words,
                                          size_t begin,
                                          size_t end,
                                          const LanguagePhrases& phrases,
                                          ChoiceRoleMask* roles) {
  std::vector<std::string> out;
  if (begin >= end)
    return out;
  // The part on its own, so numbers can't read into the next part.
  const WordList words(all_words.begin() + static_cast<std::ptrdiff_t>(begin),
                       all_words.begin() + static_cast<std::ptrdiff_t>(end));
  end = words.size();
  size_t i = 0;
  while (i < end) {
    const std::string_view w = words[i];
    if (IsSingleLetter(w)) {
      std::string letters;
      while (i < end && IsSingleLetter(words[i]))
        letters.append(words[i++]);
      if (letters.size() > 1 ||
          !InSortedList(phrases.choice_stopwords, letters)) {
        out.push_back(std::move(letters));
      }
      continue;
    }
    if (roles) {
      if (const RoleWord* role = FindRoleWord(phrases, w)) {
        *roles |= role->roles;
        ++i;
        continue;
      }
    }
    if (InSortedList(phrases.choice_stopwords, w)) {
      ++i;
      continue;
    }
    if (const std::optional<SpokenNumber> year = ReadYear(words, i, phrases)) {
      out.push_back(std::to_string(year->value));
      i += year->words;
      continue;
    }
    NumberOptions options;
    options.ordinals = false;
    if (const std::optional<SpokenNumber> number =
            ReadNumber(words, i, phrases, options)) {
      // Numbers of several words become digits ("twenty one" -> "21"). A
      // single number word stays a word ("eight" may be a misheard "aid");
      // the matcher also compares its digits.
      if (number->words == 1) {
        out.emplace_back(w);
      } else {
        out.push_back(std::to_string(number->value));
      }
      i += number->words;
      continue;
    }
    out.emplace_back(w);
    ++i;
  }
  return out;
}

// Whether |words[at...]| is a role word (or nothing), and what it names.
bool RoleTail(const WordList& words,
              size_t at,
              const LanguagePhrases& phrases,
              ChoiceRoleMask* roles) {
  if (at == words.size())
    return true;
  if (at + 1 != words.size())
    return false;
  const RoleWord* role = FindRoleWord(phrases, words[at]);
  if (!role)
    return false;
  *roles |= role->roles;
  return true;
}

}  // namespace

ChoiceField::ChoiceField() = default;
ChoiceField::ChoiceField(ChoiceFieldKind kind, std::string text)
    : kind(kind), text(std::move(text)) {}
ChoiceField::~ChoiceField() = default;
ChoiceField::ChoiceField(const ChoiceField&) = default;
ChoiceField& ChoiceField::operator=(const ChoiceField&) = default;
ChoiceField::ChoiceField(ChoiceField&&) = default;
ChoiceField& ChoiceField::operator=(ChoiceField&&) = default;

ChoiceCandidate::ChoiceCandidate() = default;
ChoiceCandidate::~ChoiceCandidate() = default;
ChoiceCandidate::ChoiceCandidate(const ChoiceCandidate&) = default;
ChoiceCandidate& ChoiceCandidate::operator=(const ChoiceCandidate&) = default;
ChoiceCandidate::ChoiceCandidate(ChoiceCandidate&&) = default;
ChoiceCandidate& ChoiceCandidate::operator=(ChoiceCandidate&&) = default;

ChoiceRef::ChoiceRef() = default;
ChoiceRef::~ChoiceRef() = default;
ChoiceRef::ChoiceRef(const ChoiceRef&) = default;
ChoiceRef& ChoiceRef::operator=(const ChoiceRef&) = default;
ChoiceRef::ChoiceRef(ChoiceRef&&) = default;
ChoiceRef& ChoiceRef::operator=(ChoiceRef&&) = default;

ChoiceMatch::ChoiceMatch() = default;
ChoiceMatch::~ChoiceMatch() = default;
ChoiceMatch::ChoiceMatch(const ChoiceMatch&) = default;
ChoiceMatch& ChoiceMatch::operator=(const ChoiceMatch&) = default;
ChoiceMatch::ChoiceMatch(ChoiceMatch&&) = default;
ChoiceMatch& ChoiceMatch::operator=(ChoiceMatch&&) = default;

std::optional<ChoiceRef> ParseChoiceRefWords(const WordList& all_words,
                                             Language language,
                                             bool bare_numbers) {
  const LanguagePhrases& phrases = PhrasesFor(language);
  WordList words = all_words;
  if (words.empty())
    return std::nullopt;
  ChoiceRef ref;

  // "this one", "the next one", "the one above": the whole reference.
  const auto whole = [&](const std::vector<std::string_view>& list) {
    for (std::string_view phrase : list) {
      if (PhraseLengthAt(phrase, words, 0) == words.size())
        return true;
    }
    return false;
  };
  if (whole(phrases.current_phrases) || whole(phrases.next_phrases) ||
      whole(phrases.previous_phrases)) {
    ref.kind = ChoiceRef::Kind::kRelative;
    ref.relative =
        whole(phrases.current_phrases) ? ChoiceRef::Relative::kCurrent
        : whole(phrases.next_phrases)  ? ChoiceRef::Relative::kNext
                                       : ChoiceRef::Relative::kPrevious;
    return ref;
  }

  size_t i = 0;
  while (i < words.size() && HasPhrase(phrases.articles, words[i]))
    ++i;
  if (i == words.size())
    return std::nullopt;

  // A position: "the fourth one", "the last video", "the second live one".
  if (const std::optional<SpokenPosition> position =
          ReadPosition(words, i, phrases, /*homophones=*/true)) {
    ref.ordinal = position->index;
    ref.from_end = position->from_end;
    size_t after = i + position->words;
    ChoiceRoleMask roles = 0;
    if (RoleTail(words, after, phrases, &roles)) {
      ref.kind = ChoiceRef::Kind::kPosition;
      ref.roles = roles;
      return ref;
    }
    // More words: a description counted by the ordinal.
    i = after;
  } else {
    // A number: "number 12", "video 3", "12" (bare, when allowed).
    size_t at = i;
    ChoiceRoleMask roles = 0;
    bool prefixed = false;
    if (const size_t n = LongestPhraseAt(phrases.number_prefixes, words, at)) {
      at += n;
      prefixed = true;
    } else if (const RoleWord* role = FindRoleWord(phrases, words[at])) {
      if (role->roles && at + 1 < words.size()) {
        roles |= role->roles;
        ++at;
        prefixed = true;
      }
    }
    // "the one" is not a number; a bare "one" right after a list is.
    if (prefixed || (bare_numbers && i == 0)) {
      NumberOptions options;
      options.ordinals = false;
      options.homophones = true;
      const std::optional<SpokenNumber> number =
          ReadNumber(words, at, phrases, options);
      if (number && number->value >= 1 && number->value <= 999'999 &&
          RoleTail(words, at + number->words, phrases, &roles)) {
        ref.kind = ChoiceRef::Kind::kNumber;
        ref.number = static_cast<int>(number->value);
        ref.roles = roles;
        return ref;
      }
    }
  }

  // A description, with cues: the last "by" and the last "from" start
  // their parts ("Stand by Me by Ben E. King").
  size_t by_at = words.size();
  size_t by_len = 0;
  size_t from_at = words.size();
  size_t from_len = 0;
  for (size_t k = i; k < words.size(); ++k) {
    const size_t by = LongestPhraseAt(phrases.by_cues, words, k);
    const size_t from = LongestPhraseAt(phrases.from_cues, words, k);
    if (by && by >= from) {
      by_at = k;
      by_len = by;
      k += by - 1;
    } else if (from) {
      from_at = k;
      from_len = from;
      k += from - 1;
    }
  }
  const size_t description_end = std::min(by_at, from_at);
  ChoiceRoleMask roles = 0;
  ref.words = DescriptionWords(words, i, description_end, phrases, &roles);
  if (by_at < words.size()) {
    const size_t end = from_at > by_at ? from_at : words.size();
    ref.by_words =
        DescriptionWords(words, by_at + by_len, end, phrases, nullptr);
  }
  if (from_at < words.size()) {
    const size_t end = by_at > from_at ? by_at : words.size();
    ref.from_words =
        DescriptionWords(words, from_at + from_len, end, phrases, nullptr);
  }
  // Within the cap, description words first.
  size_t budget = kMaxChoiceRefWords;
  for (std::vector<std::string>* list :
       {&ref.words, &ref.by_words, &ref.from_words}) {
    if (list->size() > budget)
      list->resize(budget);
    budget -= list->size();
  }
  ref.roles = roles;
  if (ref.words.empty() && ref.by_words.empty() && ref.from_words.empty() &&
      !roles) {
    return std::nullopt;
  }
  ref.kind = ChoiceRef::Kind::kDescription;
  return ref;
}

std::optional<ChoiceRef> ParseChoiceRef(std::string_view spoken,
                                        Language language,
                                        bool bare_numbers) {
  if (spoken.size() > kMaxChoiceRefBytes || !IsValidUtf8(spoken)) {
    return std::nullopt;
  }
  NormalizeOptions options;
  options.marks = true;
  const NormalizedText text = NormalizeSpokenText(spoken, options);
  WordList words;
  words.reserve(text.words.size());
  for (size_t i = 0; i < text.words.size(); ++i)
    words.push_back(text.Word(i));
  return ParseChoiceRefWords(words, language, bare_numbers);
}

namespace {

// --- Matching a description ------------------------------------------------

struct Segment {
  uint32_t begin = 0;  // into Prepared::words
  uint32_t size = 0;
  uint8_t kind = 0;
};

struct Prepared {
  std::vector<uint32_t> words;  // vocabulary ids
  std::vector<Segment> segments;
};

class Vocabulary {
 public:
  explicit Vocabulary(size_t reserve_bytes) { arena_.reserve(reserve_bytes); }

  uint32_t Intern(std::string_view word) {
    const auto found = ids_.find(word);
    if (found != ids_.end())
      return found->second;
    if (sounds_.size() >= kMaxVocabulary ||
        arena_.size() + word.size() > arena_.capacity()) {
      return kNoWord;
    }
    // The arena never reallocates (reserved up front), so views into it
    // stay valid as keys.
    const size_t at = arena_.size();
    arena_.append(word);
    const std::string_view stored =
        std::string_view(arena_).substr(at, word.size());
    const uint32_t id = static_cast<uint32_t>(sounds_.size());
    ids_.emplace(stored, id);
    sounds_.push_back(Sound::Of(std::string(word)));
    return id;
  }

  const std::vector<Sound>& sounds() const { return sounds_; }

 private:
  std::string arena_;
  std::unordered_map<std::string_view, uint32_t> ids_;
  std::vector<Sound> sounds_;
};

std::string_view CutAtWord(std::string_view text, size_t cap) {
  if (text.size() <= cap)
    return text;
  size_t cut = cap;
  while (cut > 0 && text[cut] != ' ')
    --cut;
  return text.substr(0, cut ? cut : cap);
}

void AddSegment(std::string_view text,
                size_t cap_bytes,
                size_t cap_words,
                uint8_t kind,
                Vocabulary& vocabulary,
                Prepared& prepared) {
  NormalizeOptions options;
  options.max_words = cap_words;
  const NormalizedText normalized =
      NormalizeSpokenText(CutAtWord(text, cap_bytes), options);
  Segment segment;
  segment.begin = static_cast<uint32_t>(prepared.words.size());
  segment.kind = kind;
  for (size_t i = 0; i < normalized.words.size(); ++i) {
    prepared.words.push_back(vocabulary.Intern(normalized.Word(i)));
  }
  segment.size = static_cast<uint32_t>(prepared.words.size()) - segment.begin;
  if (segment.size)
    prepared.segments.push_back(segment);
}

// One unit of the reference: a word, or two or three adjacent words of
// the same group joined.
struct RefUnit {
  Sound sound;
  Group group = kDescription;
  size_t first = 0;  // index into the group's words
  size_t count = 1;
};

std::vector<RefUnit> MakeUnits(const std::vector<std::string>& words,
                               Group group,
                               const LanguagePhrases& phrases) {
  std::vector<RefUnit> units;
  for (size_t first = 0; first < words.size(); ++first) {
    std::string joined;
    for (size_t count = 1; count <= 3 && first + count <= words.size();
         ++count) {
      joined.append(words[first + count - 1]);
      RefUnit unit;
      unit.sound = Sound::Of(joined);
      if (count == 1) {
        const NumberWord* number = FindNumberWord(phrases, joined);
        if (number && !number->ordinal && !number->homophone &&
            number->kind != NumberWordKind::kHundred &&
            number->kind != NumberWordKind::kThousand) {
          unit.sound.digits = std::to_string(number->value);
        }
      }
      if (count > 1 && unit.sound.key.empty())
        break;
      unit.group = group;
      unit.first = first;
      unit.count = count;
      units.push_back(std::move(unit));
    }
  }
  return units;
}

struct Scored {
  size_t index = 0;
  int position = 0;
  double score = 0;
};

void Rank(std::vector<Scored>& scored) {
  std::stable_sort(scored.begin(), scored.end(),
                   [](const Scored& a, const Scored& b) {
                     if (a.score != b.score)
                       return a.score > b.score;
                     return a.position < b.position;
                   });
}

void FillRanked(const std::vector<Scored>& scored,
                const MatchOptions& options,
                ChoiceMatch& result) {
  for (const Scored& s : scored) {
    if (result.ranked.size() >= options.max_ranked || s.score <= 0)
      break;
    result.ranked.push_back(RankedChoice{s.index, s.score});
  }
}

ChoiceMatch Unique(size_t index) {
  ChoiceMatch result;
  result.outcome = ChoiceOutcome::kUnique;
  result.index = index;
  result.ranked.push_back(RankedChoice{index, 1.0});
  return result;
}

ChoiceMatch NoMatch(NoMatchReason reason) {
  ChoiceMatch result;
  result.reason = reason;
  return result;
}

ChoiceMatch MatchDescription(const ChoiceRef& ref,
                             const std::vector<ChoiceCandidate>& candidates,
                             const std::vector<size_t>& eligible,
                             const MatchOptions& options) {
  // A role and nothing else ("the search box"): the items with the role.
  std::vector<std::string> words = ref.words;
  std::vector<std::string> by_words = ref.by_words;
  std::vector<std::string> from_words = ref.from_words;
  // A reference built by hand may be longer than a parsed one.
  size_t budget = kMaxChoiceRefWords;
  for (std::vector<std::string>* list : {&words, &by_words, &from_words}) {
    if (list->size() > budget)
      list->resize(budget);
    budget -= list->size();
  }

  // Read every item into the shared vocabulary.
  size_t bytes = 0;
  for (size_t index : eligible) {
    const ChoiceCandidate& c = candidates[index];
    bytes += std::min(c.title.size(), kMaxChoiceTitleBytes);
    size_t fields = 0;
    for (const ChoiceField& field : c.fields) {
      if (++fields > kMaxChoiceFields)
        break;
      bytes += std::min(field.text.size(), kMaxChoiceFieldBytes);
    }
  }
  Vocabulary vocabulary(bytes + 16);
  std::vector<Prepared> prepared(eligible.size());
  for (size_t e = 0; e < eligible.size(); ++e) {
    const ChoiceCandidate& c = candidates[eligible[e]];
    AddSegment(c.title, kMaxChoiceTitleBytes, kMaxTitleWords, kTitleSegment,
               vocabulary, prepared[e]);
    size_t fields = 0;
    for (const ChoiceField& field : c.fields) {
      if (++fields > kMaxChoiceFields)
        break;
      const bool author = field.kind == ChoiceFieldKind::kAuthor ||
                          field.kind == ChoiceFieldKind::kSource;
      AddSegment(field.text, kMaxChoiceFieldBytes, kMaxFieldWords,
                 author ? kAuthorSegment : kOtherSegment, vocabulary,
                 prepared[e]);
    }
  }
  const std::vector<Sound>& sounds = vocabulary.sounds();
  const size_t n = eligible.size();
  const LanguagePhrases& phrases = PhrasesFor(options.language);

  // How well each reference word matches each item, for |group| words
  // matched in segments of |kinds| (the title at |title_factor| when it
  // isn't one of them).
  const auto word_matches = [&](const std::vector<std::string>& group_words,
                                Group group, uint8_t kinds,
                                double title_factor) {
    std::vector<std::vector<double>> best(group_words.size(),
                                          std::vector<double>(n, 0.0));
    if (group_words.empty())
      return best;
    const std::vector<RefUnit> units = MakeUnits(group_words, group, phrases);
    // Single words long enough to be two words merged ("signin").
    std::vector<size_t> single_units;
    for (size_t u = 0; u < units.size(); ++u) {
      if (units[u].count == 1 &&
          units[u].sound.text.size() >= kMinMergedLetters) {
        single_units.push_back(u);
      }
    }
    // Each unit against each distinct word, once; |hits| says which units
    // a word matches at all (most match none), so items skip the rest.
    // At most 12 words make at most 33 units.
    static_assert(3 * kMaxChoiceRefWords <= 64);
    std::vector<uint8_t> table(units.size() * sounds.size(), 0);
    std::vector<uint64_t> hits(sounds.size(), 0);
    for (size_t u = 0; u < units.size(); ++u) {
      for (size_t v = 0; v < sounds.size(); ++v) {
        const Sound& unit = units[u].sound;
        const Sound& word = sounds[v];
        double s = 0;
        if (units[u].count == 1) {
          s = Similarity(unit, word);
        } else if (unit.text == word.text) {
          s = kJoinedWords * kExactWord;  // "mac book" / "MacBook"
        } else if (unit.sound_alike && word.sound_alike &&
                   SameOrCloseSounds(unit.key.front(), word.key.front())) {
          // Words a recognizer split keep their first sound ("rap city" /
          // "rhapsody"), as JoinedSimilarity() assumes for merges.
          s = kJoinedWords * Similarity(unit, word);
        }
        const auto q = static_cast<uint8_t>(std::lround(s * 250));
        table[u * sounds.size() + v] = q;
        if (q)
          hits[v] |= uint64_t{1} << u;
      }
    }
    for (size_t e = 0; e < n; ++e) {
      const Prepared& item = prepared[e];
      for (const Segment& segment : item.segments) {
        const bool in_kinds = (segment.kind & kinds) != 0;
        const bool title_only =
            !in_kinds && segment.kind == kTitleSegment && title_factor > 0;
        if (!in_kinds && !title_only)
          continue;
        const double factor = in_kinds ? 1.0 : title_factor;
        for (uint32_t k = 0; k < segment.size; ++k) {
          const uint32_t id = item.words[segment.begin + k];
          if (id == kNoWord)
            continue;
          for (uint64_t m = hits[id]; m; m &= m - 1) {
            const auto u = static_cast<size_t>(std::countr_zero(m));
            const uint8_t q = table[u * sounds.size() + id];
            const double s = factor * q / 250.0;
            for (size_t w = units[u].first; w < units[u].first + units[u].count;
                 ++w) {
              best[w][e] = std::max(best[w][e], s);
            }
          }
          // A reference word against two adjacent item words joined
          // ("signin" / "Sign in"), when it hasn't matched well yet.
          if (k + 1 >= segment.size)
            continue;
          const uint32_t next = item.words[segment.begin + k + 1];
          if (next == kNoWord)
            continue;
          const Sound& left = sounds[id];
          const Sound& right = sounds[next];
          for (size_t u : single_units) {
            const RefUnit& unit = units[u];
            if (best[unit.first][e] >= kGoodMatch)
              continue;
            const double s = JoinedSimilarity(unit.sound, left, right);
            if (s > 0) {
              best[unit.first][e] =
                  std::max(best[unit.first][e], s * kJoinedWords * factor);
            }
          }
        }
      }
    }
    for (std::vector<double>& row : best) {
      const bool spelled = std::any_of(
          row.begin(), row.end(), [](double s) { return s >= kSpelledMatch; });
      if (!spelled)
        continue;
      for (double& s : row) {
        if (s < kSpelledMatch)
          s *= kSoundAlikeWhenSpelled;
      }
    }
    return best;
  };

  constexpr uint8_t kAll = kTitleSegment | kAuthorSegment | kOtherSegment;
  std::vector<std::vector<double>> by_best =
      word_matches(by_words, kBy, kAuthorSegment, kByInTitle);
  // "by" naming nobody ("Stand by me"): its words describe the item.
  if (!by_words.empty()) {
    bool any = false;
    for (const std::vector<double>& row : by_best) {
      for (double s : row)
        any = any || s >= kGoodMatch;
    }
    if (!any) {
      words.insert(words.end(), by_words.begin(), by_words.end());
      by_words.clear();
      by_best.clear();
    }
  }
  std::vector<std::vector<double>> word_best =
      word_matches(words, kDescription, kAll, 0);
  std::vector<std::vector<double>> from_best =
      word_matches(from_words, kFrom, kAll, 0);

  // Weigh each word by how few items it matches.
  std::vector<double> scores(n, 0.0);
  double total_weight = 0;
  const double log_n = std::log(static_cast<double>(n) + 1.0);
  const auto add = [&](const std::vector<std::vector<double>>& best,
                       const std::vector<std::string>& group_words,
                       bool by_cue) {
    for (size_t w = 0; w < best.size(); ++w) {
      const std::vector<double>& row = best[w];
      size_t df = 0;
      for (double s : row)
        df += s >= kGoodMatch ? 1 : 0;
      // A particle no item has says nothing ("the one with the guitar").
      double weight = InSortedList(phrases.choice_particles, group_words[w])
                          ? 0.0
                          : kUnmatchedWordWeight;
      if (df > 0) {
        const double idf = std::log((static_cast<double>(n) + 1.0) /
                                    (static_cast<double>(df) + 1.0)) /
                           log_n;
        weight = kSharedWordWeight + (1.0 - kSharedWordWeight) * idf;
        if (by_cue)
          weight = std::max(weight, kByCueWeight);
      }
      total_weight += weight;
      for (size_t e = 0; e < n; ++e)
        scores[e] += weight * row[e];
    }
  };
  add(word_best, words, false);
  add(by_best, by_words, true);
  add(from_best, from_words, false);

  std::vector<Scored> scored(n);
  for (size_t e = 0; e < n; ++e) {
    scored[e].index = eligible[e];
    scored[e].position = candidates[eligible[e]].position;
    // Only a role ("the search box"): every item with it matches fully.
    scored[e].score =
        total_weight > 0 ? scores[e] / total_weight : (ref.roles ? 1.0 : 0.0);
  }
  Rank(scored);

  ChoiceMatch result;
  FillRanked(scored, options, result);
  const double top = scored.empty() ? 0 : scored[0].score;
  if (top < options.min_score) {
    result.reason = NoMatchReason::kNothingMatched;
    return result;
  }

  // "The second live one": the ordinal-th matching item in list order.
  if (ref.ordinal > 0) {
    std::vector<Scored> matching;
    for (const Scored& s : scored) {
      if (s.score >= options.min_score && s.score >= top - kOrdinalBand) {
        matching.push_back(s);
      }
    }
    std::stable_sort(matching.begin(), matching.end(),
                     [](const Scored& a, const Scored& b) {
                       return a.position < b.position;
                     });
    const size_t k = static_cast<size_t>(ref.ordinal);
    if (k > matching.size()) {
      result.reason = NoMatchReason::kOutOfRange;
      return result;
    }
    const Scored& chosen = matching[ref.from_end ? matching.size() - k : k - 1];
    result.outcome = ChoiceOutcome::kUnique;
    result.index = chosen.index;
    return result;
  }

  const double second = scored.size() > 1 ? scored[1].score : 0;
  if (top - second >= options.margin) {
    result.outcome = ChoiceOutcome::kUnique;
    result.index = scored[0].index;
    return result;
  }
  result.outcome = ChoiceOutcome::kAmbiguous;
  for (const Scored& s : scored) {
    if (result.close.size() >= kMaxCloseChoices ||
        top - s.score >= options.margin) {
      break;
    }
    result.close.push_back(s.index);
  }
  return result;
}

}  // namespace

ChoiceMatch MatchSpokenChoice(const ChoiceRef& ref,
                              const std::vector<ChoiceCandidate>& candidates,
                              const MatchOptions& options) {
  const size_t count = std::min(candidates.size(), kMaxChoiceCandidates);
  if (count == 0)
    return NoMatch(NoMatchReason::kNoCandidates);
  if (ref.kind == ChoiceRef::Kind::kNone) {
    return NoMatch(NoMatchReason::kEmptyReference);
  }

  // A role word narrows the items when any item has a role it names.
  std::vector<size_t> eligible;
  if (ref.roles) {
    for (size_t i = 0; i < count; ++i) {
      if (RoleBit(candidates[i].role) & ref.roles)
        eligible.push_back(i);
    }
  }
  if (eligible.empty()) {
    for (size_t i = 0; i < count; ++i)
      eligible.push_back(i);
  }
  // In list order.
  std::stable_sort(eligible.begin(), eligible.end(), [&](size_t a, size_t b) {
    return candidates[a].position < candidates[b].position;
  });

  switch (ref.kind) {
    case ChoiceRef::Kind::kNone:
      break;
    case ChoiceRef::Kind::kNumber: {
      bool numbered = false;
      for (size_t i = 0; i < count; ++i) {
        numbered = numbered || candidates[i].screen_number > 0;
      }
      for (size_t index : eligible) {
        const ChoiceCandidate& c = candidates[index];
        if ((numbered ? c.screen_number : c.position) == ref.number) {
          return Unique(index);
        }
      }
      return NoMatch(NoMatchReason::kOutOfRange);
    }
    case ChoiceRef::Kind::kPosition: {
      const size_t k = static_cast<size_t>(std::max(ref.ordinal, 0));
      if (k == 0 || k > eligible.size()) {
        return NoMatch(NoMatchReason::kOutOfRange);
      }
      return Unique(eligible[ref.from_end ? eligible.size() - k : k - 1]);
    }
    case ChoiceRef::Kind::kRelative: {
      const int current = options.current_position;
      if (current <= 0)
        return NoMatch(NoMatchReason::kNoCurrentItem);
      std::optional<size_t> found;
      for (size_t index : eligible) {
        const int position = candidates[index].position;
        switch (ref.relative) {
          case ChoiceRef::Relative::kCurrent:
            if (position == current)
              found = index;
            break;
          case ChoiceRef::Relative::kNext:
            if (position > current && !found)
              found = index;
            break;
          case ChoiceRef::Relative::kPrevious:
            if (position < current)
              found = index;
            break;
        }
      }
      if (!found)
        return NoMatch(NoMatchReason::kOutOfRange);
      return Unique(*found);
    }
    case ChoiceRef::Kind::kDescription:
      return MatchDescription(ref, candidates, eligible, options);
  }
  return NoMatch(NoMatchReason::kEmptyReference);
}

}  // namespace voice
}  // namespace molt_ai
