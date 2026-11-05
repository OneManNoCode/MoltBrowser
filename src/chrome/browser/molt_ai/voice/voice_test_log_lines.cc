// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_test_log_lines.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"

namespace molt_ai {
namespace voice {

namespace {

// A folded word shorter or longer than these can't be a confirmation word
// (the lists' words have 4 to 10 letters; a plural or a word split in two
// adds a few).
constexpr size_t kMinMaskedWordBytes = 3;
constexpr size_t kMaxMaskedWordBytes = 24;

// A word this long or longer is also masked when heard one letter off
// ("harbour", "harber"); shorter ones would take too many other words.
constexpr size_t kMinNearMissWordBytes = 6;

// Enough for every word of a capped text.
constexpr size_t kMaxMaskedWords = kMaxTestLogTextBytes / 2 + 1;

// Whether |a| and |b| differ by at most one letter added, dropped or
// changed.
bool OneEditApart(std::string_view a, std::string_view b) {
  if (a.size() < b.size()) {
    std::swap(a, b);
  }
  if (a.size() - b.size() > 1) {
    return false;
  }
  size_t i = 0;
  while (i < b.size() && a[i] == b[i]) {
    ++i;
  }
  if (i == b.size()) {
    return true;  // equal, or one letter added at the end
  }
  // Skip one letter of |a| (added), or of both (changed).
  return a.substr(i + 1) == b.substr(i + (a.size() == b.size() ? 1 : 0));
}

// Whether the folded word |heard| is |word| as the log masks it: the word,
// its plural, or (long words) one letter off.
bool IsWord(std::string_view heard, std::string_view word) {
  if (word.empty() || heard.size() < word.size()) {
    return word.size() >= kMinNearMissWordBytes &&
           heard.size() + 1 == word.size() && OneEditApart(heard, word);
  }
  if (heard == word) {
    return true;
  }
  if (heard.substr(0, word.size()) == word) {
    const std::string_view rest = heard.substr(word.size());
    if (rest == "s" || rest == "es") {
      return true;
    }
  }
  return word.size() >= kMinNearMissWordBytes && OneEditApart(heard, word);
}

// Whether the folded word |heard| is any confirmation word, or
// |pending_word|.
bool IsAnyWord(std::string_view heard, std::string_view pending_word) {
  if (heard.size() < kMinMaskedWordBytes ||
      heard.size() > kMaxMaskedWordBytes) {
    return false;
  }
  if (!pending_word.empty() && IsWord(heard, FoldForComparison(pending_word))) {
    return true;
  }
  for (Language language : {Language::kEnglish, Language::kSpanish}) {
    for (std::string_view word : ConfirmationWords(language)) {
      if (IsWord(heard, word)) {
        return true;
      }
    }
  }
  return false;
}

void AppendQuoted(std::string& out, std::string_view text) {
  out.push_back('"');
  out.append(TestLogJsonEscape(text));
  out.push_back('"');
}

void AppendBool(std::string& out, std::string_view key, bool value) {
  out.append(",\"");
  out.append(key);
  out.append("\":");
  out.append(value ? "true" : "false");
}

void AppendString(std::string& out,
                  std::string_view key,
                  std::string_view value) {
  out.append(",\"");
  out.append(key);
  out.append("\":");
  AppendQuoted(out, value);
}

}  // namespace

std::string MaskConfirmationWords(std::string_view text,
                                  std::string_view pending_word) {
  const std::string clean = CleanSpokenText(text, kMaxTestLogTextBytes);
  NormalizeOptions options;
  options.max_words = kMaxMaskedWords;
  const NormalizedText normalized = NormalizeSpokenText(clean, options);
  // The source byte ranges to mask, in order.
  std::vector<std::pair<size_t, size_t>> masked;
  const size_t count = normalized.words.size();
  std::vector<bool> is_word(count, false);
  for (size_t i = 0; i < count; ++i) {
    is_word[i] = IsAnyWord(normalized.Word(i), pending_word);
  }
  for (size_t i = 0; i < count; ++i) {
    if (is_word[i]) {
      masked.emplace_back(normalized.words[i].source_begin,
                          normalized.words[i].source_end);
      continue;
    }
    // A word heard split in two ("har bor"), neither half one on its own.
    if (i + 1 < count && !is_word[i + 1] &&
        IsAnyWord(std::string(normalized.Word(i)) +
                      std::string(normalized.Word(i + 1)),
                  pending_word)) {
      masked.emplace_back(normalized.words[i].source_begin,
                          normalized.words[i + 1].source_end);
      ++i;
    }
  }
  // Words past what was read (none for a capped text) are dropped rather
  // than written unmasked.
  const size_t limit =
      !normalized.truncated
          ? clean.size()
          : (normalized.words.empty() ? 0 : normalized.words.back().source_end);
  std::string out;
  out.reserve(limit);
  size_t at = 0;
  for (const auto& [begin, end] : masked) {
    if (begin < at || end > limit) {
      continue;
    }
    out.append(clean, at, begin - at);
    out.append(kTestLogMaskedWord);
    at = end;
  }
  out.append(clean, at, limit - at);
  return out;
}

std::string TestLogJsonEscape(std::string_view text) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (u < 0x20 || u == 0x7f) {
          out.append("\\u00");
          out.push_back(kHex[u >> 4]);
          out.push_back(kHex[u & 0xf]);
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

TestLogLines::TestLogLines(int window) : window_(window) {}

TestLogLines::~TestLogLines() = default;

std::string TestLogLines::Head(int64_t ms, std::string_view event) const {
  std::string out = "{\"t\":";
  out.append(std::to_string(ms));
  out.append(",\"w\":");
  out.append(std::to_string(window_));
  out.append(",\"e\":\"");
  out.append(event);
  out.push_back('"');
  return out;
}

std::string TestLogLines::Start(int64_t ms,
                                std::span<const std::string_view> kinds) const {
  std::string out = Head(ms, "start");
  out.append(",\"kinds\":[");
  bool first = true;
  for (std::string_view kind : kinds) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    AppendQuoted(out, CleanSpokenText(kind, kMaxTestLogTextBytes));
  }
  out.append("]}\n");
  return out;
}

std::string TestLogLines::Heard(int64_t ms,
                                std::string_view text,
                                bool from_microphone,
                                bool clean,
                                std::string_view intent,
                                std::string_view pending_word) const {
  std::string out = Head(ms, "heard");
  AppendString(out, "text", MaskConfirmationWords(text, pending_word));
  AppendString(out, "source", from_microphone ? "mic" : "typed");
  AppendBool(out, "clean", clean);
  AppendString(out, "intent", CleanSpokenText(intent, 64));
  out.append("}\n");
  return out;
}

std::string TestLogLines::Said(int64_t ms, std::string_view text) const {
  std::string out = Head(ms, "said");
  AppendString(out, "text", MaskConfirmationWords(text, std::string_view()));
  out.append("}\n");
  return out;
}

std::string TestLogLines::Map(int64_t ms,
                              const MapSummary& map,
                              std::string_view pending_word) const {
  std::string out = Head(ms, "map");
  out.append(",\"items\":");
  out.append(std::to_string(map.items));
  out.append(",\"lists\":");
  out.append(std::to_string(map.lists));
  out.append(",\"media\":");
  out.append(std::to_string(map.media));
  AppendBool(out, "selection", map.selection);
  AppendBool(out, "selected_text", map.selected_text);
  out.append(",\"labels\":[");
  for (size_t i = 0; i < map.labels.size() && i < kMaxTestLogLabels; ++i) {
    if (i) {
      out.push_back(',');
    }
    AppendQuoted(out,
                 MaskConfirmationWords(
                     CleanSpokenText(map.labels[i], kMaxTestLogLabelBytes),
                     pending_word));
  }
  out.append("]}\n");
  return out;
}

std::string TestLogLines::Microphone(int64_t ms,
                                     float peak,
                                     int64_t samples,
                                     int64_t dropped,
                                     std::string_view echo) const {
  std::string out = Head(ms, "mic");
  const int db = peak > 1e-6f && std::isfinite(peak)
                     ? static_cast<int>(
                           std::lround(20.0 * std::log10(std::min(peak, 1.0f))))
                     : -120;
  out.append(",\"peak_db\":");
  out.append(std::to_string(db));
  out.append(",\"samples\":");
  out.append(std::to_string(std::max<int64_t>(samples, 0)));
  out.append(",\"dropped\":");
  out.append(std::to_string(std::max<int64_t>(dropped, 0)));
  AppendString(out, "echo", CleanSpokenText(echo, 32));
  out.append("}\n");
  return out;
}

std::string TestLogLines::Dropped(int64_t ms,
                                  std::string_view text,
                                  std::string_view reason,
                                  std::string_view pending_word) const {
  std::string out = Head(ms, "dropped");
  AppendString(out, "text", MaskConfirmationWords(text, pending_word));
  AppendString(out, "reason", CleanSpokenText(reason, 64));
  out.append("}\n");
  return out;
}

std::string TestLogLines::Changed(int64_t ms, const TestLogState& now) {
  std::string out;
  if (now.activity != activity_ || now.mic != mic_ || now.waiting != waiting_) {
    activity_ = std::string(now.activity);
    mic_ = now.mic;
    waiting_ = now.waiting;
    out.append(Head(ms, "activity"));
    AppendString(out, "activity", CleanSpokenText(now.activity, 32));
    AppendBool(out, "mic", now.mic);
    AppendBool(out, "waiting", now.waiting);
    out.append("}\n");
  }
  if (now.prompt_id != prompt_id_) {
    if (prompt_id_ != 0) {
      out.append(Head(ms, "prompt"));
      out.append(",\"op\":\"close\",\"id\":");
      out.append(std::to_string(prompt_id_));
      out.append("}\n");
    }
    if (now.prompt_id != 0) {
      out.append(Head(ms, "prompt"));
      out.append(",\"op\":\"open\",\"id\":");
      out.append(std::to_string(now.prompt_id));
      AppendString(out, "kind", CleanSpokenText(now.prompt_kind, 32));
      out.append("}\n");
    }
    prompt_id_ = now.prompt_id;
  }
  if (now.confirming != confirming_) {
    confirming_ = now.confirming;
    out.append(Head(ms, "confirm"));
    if (now.confirming) {
      out.append(",\"op\":\"open\"");
      AppendString(out, "what", MaskConfirmationWords(now.confirm_what, {}));
    } else {
      out.append(",\"op\":\"close\"");
    }
    out.append("}\n");
  }
  return out;
}

}  // namespace voice
}  // namespace molt_ai
