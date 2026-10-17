// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

#include <algorithm>

namespace molt_ai {

namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

// Largest k in [1, tag.size()-1] such that |s| ends with tag[0, k).
size_t PartialTagSuffix(std::string_view s, std::string_view tag) {
  const size_t max_k = std::min(s.size(), tag.size() - 1);
  for (size_t k = max_k; k > 0; --k) {
    if (s.substr(s.size() - k) == tag.substr(0, k)) return k;
  }
  return 0;
}

}  // namespace

// ---- HarmonyChannelFilter -------------------------------------------------

std::string HarmonyChannelFilter::Feed(const std::string& piece) {
  // Harmony control tokens arrive as whole special-token pieces.
  bool is_ctrl = true;
  if (piece == "<|channel|>") {
    harmony_ = true;
    state_ = kChannelName;
    channel_.clear();
    emit_ = false;
  } else if (piece == "<|message|>") {
    if (harmony_) {
      state_ = kBody;
      emit_ = channel_.find("final") != std::string::npos;
    } else {
      is_ctrl = false;
    }
  } else if (piece == "<|start|>" || piece == "<|end|>" ||
             piece == "<|return|>" || piece == "<|constrain|>" ||
             piece == "<|call|>") {
    if (harmony_) {
      state_ = kOutside;  // message boundary; next channel decides
      emit_ = false;
    } else {
      is_ctrl = false;
    }
  } else {
    is_ctrl = false;
  }

  if (is_ctrl) return std::string();
  if (!harmony_) return piece;  // normal model — pass everything through
  if (state_ == kChannelName) {
    channel_ += piece;  // building the channel name; don't surface
    return std::string();
  }
  if (state_ == kBody && emit_) return piece;  // final channel body
  // analysis/commentary body or a role name between messages — drop
  return std::string();
}

// ---- GemmaThoughtFilter ---------------------------------------------------

GemmaThoughtFilter::GemmaThoughtFilter(bool start_inside)
    : depth_(start_inside ? 1 : 0) {}

std::string GemmaThoughtFilter::EmitVisible(std::string_view text) {
  if (strip_leading_ws_) {
    size_t i = 0;
    while (i < text.size() && IsSpace(text[i])) ++i;
    text.remove_prefix(i);
    if (!text.empty()) strip_leading_ws_ = false;
  }
  return std::string(text);
}

std::string GemmaThoughtFilter::Feed(std::string_view piece) {
  std::string out;
  pending_.append(piece);
  const std::string_view open(kOpenMarker);
  const std::string_view close(kCloseMarker);
  while (!pending_.empty()) {
    const size_t open_pos = pending_.find(open);
    const size_t close_pos = pending_.find(close);
    const size_t first = std::min(open_pos, close_pos);
    if (first == std::string::npos) {
      // No whole marker: keep a possible marker prefix, hide or show the
      // rest.
      const size_t hold = std::max(PartialTagSuffix(pending_, open),
                                   PartialTagSuffix(pending_, close));
      if (depth_ == 0) {
        out += EmitVisible(
            std::string_view(pending_).substr(0, pending_.size() - hold));
      }
      pending_.erase(0, pending_.size() - hold);
      break;
    }
    if (depth_ == 0) {
      out += EmitVisible(std::string_view(pending_).substr(0, first));
    }
    if (first == open_pos) {
      ++depth_;
      pending_.erase(0, first + open.size());
    } else {
      // A close: ends one level, or is an orphan (dropped) at depth 0.
      if (depth_ > 0) --depth_;
      pending_.erase(0, first + close.size());
    }
  }
  return out;
}

std::string GemmaThoughtFilter::Flush() {
  std::string out;
  if (depth_ == 0) out = EmitVisible(pending_);
  pending_.clear();
  return out;
}

void GemmaThoughtFilter::ForceClose() {
  if (depth_ == 0) return;
  depth_ = 0;
  pending_.clear();  // a held-back marker prefix inside the block
}

// static
bool GemmaThoughtFilter::EndsInsideThought(std::string_view prompt_tail) {
  GemmaThoughtFilter f;
  f.Feed(prompt_tail);
  return f.hiding();
}

// ---- ThinkTagFilter -------------------------------------------------------

ThinkTagFilter::ThinkTagFilter(bool start_inside)
    : in_think_(start_inside), saw_think_(start_inside) {}

std::string ThinkTagFilter::EmitVisible(std::string_view text) {
  if (strip_leading_ws_) {
    size_t i = 0;
    while (i < text.size() && IsSpace(text[i])) ++i;
    text.remove_prefix(i);
    if (!text.empty()) strip_leading_ws_ = false;
  }
  if (!visible_started_) {
    for (char c : text) {
      if (!IsSpace(c)) {
        visible_started_ = true;
        break;
      }
    }
  }
  return std::string(text);
}

