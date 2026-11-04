// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_speech_text.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"

namespace molt_ai {
namespace voice {

namespace {

// At most this many fields are read with a candidate at full verbosity.
constexpr size_t kMaxFieldsRead = 6;
// Counts up to this are said in words.
constexpr int64_t kMaxScaledCount = 1000000000000000;

std::string Unit(int64_t n, std::string_view one, std::string_view many) {
  std::string out = std::to_string(n);
  out.push_back(' ');
  out.append(n == 1 ? one : many);
  return out;
}

// |value| / |scale| with one decimal when it has one: "2.1", "312".
std::string Scaled(int64_t value, int64_t scale) {
  const int64_t tenths = (value * 10 + scale / 2) / scale;
  std::string out = std::to_string(tenths / 10);
  if (tenths % 10 != 0) {
    out.push_back('.');
    out.push_back(static_cast<char>('0' + tenths % 10));
  }
  return out;
}

const ChoiceField* FieldWith(const ChoiceCandidate& candidate,
                             ChoiceFieldKind kind) {
  for (const ChoiceField& field : candidate.fields) {
    if (field.kind == kind && !field.text.empty()) {
      return &field;
    }
  }
  return nullptr;
}

const std::string* FieldOf(const ChoiceCandidate& candidate,
                           ChoiceFieldKind kind) {
  const ChoiceField* field = FieldWith(candidate, kind);
  return field ? &field->text : nullptr;
}

// A duration read by the page map is at most this long (ParseDuration()'s
// own limit): anything else is read again.
constexpr int64_t kMaxReadSeconds = 1000 * 3600;

// The units of time a length is said in.
struct TimeWords {
  std::string_view second, seconds, minute, minutes, hour, hours;
};

const TimeWords& TimeWordsFor(Language language) {
  static constexpr TimeWords kEnglish = {"second",  "seconds", "minute",
                                         "minutes", "hour",    "hours"};
  static constexpr TimeWords kSpanish = {"segundo", "segundos", "minuto",
                                         "minutos", "hora",     "horas"};
  return language == Language::kSpanish ? kSpanish : kEnglish;
}

void AppendPart(std::string& out, std::string_view part) {
  if (part.empty()) {
    return;
  }
  if (!out.empty()) {
    out.append(", ");
  }
  out.append(part);
}

}  // namespace

std::string SpokenDuration(int seconds) {
  return SpokenDuration(seconds, Language::kEnglish);
}

std::string SpokenLength(int seconds) {
  return SpokenLength(seconds, Language::kEnglish);
}

std::string SpokenDuration(int seconds, Language language) {
  const TimeWords& w = TimeWordsFor(language);
  if (seconds < 0) {
    return std::string();
  }
  if (seconds < 60) {
    return Unit(seconds, w.second, w.seconds);
  }
  const int minutes = (seconds + 30) / 60;
  if (minutes < 60) {
    return Unit(minutes, w.minute, w.minutes);
  }
  std::string out = Unit(minutes / 60, w.hour, w.hours);
  if (minutes % 60 != 0) {
    out.push_back(' ');
    out.append(Unit(minutes % 60, w.minute, w.minutes));
  }
  return out;
}

std::string SpokenLength(int seconds, Language language) {
  const TimeWords& w = TimeWordsFor(language);
  if (seconds < 0) {
    seconds = -seconds;
  }
  const int hours = seconds / 3600;
  const int minutes = seconds / 60 % 60;
  const int rest = seconds % 60;
  std::string out;
  if (hours > 0) {
    out = Unit(hours, w.hour, w.hours);
  }
  if (minutes > 0) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(Unit(minutes, w.minute, w.minutes));
  }
  if (rest > 0 || out.empty()) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(Unit(rest, w.second, w.seconds));
  }
  return out;
}

std::string SpokenCount(std::string_view text) {
  return SpokenCount(text, Language::kEnglish);
}

std::string SpokenCount(std::string_view text, Language language) {
  return SpokenCountOf(text, -1, std::string_view(), language);
}

