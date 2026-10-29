// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The spoken-choice matcher (voice/spoken_choice.h): the items come from
// page text (titles, channels, dates: any bytes), the reference from what
// the user said. The input is text, so seeds and the dictionary stay
// readable:
//   byte 0      flags: bit 0 bare numbers; bit 1 screen numbers; bit 2
//               positions out of order (reversed, with gaps); bit 3 roles
//               from the item's line; bit 4 a reference built from the
//               bytes of line 0 instead of parsed; bits 5-7 the current
//               position (0-7)
//   line 0      the reference ("the live one by queen")
//   lines 1...  items: title, then tab-separated fields (author, then
//               details)
//
// Checks: ParseChoiceRef() gives a reference that names something, within
// its caps; the match is the same every time, and the same (by position)
// with the items passed in reverse order; a unique match is a valid index,
// and for positions, numbers and relations it is the item an independent
// computation finds; an ambiguous match has 1 to kMaxCloseChoices distinct
// valid items, the best first, all within the margin; ranked items are
// distinct, valid, scored in (0, 1], best first, at most max_ranked; a
// unique description leads by the margin and reaches the minimum score;
// no match says why.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/check_op.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"

namespace {

using molt_ai::voice::ChoiceCandidate;
using molt_ai::voice::ChoiceField;
using molt_ai::voice::ChoiceFieldKind;
using molt_ai::voice::ChoiceMatch;
using molt_ai::voice::ChoiceOutcome;
using molt_ai::voice::ChoiceRef;
using molt_ai::voice::ChoiceRole;
using molt_ai::voice::kMaxChoiceCandidates;
using molt_ai::voice::kMaxChoiceRefWords;
using molt_ai::voice::kMaxCloseChoices;
using molt_ai::voice::Language;
using molt_ai::voice::MatchOptions;
using molt_ai::voice::MatchSpokenChoice;
using molt_ai::voice::NoMatchReason;
using molt_ai::voice::ParseChoiceRef;
using molt_ai::voice::RoleBit;

std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  while (true) {
    const size_t at = text.find(separator);
    parts.push_back(text.substr(0, at));
    if (at == std::string_view::npos)
      break;
    text.remove_prefix(at + 1);
  }
  return parts;
}

// A reference from raw bytes, to reach values parsing never makes.
ChoiceRef RawRef(std::string_view bytes) {
  ChoiceRef ref;
  const auto at = [&](size_t i) -> uint8_t {
    return i < bytes.size() ? static_cast<uint8_t>(bytes[i]) : 0;
  };
  ref.kind = static_cast<ChoiceRef::Kind>(at(0) % 5);
  ref.ordinal = static_cast<int8_t>(at(1));
  ref.from_end = at(2) & 1;
  ref.number = static_cast<int8_t>(at(3));
  ref.relative = static_cast<ChoiceRef::Relative>(at(4) % 3);
  ref.roles = at(5) & 1 ? (1u << (at(6) % 16)) : 0;
  for (std::string_view word :
       Split(bytes.size() > 7 ? bytes.substr(7) : "", ' ')) {
    if (!word.empty() && ref.words.size() < 8)
      ref.words.emplace_back(word);
  }
  return ref;
}

// What the item at a position, number or relation is, computed without
// the matcher: nullopt when there is none.
std::optional<int> ExpectedPosition(const ChoiceRef& ref,
                                    const std::vector<ChoiceCandidate>& list,
                                    int current) {
  const size_t count = std::min(list.size(), kMaxChoiceCandidates);
  std::vector<int> positions;
  for (size_t i = 0; i < count; ++i) {
    if (!ref.roles || (RoleBit(list[i].role) & ref.roles)) {
      positions.push_back(list[i].position);
    }
  }
  if (positions.empty()) {
    for (size_t i = 0; i < count; ++i)
      positions.push_back(list[i].position);
  }
  std::stable_sort(positions.begin(), positions.end());
  switch (ref.kind) {
    case ChoiceRef::Kind::kPosition: {
      if (ref.ordinal < 1 ||
          static_cast<size_t>(ref.ordinal) > positions.size()) {
        return std::nullopt;
      }
      const size_t k = static_cast<size_t>(ref.ordinal);
      return positions[ref.from_end ? positions.size() - k : k - 1];
    }
    case ChoiceRef::Kind::kRelative: {
      if (current <= 0)
        return std::nullopt;
      std::optional<int> found;
      for (int p : positions) {
        if (ref.relative == ChoiceRef::Relative::kCurrent && p == current) {
          found = p;
        }
        if (ref.relative == ChoiceRef::Relative::kNext && p > current &&
            !found) {
          found = p;
        }
        if (ref.relative == ChoiceRef::Relative::kPrevious && p < current) {
          found = p;
        }
      }
      return found;
    }
    default:
      return std::nullopt;
  }
}