std::string ThinkTagFilter::Feed(std::string_view piece) {
  std::string out;
  pending_.append(piece);
  const std::string_view open_tag(kOpenTag);
  const std::string_view close_tag(kCloseTag);
  while (!pending_.empty()) {
    if (in_think_) {
      const size_t pos = pending_.find(close_tag);
      if (pos != std::string::npos) {
        pending_.erase(0, pos + close_tag.size());
        in_think_ = false;
        strip_leading_ws_ = true;
        continue;
      }
      // Hidden: drop everything except a possible partial "</think>".
      const size_t hold = PartialTagSuffix(pending_, close_tag);
      pending_.erase(0, pending_.size() - hold);
      break;
    }

    // Tags only act as reasoning delimiters before the answer has started;
    // a model that later writes "<think>" in prose or code just shows it.
    if (!visible_started_) {
      const size_t open_pos = pending_.find(open_tag);
      const size_t close_pos = pending_.find(close_tag);
      const size_t first = std::min(open_pos, close_pos);
      if (first != std::string::npos) {
        std::string before = EmitVisible(std::string_view(pending_).substr(
            0, first));
        if (visible_started_) {
          // Real text preceded the tag: from here on tags are plain text.
          out += before;
          pending_.erase(0, first);
          continue;
        }
        out += before;  // whitespace only
        if (first == open_pos) {
          in_think_ = true;
          saw_think_ = true;
          pending_.erase(0, first + open_tag.size());
        } else {
          // Orphan "</think>" before any answer text: the block was opened
          // by the prompt. Drop the tag and the whitespace after it.
          pending_.erase(0, first + close_tag.size());
          strip_leading_ws_ = true;
        }
        continue;
      }
      const size_t hold = std::max(PartialTagSuffix(pending_, open_tag),
                                   PartialTagSuffix(pending_, close_tag));
      out += EmitVisible(
          std::string_view(pending_).substr(0, pending_.size() - hold));
      pending_.erase(0, pending_.size() - hold);
      // If visible text just started, the held-back bytes can no longer be
      // a reasoning tag; loop once more to release them as text.
      if (visible_started_ && hold > 0) continue;
      break;
    }

    out += EmitVisible(pending_);
    pending_.clear();
  }
  return out;
}

std::string ThinkTagFilter::Flush() {
  if (in_think_) {
    pending_.clear();
    return std::string();
  }
  std::string out = EmitVisible(pending_);
  pending_.clear();
  return out;
}

// ---- StopSequenceFilter ---------------------------------------------------

StopSequenceFilter::StopSequenceFilter(std::vector<std::string> stop_sequences) {
  for (std::string& text : stop_sequences) {
    if (text.empty()) continue;
    Stop stop;
    // failure[i] = length of the longest proper prefix of text[0..i] that is
    // also a suffix of it.
    stop.failure.assign(text.size(), 0);
    size_t k = 0;
    for (size_t i = 1; i < text.size(); ++i) {
      while (k > 0 && text[i] != text[k]) k = stop.failure[k - 1];
      if (text[i] == text[k]) ++k;
      stop.failure[i] = k;
    }
    stop.text = std::move(text);
    stops_.push_back(std::move(stop));
  }
}

StopSequenceFilter::~StopSequenceFilter() = default;
StopSequenceFilter::StopSequenceFilter(StopSequenceFilter&&) = default;
StopSequenceFilter& StopSequenceFilter::operator=(StopSequenceFilter&&) =
    default;

std::string StopSequenceFilter::Feed(std::string_view text) {
  std::string out;
  if (stopped_) return out;
  if (stops_.empty()) {
    out.assign(text);
    return out;
  }
  // pending_ holds every byte not released yet. A stop string completing at
  // pending_[i] starts inside pending_: its first (length - 1) bytes were a
  // partial match, and partial matches are never released.
  const size_t begin = pending_.size();
  pending_.append(text);
  for (size_t i = begin; i < pending_.size(); ++i) {
    const char c = pending_[i];
    for (Stop& stop : stops_) {
      size_t k = stop.matched;
      while (k > 0 && stop.text[k] != c) k = stop.failure[k - 1];
      if (stop.text[k] == c) ++k;
      if (k == stop.text.size()) {
        out.append(pending_, 0, i + 1 - k);
        pending_.clear();
        stopped_ = true;
        return out;
      }
      stop.matched = k;
    }
  }
  size_t hold = 0;
  for (const Stop& stop : stops_) hold = std::max(hold, stop.matched);
  hold = std::min(hold, pending_.size());
  out.append(pending_, 0, pending_.size() - hold);
  pending_.erase(0, pending_.size() - hold);
  return out;
}