std::string SpokenCountOf(std::string_view text,
                          int64_t value,
                          std::string_view unit,
                          Language language) {
  const std::string cleaned = PageText(text);
  // Read as the page map read it (in the page's language), else again.
  std::optional<ParsedCount> count;
  if (value >= 0) {
    count = ParsedCount{value, std::string(unit)};
  } else {
    count = ParseCount(cleaned);
  }
  // Past a thousand trillion it is read as shown (and Scaled() can't
  // overflow).
  if (!count || count->value <= 0 || count->value > kMaxScaledCount ||
      count->unit.empty()) {
    return cleaned;
  }
  const int64_t v = count->value;
  if (language == Language::kSpanish) {
    // A decimal comma, "mil" for thousands, and "de" after "millones":
    // "1,2 millones de visualizaciones", "15 mil suscriptores".
    auto comma = [](std::string number) {
      std::replace(number.begin(), number.end(), '.', ',');
      return number;
    };
    std::string number;
    bool of = false;
    if (v >= 1000000000) {
      number = comma(Scaled(v, 1000000000)) + " mil millones";
      of = true;
    } else if (v >= 1000000) {
      const std::string scaled = comma(Scaled(v, 1000000));
      number = scaled + (scaled == "1" ? " millón" : " millones");
      of = true;
    } else if (v >= 10000) {
      number = comma(Scaled(v, 1000)) + " mil";
    } else {
      number = std::to_string(v);
    }
    return number + (of ? " de " : " ") + count->unit;
  }
  std::string number;
  if (v >= 1000000000) {
    number = Scaled(v, 1000000000) + " billion";
  } else if (v >= 1000000) {
    number = Scaled(v, 1000000) + " million";
  } else if (v >= 10000) {
    number = Scaled(v, 1000) + " thousand";
  } else {
    number = std::to_string(v);
  }
  return number + " " + count->unit;
}

std::string CountOf(size_t n, std::string_view one, std::string_view many) {
  return Unit(static_cast<int64_t>(n), one, many);
}

int SpokenNumber(const ChoiceCandidate& candidate) {
  return candidate.screen_number > 0 ? candidate.screen_number
                                     : candidate.position;
}

std::string PageText(std::string_view text) {
  return SpeechSafeText(text, kMaxSpokenPageTextChars);
}

std::string SpokenCandidate(const ChoiceCandidate& candidate,
                            Verbosity verbosity) {
  return SpokenCandidate(candidate, verbosity, Language::kEnglish);
}

std::string SpokenCandidate(const ChoiceCandidate& candidate,
                            Verbosity verbosity,
                            Language language) {
  std::string out = PageText(candidate.title);
  if (const std::string* author =
          FieldOf(candidate, ChoiceFieldKind::kAuthor)) {
    AppendPart(out, PageText(*author));
  } else if (const std::string* source =
                 FieldOf(candidate, ChoiceFieldKind::kSource)) {
    AppendPart(out, PageText(*source));
  }
  if (const ChoiceField* duration =
          FieldWith(candidate, ChoiceFieldKind::kDuration)) {
    // As the page map read it (in the page's language), else again.
    std::optional<int> seconds =
        duration->value >= 0 && duration->value <= kMaxReadSeconds
            ? std::optional<int>(static_cast<int>(duration->value))
            : ParseDuration(duration->text);
    AppendPart(out, seconds ? SpokenDuration(*seconds, language)
                            : PageText(duration->text));
  }
  if (verbosity == Verbosity::kFull) {
    size_t read = 0;
    for (const ChoiceField& field : candidate.fields) {
      if (read >= kMaxFieldsRead) {
        break;
      }
      switch (field.kind) {
        case ChoiceFieldKind::kDate:
        case ChoiceFieldKind::kPrice:
        case ChoiceFieldKind::kDetail:
          AppendPart(out, PageText(field.text));
          ++read;
          break;
        case ChoiceFieldKind::kCount:
          AppendPart(out, SpokenCountOf(field.text, field.value, field.unit,
                                        language));
          ++read;
          break;
        case ChoiceFieldKind::kAuthor:
        case ChoiceFieldKind::kSource:
        case ChoiceFieldKind::kDuration:
          break;
      }
    }
  }
  if (out.empty()) {
    out = Localized(language, "untitled");
  }
  return out;
}