void CheckMatch(const ChoiceMatch& m,
                const ChoiceRef& ref,
                const std::vector<ChoiceCandidate>& list,
                const MatchOptions& options) {
  const size_t count = std::min(list.size(), kMaxChoiceCandidates);
  CHECK_LE(m.ranked.size(), options.max_ranked);
  std::set<size_t> seen;
  for (size_t i = 0; i < m.ranked.size(); ++i) {
    CHECK_LT(m.ranked[i].index, count);
    CHECK(seen.insert(m.ranked[i].index).second);
    CHECK_GT(m.ranked[i].score, 0.0);
    CHECK_LE(m.ranked[i].score, 1.0);
    if (i > 0)
      CHECK_LE(m.ranked[i].score, m.ranked[i - 1].score);
  }
  switch (m.outcome) {
    case ChoiceOutcome::kUnique:
      CHECK_LT(m.index, count);
      CHECK(m.reason == NoMatchReason::kNone);
      CHECK(!m.ranked.empty());
      if (ref.kind == ChoiceRef::Kind::kDescription && ref.ordinal == 0) {
        CHECK_EQ(m.ranked[0].index, m.index);
        CHECK_GE(m.ranked[0].score, options.min_score);
        const double second = m.ranked.size() > 1 ? m.ranked[1].score : 0.0;
        CHECK_GE(m.ranked[0].score - second, options.margin - 1e-9);
      }
      break;
    case ChoiceOutcome::kAmbiguous: {
      CHECK(ref.kind == ChoiceRef::Kind::kDescription);
      CHECK(!m.close.empty());
      CHECK_LE(m.close.size(), kMaxCloseChoices);
      CHECK_EQ(m.close[0], m.ranked[0].index);
      CHECK_GE(m.ranked[0].score, options.min_score);
      std::set<size_t> distinct(m.close.begin(), m.close.end());
      CHECK_EQ(distinct.size(), m.close.size());
      for (size_t index : m.close)
        CHECK_LT(index, count);
      break;
    }
    case ChoiceOutcome::kNoMatch:
      CHECK(m.reason != NoMatchReason::kNone);
      break;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0)
    return 0;
  const uint8_t flags = data[0];
  const std::string_view text(reinterpret_cast<const char*>(data + 1),
                              size - 1);
  const std::vector<std::string_view> lines = Split(text, '\n');

  // The reference.
  ChoiceRef ref;
  if (flags & 16) {
    ref = RawRef(lines[0]);
  } else {
    const std::optional<ChoiceRef> parsed =
        ParseChoiceRef(lines[0], Language::kEnglish, flags & 1);
    if (!parsed)
      return 0;
    ref = *parsed;
    CHECK(ref.kind != ChoiceRef::Kind::kNone);
    const size_t words =
        ref.words.size() + ref.by_words.size() + ref.from_words.size();
    CHECK_LE(words, kMaxChoiceRefWords);
    if (ref.kind == ChoiceRef::Kind::kDescription) {
      CHECK(words > 0 || ref.roles != 0);
    }
    if (ref.kind == ChoiceRef::Kind::kPosition)
      CHECK_GE(ref.ordinal, 1);
    if (ref.kind == ChoiceRef::Kind::kNumber)
      CHECK_GE(ref.number, 1);
  }

  // The items.
  std::vector<ChoiceCandidate> list;
  for (size_t line = 1; line < lines.size(); ++line) {
    ChoiceCandidate c;
    const int index = static_cast<int>(line);
    c.position =
        (flags & 4) ? 3 * (static_cast<int>(lines.size()) - index) : index;
    if (flags & 2)
      c.screen_number = index + 10;
    if (flags & 8) {
      c.role = static_cast<ChoiceRole>(lines[line].size() % 16);
    }
    const std::vector<std::string_view> parts = Split(lines[line], '\t');
    c.title = std::string(parts[0]);
    for (size_t f = 1; f < parts.size(); ++f) {
      c.fields.emplace_back(f == 1 ? ChoiceFieldKind::kAuthor
                                   : static_cast<ChoiceFieldKind>(f % 7),
                            std::string(parts[f]));
    }
    list.push_back(std::move(c));
  }

  MatchOptions options;
  options.current_position = (flags >> 5) & 7;
  const ChoiceMatch m = MatchSpokenChoice(ref, list, options);
  CheckMatch(m, ref, list, options);

  // The same every time.
  const ChoiceMatch again = MatchSpokenChoice(ref, list, options);
  CHECK(again.outcome == m.outcome);
  CHECK_EQ(again.index, m.index);
  CHECK(again.close == m.close);
  CHECK_EQ(again.ranked.size(), m.ranked.size());

  // Positions, numbers and relations agree with an independent reading.
  if (ref.kind == ChoiceRef::Kind::kPosition ||
      ref.kind == ChoiceRef::Kind::kRelative) {
    const std::optional<int> expected =
        ExpectedPosition(ref, list, options.current_position);
    CHECK_EQ(expected.has_value(), m.outcome == ChoiceOutcome::kUnique);
    if (expected)
      CHECK_EQ(list[m.index].position, *expected);
  }
  if (ref.kind == ChoiceRef::Kind::kNumber &&
      m.outcome == ChoiceOutcome::kUnique) {
    const ChoiceCandidate& chosen = list[m.index];
    CHECK_EQ((flags & 2) ? chosen.screen_number : chosen.position, ref.number);
  }

  // Order doesn't matter: the same item, by position, from the list
  // reversed (positions are distinct here).
  if (list.size() <= kMaxChoiceCandidates) {
    std::vector<ChoiceCandidate> reversed(list.rbegin(), list.rend());
    const ChoiceMatch r = MatchSpokenChoice(ref, reversed, options);
    CHECK(r.outcome == m.outcome);
    if (m.outcome == ChoiceOutcome::kUnique) {
      CHECK_EQ(reversed[r.index].position, list[m.index].position);
    }
    if (m.outcome == ChoiceOutcome::kAmbiguous) {
      CHECK_EQ(r.close.size(), m.close.size());
    }
  }
  return 0;
}