std::string StopSequenceFilter::Flush() {
  std::string out;
  if (!stopped_) out.swap(pending_);
  pending_.clear();
  for (Stop& stop : stops_) stop.matched = 0;
  return out;
}

// ---- UTF-8 ----------------------------------------------------------------

namespace {

// Byte ranges of a well-formed UTF-8 sequence starting with |lead| (Table 3-7
// of the Unicode Standard). |length| is 0 for a byte that can't start one
// (continuation byte, C0/C1 overlong lead, F5..FF).
struct SequenceShape {
  size_t length = 0;
  unsigned char second_min = 0x80;
  unsigned char second_max = 0xBF;
};

SequenceShape ShapeOf(unsigned char lead) {
  SequenceShape shape;
  if (lead < 0x80) {
    shape.length = 1;
  } else if (lead >= 0xC2 && lead <= 0xDF) {
    shape.length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    shape.length = 3;
    if (lead == 0xE0) shape.second_min = 0xA0;  // no overlong 3-byte forms
    if (lead == 0xED) shape.second_max = 0x9F;  // no surrogates
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    shape.length = 4;
    if (lead == 0xF0) shape.second_min = 0x90;  // no overlong 4-byte forms
    if (lead == 0xF4) shape.second_max = 0x8F;  // nothing above U+10FFFF
  }
  return shape;
}

// How many bytes at s[i] belong to a (possibly incomplete) well-formed
// sequence: the full length if it is complete, otherwise the length of its
// maximal subpart (at least 1 for a lead byte that can start a sequence, 0
// for one that can't).
size_t MatchedPrefix(std::string_view s, size_t i, const SequenceShape& shape) {
  if (shape.length == 0) return 0;
  size_t n = 1;
  while (n < shape.length && i + n < s.size()) {
    const auto b = static_cast<unsigned char>(s[i + n]);
    const unsigned char lo = n == 1 ? shape.second_min : 0x80;
    const unsigned char hi = n == 1 ? shape.second_max : 0xBF;
    if (b < lo || b > hi) break;
    ++n;
  }
  return n;
}

constexpr char kReplacementCharacter[] = "\xEF\xBF\xBD";

}  // namespace

bool IsValidUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const SequenceShape shape = ShapeOf(static_cast<unsigned char>(s[i]));
    const size_t n = MatchedPrefix(s, i, shape);
    if (n == 0 || n != shape.length) return false;
    i += n;
  }
  return true;
}

std::string ReplaceInvalidUtf8(std::string_view s) {
  std::string out;
  size_t i = 0;
  size_t copied = 0;  // s[copied, i) is valid and not yet in |out|
  bool replaced = false;
  while (i < s.size()) {
    const SequenceShape shape = ShapeOf(static_cast<unsigned char>(s[i]));
    const size_t n = MatchedPrefix(s, i, shape);
    if (n != 0 && n == shape.length) {
      i += n;
      continue;
    }
    if (!replaced) out.reserve(s.size() + 8);
    replaced = true;
    out.append(s.substr(copied, i - copied));
    out += kReplacementCharacter;
    i += n == 0 ? 1 : n;  // one U+FFFD per maximal subpart
    copied = i;
  }
  if (!replaced) return std::string(s);
  out.append(s.substr(copied));
  return out;
}

std::string ReplaceNul(std::string_view s) {
  if (s.find('\0') == std::string_view::npos) return std::string(s);
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\0') {
      out += kReplacementCharacter;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string ToDisplayText(std::string_view s) {
  return ReplaceNul(ReplaceInvalidUtf8(s));
}

// ---- Utf8Assembler --------------------------------------------------------

size_t Utf8SafePrefixLength(std::string_view s) {
  if (s.empty()) return 0;
  size_t i = s.size();
  while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
    --i;  // skip UTF-8 continuation bytes
  }
  if (i == 0) return s.size();  // only continuation bytes: nothing to wait for
  const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
  const size_t need = (lead < 0x80)             ? 1
                      : ((lead & 0xE0) == 0xC0) ? 2
                      : ((lead & 0xF0) == 0xE0) ? 3
                      : ((lead & 0xF8) == 0xF0) ? 4
                                                : 1;
  const size_t have = s.size() - (i - 1);
  return have >= need ? s.size() : (i - 1);  // hold back an incomplete char
}

std::string Utf8Assembler::Push(std::string_view bytes) {
  pending_.append(bytes);
  const size_t safe = Utf8SafePrefixLength(pending_);
  std::string out = pending_.substr(0, safe);
  pending_.erase(0, safe);
  return out;
}

std::string Utf8Assembler::Flush() {
  std::string out;
  out.swap(pending_);
  return out;
}

}  // namespace molt_ai