std::string_view ChooseHint() {
  return ChooseHint(Language::kEnglish);
}

std::string_view ChooseHint(Language language) {
  switch (language) {
    case Language::kEnglish:
      break;
    case Language::kSpanish:
      return "Di un número o un nombre.";
  }
  return "Say a number or a name.";
}

std::vector<std::string> ChunkSentences(
    const std::vector<std::string>& sentences) {
  std::vector<std::string> chunks;
  std::string current;
  size_t current_chars = 0;
  for (const std::string& raw : sentences) {
    std::string sentence = SpeechSafeText(raw, kMaxSpeechChunkChars);
    if (sentence.empty()) {
      continue;
    }
    const size_t chars = CountCodePoints(sentence);
    if (!current.empty() && current_chars + 1 + chars > kMaxSpeechChunkChars) {
      chunks.push_back(std::move(current));
      current.clear();
      current_chars = 0;
    }
    if (!current.empty()) {
      current.push_back(' ');
      ++current_chars;
    }
    current.append(sentence);
    current_chars += chars;
  }
  if (!current.empty()) {
    chunks.push_back(std::move(current));
  }
  return chunks;
}

std::vector<std::string> ListSpeech(const CandidateSet& set,
                                    std::string_view one,
                                    std::string_view many,
                                    Verbosity verbosity,
                                    size_t normal_count) {
  return ListSpeech(set, one, many, verbosity, normal_count,
                    Language::kEnglish);
}

std::vector<std::string> ListSpeech(const CandidateSet& set,
                                    std::string_view one,
                                    std::string_view many,
                                    Verbosity verbosity,
                                    size_t normal_count,
                                    Language language) {
  std::vector<std::string> sentences;
  sentences.push_back(CountOf(set.size(), one, many) + ".");
  size_t read = 0;
  switch (verbosity) {
    case Verbosity::kBrief:
      read = 0;
      break;
    case Verbosity::kFull:
      read = std::min(set.size(), kFullListRead);
      break;
    case Verbosity::kNormal:
    case Verbosity::kMore:
    case Verbosity::kLess:
      read = std::min(set.size(), normal_count);
      break;
  }
  for (size_t i = 0; i < read; ++i) {
    const ChoiceCandidate& candidate = set.candidates[i];
    sentences.push_back(std::to_string(SpokenNumber(candidate)) + ": " +
                        SpokenCandidate(candidate, verbosity, language) + ".");
  }
  if (!set.empty()) {
    sentences.emplace_back(ChooseHint(language));
  }
  return ChunkSentences(sentences);
}

std::vector<std::string> OfferSpeech(const CandidateSet& subset) {
  return OfferSpeech(subset, Language::kEnglish);
}

std::vector<std::string> OfferSpeech(const CandidateSet& subset,
                                     Language language) {
  const bool spanish = language == Language::kSpanish;
  std::vector<std::string> sentences;
  for (size_t i = 0; i < subset.size(); ++i) {
    std::string sentence;
    if (i == 0) {
      sentence = spanish ? "¿Te refieres a " : "Did you mean ";
    } else if (i + 1 == subset.size()) {
      sentence = spanish ? "o " : "or ";
    }
    sentence.append(std::to_string(SpokenNumber(subset.candidates[i])));
    sentence.append(": ");
    sentence.append(
        SpokenCandidate(subset.candidates[i], Verbosity::kNormal, language));
    sentence.append(i + 1 == subset.size() ? "?" : ",");
    sentences.push_back(std::move(sentence));
  }
  sentences.emplace_back(ChooseHint(language));
  return ChunkSentences(sentences);
}

}  // namespace voice
}  // namespace molt_ai
