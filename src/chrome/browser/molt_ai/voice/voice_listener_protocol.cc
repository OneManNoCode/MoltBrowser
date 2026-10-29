// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_listener_protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

namespace {

// ---- Frames ----------------------------------------------------------------

void AppendUint32(uint32_t value, std::string* out) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

// ---- Phrases ---------------------------------------------------------------

constexpr std::array<std::string_view, 20> kOnes = {
    "zero",    "one",     "two",       "three",    "four",
    "five",    "six",     "seven",     "eight",    "nine",
    "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
constexpr std::array<std::string_view, 10> kTens = {
    "",      "",      "twenty",  "thirty", "forty",
    "fifty", "sixty", "seventy", "eighty", "ninety"};

void AppendWord(std::string_view word, std::string* out) {
  if (!out->empty()) {
    out->push_back(' ');
  }
  out->append(word);
}

// 0 <= n < 1000.
void AppendBelowThousand(uint32_t n, std::string* out) {
  if (n >= 100) {
    AppendWord(kOnes[n / 100], out);
    AppendWord("hundred", out);
    n %= 100;
    if (n == 0) {
      return;
    }
  }
  if (n < 20) {
    AppendWord(kOnes[n], out);
    return;
  }
  AppendWord(kTens[n / 10], out);
  if (n % 10 != 0) {
    AppendWord(kOnes[n % 10], out);
  }
}

// 0 <= n < 1000000000.
std::string CardinalWords(uint32_t n) {
  std::string out;
  if (n == 0) {
    return std::string(kOnes[0]);
  }
  if (n >= 1000000) {
    AppendBelowThousand(n / 1000000, &out);
    AppendWord("million", &out);
    n %= 1000000;
  }
  if (n >= 1000) {
    AppendBelowThousand(n / 1000, &out);
    AppendWord("thousand", &out);
    n %= 1000;
  }
  if (n > 0) {
    AppendBelowThousand(n, &out);
  }
  return out;
}

// The last word of |words| as an ordinal ("twenty four" -> "twenty
// fourth").
std::string MakeOrdinal(std::string words) {
  const size_t space = words.rfind(' ');
  const size_t start = space == std::string::npos ? 0 : space + 1;
  const std::string last = words.substr(start);
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 6>
      kIrregular = {{{"one", "first"},
                     {"two", "second"},
                     {"three", "third"},
                     {"five", "fifth"},
                     {"eight", "eighth"},
                     {"nine", "ninth"}}};
  std::string ordinal;
  for (const auto& [cardinal, irregular] : kIrregular) {
    if (last == cardinal) {
      ordinal = std::string(irregular);
    }
  }
  if (ordinal.empty()) {
    if (last == "twelve") {
      ordinal = "twelfth";
    } else if (last.ends_with('y')) {
      ordinal = last.substr(0, last.size() - 1) + "ieth";
    } else {
      ordinal = last + "th";
    }
  }
  return words.substr(0, start) + ordinal;
}

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

bool IsAsciiLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char ToLowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool AllDigits(std::string_view word) {
  return !word.empty() && std::all_of(word.begin(), word.end(), IsAsciiDigit);
}

// "4th", "21st", "2nd", "3rd": the number's words as an ordinal; empty
// otherwise.
std::string OrdinalAsWords(std::string_view word) {
  if (word.size() < 3) {
    return std::string();
  }
  const std::string_view suffix = word.substr(word.size() - 2);
  if (suffix != "st" && suffix != "nd" && suffix != "rd" && suffix != "th") {
    return std::string();
  }
  const std::string cardinal = NumberAsWords(word.substr(0, word.size() - 2));
  return cardinal.empty() ? std::string() : MakeOrdinal(cardinal);
}

// The source bytes [begin, end) as a word with its apostrophes, if it is
// plain ASCII letters, digits and apostrophes (’ counts as one): lower
// case; nullopt otherwise.
std::optional<std::string> AsciiWordWithApostrophes(std::string_view source) {
  std::string out;
  size_t i = 0;
  while (i < source.size()) {
    const char c = source[i];
    if (IsAsciiLetter(c) || IsAsciiDigit(c)) {
      out.push_back(c);
      ++i;
    } else if (c == '\'') {
      out.push_back('\'');
      ++i;
    } else if (source.substr(i, 3) == "\xE2\x80\x99") {  // U+2019
      out.push_back('\'');
      i += 3;
    } else {
      return std::nullopt;
    }
  }
  // Apostrophes only inside the word.
  while (!out.empty() && out.front() == '\'') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == '\'') {
    out.pop_back();
  }
  if (out.find("''") != std::string::npos) {
    return std::nullopt;
  }
  return out;
}

// Whether |word| holds only what a phrase may: ASCII letters, digits and
// apostrophes.
bool IsPhraseWord(std::string_view word) {
  return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
    return IsAsciiLetter(c) || IsAsciiDigit(c) || c == '\'';
  });
}

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = ToLowerAscii(c);
  }
  return out;
}

// Appends |words| (space-separated) to |phrase| while it stays within the
// caps; false once it is full.
bool AppendPhraseWords(std::string_view words,
                       std::string* phrase,
                       size_t* count) {
  size_t start = 0;
  while (start < words.size()) {
    size_t end = words.find(' ', start);
    if (end == std::string_view::npos) {
      end = words.size();
    }
    const std::string_view word = words.substr(start, end - start);
    start = end + 1;
    if (word.empty()) {
      continue;
    }
    const size_t added = word.size() + (phrase->empty() ? 0 : 1);
    if (*count >= kMaxPhraseWords || phrase->size() + added > kMaxPhraseBytes) {
      return false;
    }
    AppendWord(word, phrase);
    ++*count;
  }
  return true;
}

// Splits a title at the separators that set its parts apart.
std::vector<std::string_view> SplitTitle(std::string_view title) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  auto cut = [&](size_t at, size_t skip) {
    if (at > start) {
      parts.push_back(title.substr(start, at - start));
    }
    start = at + skip;
  };
  size_t i = 0;
  while (i < title.size()) {
    const char c = title[i];
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '!' || c == ',' ||
        c == '?' || c == '|' || c == '/') {
      cut(i, 1);
      ++i;
      continue;
    }
    // " - ", " – ", " — ", ": ".
    const bool space_before = i > 0 && title[i - 1] == ' ';
    if (c == '-' && space_before && i + 1 < title.size() &&
        title[i + 1] == ' ') {
      cut(i, 1);
      ++i;
      continue;
    }
    if ((title.substr(i, 3) == "\xE2\x80\x93" ||
         title.substr(i, 3) == "\xE2\x80\x94") &&
        space_before) {
      cut(i, 3);
      i += 3;
      continue;
    }
    if (c == ':' && i + 1 < title.size() && title[i + 1] == ' ') {
      cut(i, 1);
      ++i;
      continue;
    }
    ++i;
  }
  if (start < title.size()) {
    parts.push_back(title.substr(start));
  }
  return parts;
}

// ---- JSON (writing) --------------------------------------------------------

void AppendJsonString(std::string_view text, std::string* out) {
  out->push_back('"');
  for (const char c : text) {
    const auto byte = static_cast<uint8_t>(c);
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(c);
    } else if (byte < 0x20) {
      constexpr std::string_view kHex = "0123456789abcdef";
      out->append("\\u00");
      out->push_back(kHex[byte >> 4]);
      out->push_back(kHex[byte & 0xf]);
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
}

void AppendJsonStrings(const std::vector<std::string>& values,
                       size_t max,
                       std::string* out) {
  out->push_back('[');
  size_t written = 0;
  for (const std::string& value : values) {
    if (written == max) {
      break;
    }
    const std::string reduced = ReduceSpeechPhrase(value);
    if (reduced.empty()) {
      continue;
    }
    if (written > 0) {
      out->push_back(',');
    }
    AppendJsonString(reduced, out);
    ++written;
  }
  out->push_back(']');
}

// ---- JSON (reading) --------------------------------------------------------

// A value of the kinds a helper line may hold.
struct JsonValue {
  enum class Kind { kString, kNumber, kBool, kNull, kStrings };
  Kind kind = Kind::kNull;
  std::string string;
  double number = 0;
  bool boolean = false;
  std::vector<std::string> strings;
};

// A strict, linear reader of one flat JSON object.
class LineParser {
 public:
  explicit LineParser(std::string_view text) : text_(text) {}

  // The object's members, in order; nullopt (with |error_|) if the text
  // isn't one flat object.
  std::optional<std::vector<std::pair<std::string, JsonValue>>> ParseObject() {
    SkipSpace();
    if (!Consume('{')) {
      error_ = Peek() == '[' || Peek() == '"' ? HelperLineError::kNotObject
                                              : HelperLineError::kNotJson;
      return std::nullopt;
    }
    std::vector<std::pair<std::string, JsonValue>> members;
    SkipSpace();
    if (Consume('}')) {
      return Finish(std::move(members));
    }
    for (;;) {
      SkipSpace();
      std::optional<std::string> key = ParseString();
      if (!key) {
        return Fail();
      }
      SkipSpace();
      if (!Consume(':')) {
        return Fail();
      }
      SkipSpace();
      std::optional<JsonValue> value = ParseValue(/*in_array=*/false);
      if (!value) {
        return Fail();
      }
      for (const auto& member : members) {
        if (member.first == *key) {
          error_ = HelperLineError::kDuplicateKey;
          return std::nullopt;
        }
      }
      members.emplace_back(std::move(*key), std::move(*value));
      SkipSpace();
      if (Consume(',')) {
        continue;
      }
      if (Consume('}')) {
        return Finish(std::move(members));
      }
      return Fail();
    }
  }

  HelperLineError error() const { return error_; }

 private:
  std::optional<std::vector<std::pair<std::string, JsonValue>>> Finish(
      std::vector<std::pair<std::string, JsonValue>> members) {
    SkipSpace();
    if (pos_ != text_.size()) {
      return Fail();
    }
    return members;
  }

  std::nullopt_t Fail() {
    if (error_ == HelperLineError::kNone) {
      error_ = HelperLineError::kNotJson;
    }
    return std::nullopt;
  }

  char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

  bool Consume(char c) {
    if (Peek() == c && pos_ < text_.size()) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool ConsumeWord(std::string_view word) {
    if (text_.substr(pos_, word.size()) == word) {
      pos_ += word.size();
      return true;
    }
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' ||
            text_[pos_] == '\n')) {
      ++pos_;
    }
  }

  std::optional<JsonValue> ParseValue(bool in_array) {
    JsonValue value;
    const char c = Peek();
    if (c == '"') {
      std::optional<std::string> string = ParseString();
      if (!string) {
        return std::nullopt;
      }
      value.kind = JsonValue::Kind::kString;
      value.string = std::move(*string);
      return value;
    }
    if (c == '[' && !in_array) {
      ++pos_;
      value.kind = JsonValue::Kind::kStrings;
      SkipSpace();
      if (Consume(']')) {
        return value;
      }
      for (;;) {
        SkipSpace();
        if (Peek() != '"') {
          // Only strings in arrays, no nesting.
          error_ = HelperLineError::kBadField;
          return std::nullopt;
        }
        std::optional<std::string> string = ParseString();
        if (!string) {
          return std::nullopt;
        }
        value.strings.push_back(std::move(*string));
        SkipSpace();
        if (Consume(',')) {
          continue;
        }
        if (Consume(']')) {
          return value;
        }
        return std::nullopt;
      }
    }
    if (c == '{' || c == '[') {
      error_ = HelperLineError::kBadField;
      return std::nullopt;
    }
    if (ConsumeWord("true")) {
      value.kind = JsonValue::Kind::kBool;
      value.boolean = true;
      return value;
    }
    if (ConsumeWord("false")) {
      value.kind = JsonValue::Kind::kBool;
      return value;
    }
    if (ConsumeWord("null")) {
      value.kind = JsonValue::Kind::kNull;
      return value;
    }
    std::optional<double> number = ParseNumber();
    if (!number) {
      return std::nullopt;
    }
    value.kind = JsonValue::Kind::kNumber;
    value.number = *number;
    return value;
  }

  // RFC 8259 numbers. The value only needs to be close (confidences and
  // a protocol version); huge exponents saturate.
  std::optional<double> ParseNumber() {
    const bool negative = Consume('-');
    if (!IsAsciiDigit(Peek())) {
      return std::nullopt;
    }
    double value = 0;
    if (Consume('0')) {
      if (IsAsciiDigit(Peek())) {
        return std::nullopt;  // no leading zeros
      }
    } else {
      while (IsAsciiDigit(Peek())) {
        value = value * 10 + (text_[pos_] - '0');
        ++pos_;
      }
    }
    if (Consume('.')) {
      if (!IsAsciiDigit(Peek())) {
        return std::nullopt;
      }
      double scale = 0.1;
      while (IsAsciiDigit(Peek())) {
        value += (text_[pos_] - '0') * scale;
        scale /= 10;
        ++pos_;
      }
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++pos_;
      bool negative_exponent = false;
      if (Peek() == '+' || Peek() == '-') {
        negative_exponent = Peek() == '-';
        ++pos_;
      }
      if (!IsAsciiDigit(Peek())) {
        return std::nullopt;
      }
      int exponent = 0;
      while (IsAsciiDigit(Peek())) {
        exponent = std::min(exponent * 10 + (text_[pos_] - '0'), 400);
        ++pos_;
      }
      for (int i = 0; i < exponent && value != 0; ++i) {
        value = negative_exponent ? value / 10 : value * 10;
        if (value > 1e300) {
          break;
        }
      }
    }
    return negative ? -value : value;
  }

  static int HexValue(char c) {
    if (IsAsciiDigit(c)) {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  std::optional<uint32_t> ParseHex4() {
    if (text_.size() - pos_ < 4) {
      return std::nullopt;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = HexValue(text_[pos_ + static_cast<size_t>(i)]);
      if (digit < 0) {
        return std::nullopt;
      }
      value = value * 16 + static_cast<uint32_t>(digit);
    }
    pos_ += 4;
    return value;
  }

  static void AppendUtf8(uint32_t code_point, std::string* out) {
    if (code_point < 0x80) {
      out->push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }

  // A string, with its escapes resolved. Raw bytes are copied as they are;
  // the caller checks the result is UTF-8. A \u escape of a lone surrogate
  // is refused.
  std::optional<std::string> ParseString() {
    if (!Consume('"')) {
      return std::nullopt;
    }
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        if (!IsValidUtf8(out)) {
          error_ = HelperLineError::kInvalidUtf8;
          return std::nullopt;
        }
        return out;
      }
      if (static_cast<uint8_t>(c) < 0x20) {
        return std::nullopt;  // control characters must be escaped
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return std::nullopt;
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escape);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          std::optional<uint32_t> unit = ParseHex4();
          if (!unit) {
            return std::nullopt;
          }
          uint32_t code_point = *unit;
          if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            error_ = HelperLineError::kInvalidUtf8;
            return std::nullopt;
          }
          if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            if (!ConsumeWord("\\u")) {
              error_ = HelperLineError::kInvalidUtf8;
              return std::nullopt;
            }
            std::optional<uint32_t> low = ParseHex4();
            if (!low || *low < 0xDC00 || *low > 0xDFFF) {
              error_ = HelperLineError::kInvalidUtf8;
              return std::nullopt;
            }
            code_point =
                0x10000 + ((code_point - 0xD800) << 10) + (*low - 0xDC00);
          }
          AppendUtf8(code_point, &out);
          break;
        }
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::string_view text_;
  size_t pos_ = 0;
  HelperLineError error_ = HelperLineError::kNone;
};

const JsonValue* FindMember(
    const std::vector<std::pair<std::string, JsonValue>>& members,
    std::string_view key) {
  for (const auto& member : members) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

std::optional<HelperEventType> EventTypeFromName(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, HelperEventType>, 7>
      kTypes = {{{"ready", HelperEventType::kReady},
                 {"wake", HelperEventType::kWake},
                 {"speech_start", HelperEventType::kSpeechStart},
                 {"partial", HelperEventType::kPartial},
                 {"final", HelperEventType::kFinal},
                 {"speech_end", HelperEventType::kSpeechEnd},
                 {"error", HelperEventType::kError}}};
  for (const auto& [type_name, type] : kTypes) {
    if (name == type_name) {
      return type;
    }
  }
  return std::nullopt;
}

// An optional whole number from 0 to 2^53 ("epoch", "audio_ms"); -1 when
// it isn't there.
HelperLineError ReadCount(
    const std::vector<std::pair<std::string, JsonValue>>& members,
    std::string_view key,
    int64_t* out) {
  *out = -1;
  const JsonValue* value = FindMember(members, key);
  if (!value) {
    return HelperLineError::kNone;
  }
  constexpr double kMaxCount = 9007199254740992.0;  // 2^53
  if (value->kind != JsonValue::Kind::kNumber || !(value->number >= 0) ||
      value->number > kMaxCount ||
      value->number !=
          static_cast<double>(static_cast<int64_t>(value->number))) {
    return HelperLineError::kBadField;
  }
  *out = static_cast<int64_t>(value->number);
  return HelperLineError::kNone;
}

// A required or optional string field within |cap| bytes.
HelperLineError ReadString(
    const std::vector<std::pair<std::string, JsonValue>>& members,
    std::string_view key,
    size_t cap,
    bool required,
    std::string* out) {
  const JsonValue* value = FindMember(members, key);
  if (!value) {
    return required ? HelperLineError::kMissingField : HelperLineError::kNone;
  }
  if (value->kind != JsonValue::Kind::kString) {
    return HelperLineError::kBadField;
  }
  if (value->string.size() > cap) {
    return HelperLineError::kFieldTooLong;
  }
  *out = value->string;
  return HelperLineError::kNone;
}

}  // namespace

// ---------------------------------------------------------------------------
// Browser to helper
// ---------------------------------------------------------------------------

HelperConfig::HelperConfig() = default;
HelperConfig::~HelperConfig() = default;
HelperConfig::HelperConfig(const HelperConfig&) = default;
HelperConfig& HelperConfig::operator=(const HelperConfig&) = default;
HelperConfig::HelperConfig(HelperConfig&&) = default;
HelperConfig& HelperConfig::operator=(HelperConfig&&) = default;

std::string EncodeHelperFrame(HelperFrameType type, std::string_view payload) {
  if (payload.size() > kMaxFramePayloadBytes) {
    return std::string();
  }
  std::string frame;
  frame.reserve(kFrameHeaderBytes + payload.size());
  AppendUint32(static_cast<uint32_t>(type), &frame);
  AppendUint32(static_cast<uint32_t>(payload.size()), &frame);
  frame.append(payload);
  return frame;
}

std::string EncodeAudioFrame(std::span<const int16_t> samples) {
  if (samples.size() > kMaxFramePayloadBytes / 2) {
    return std::string();
  }
  std::string frame;
  frame.reserve(kFrameHeaderBytes + samples.size() * 2);
  AppendUint32(static_cast<uint32_t>(HelperFrameType::kAudio), &frame);
  AppendUint32(static_cast<uint32_t>(samples.size() * 2), &frame);
  for (const int16_t sample : samples) {
    const auto bits = static_cast<uint16_t>(sample);
    frame.push_back(static_cast<char>(bits & 0xff));
    frame.push_back(static_cast<char>(bits >> 8));
  }
  return frame;
}

std::string EncodeConfigJson(const HelperConfig& config) {
  std::string json = "{\"wake_phrases\":";
  AppendJsonStrings(config.wake_phrases, kMaxWakePhrases, &json);
  json += ",\"wake_required\":";
  json += config.wake_required ? "true" : "false";
  json += ",\"hotwords\":";
  AppendJsonStrings(config.hotwords, kMaxHotwords, &json);
  json += ",\"vad_silence_ms\":";
  json += std::to_string(
      std::clamp(config.vad_silence_ms, kMinVadSilenceMs, kMaxVadSilenceMs));
  json += "}";
  return json;
}

std::string NumberAsWords(std::string_view digits) {
  if (digits.empty() || digits.size() > 9 || !AllDigits(digits)) {
    return std::string();
  }
  uint32_t n = 0;
  for (const char c : digits) {
    n = n * 10 + static_cast<uint32_t>(c - '0');
  }
  // Years as they are said: "nineteen eighty five", "twenty twenty two";
  // 2000-2009 as numbers ("two thousand five").
  if (digits.size() == 4 && n >= 1100 && n <= 2099 &&
      !(n >= 2000 && n <= 2009)) {
    std::string words = CardinalWords(n / 100);
    const uint32_t low = n % 100;
    if (low == 0) {
      AppendWord("hundred", &words);
    } else if (low < 10) {
      AppendWord("oh", &words);
      AppendWord(kOnes[low], &words);
    } else {
      AppendWord(CardinalWords(low), &words);
    }
    return words;
  }
  return CardinalWords(n);
}

std::string ReduceSpeechPhrase(std::string_view text) {
  // Only what SpeechSafeText() reads; then the words as voice mode folds
  // them (accents to base letters, lower case), with each word's source
  // to keep its apostrophes.
  NormalizeOptions options;
  options.max_words = kMaxPhraseWords * 2;
  const std::string_view source = text.substr(0, kMaxSpeechScanBytes);
  const NormalizedText normalized = NormalizeSpokenText(source, options);
  std::string phrase;
  size_t count = 0;
  for (size_t i = 0; i < normalized.words.size(); ++i) {
    const SpokenWord& word = normalized.words[i];
    const std::string_view folded = normalized.Word(i);
    std::string reduced;
    if (word.numeric) {
      reduced = NumberAsWords(folded);
    } else if (const std::string ordinal = OrdinalAsWords(folded);
               !ordinal.empty()) {
      reduced = ordinal;
    } else if (std::optional<std::string> ascii = AsciiWordWithApostrophes(
                   source.substr(word.source_begin,
                                 word.source_end - word.source_begin))) {
      reduced = std::move(*ascii);
    } else {
      reduced = std::string(folded);
    }
    // Scripts the folding leaves as they are (and digits too long to read)
    // can't be spelled: left out.
    if (!IsPhraseWord(reduced) && !reduced.empty()) {
      std::string kept;
      size_t start = 0;
      while (start < reduced.size()) {
        size_t end = reduced.find(' ', start);
        if (end == std::string::npos) {
          end = reduced.size();
        }
        const std::string_view part =
            std::string_view(reduced).substr(start, end - start);
        if (IsPhraseWord(part)) {
          AppendWord(part, &kept);
        }
        start = end + 1;
      }
      reduced = std::move(kept);
    }
    if (reduced.empty()) {
      continue;
    }
    if (!AppendPhraseWords(reduced, &phrase, &count)) {
      break;
    }
  }
  return phrase;
}

std::vector<std::string> HotwordsFromPageMap(const PageMap& map, size_t max) {
  std::vector<std::string> phrases;
  std::vector<std::string> seen;  // lower case
  auto add = [&](std::string_view text) {
    if (phrases.size() >= max) {
      return;
    }
    std::string phrase = ReduceSpeechPhrase(text);
    std::string key = LowerAscii(phrase);
    if (phrase.empty() ||
        std::find(seen.begin(), seen.end(), key) != seen.end()) {
      return;
    }
    seen.push_back(std::move(key));
    phrases.push_back(std::move(phrase));
  };
  for (const PageList& list : map.lists) {
    for (const PageListEntry& entry : list.entries) {
      if (phrases.size() >= max) {
        return phrases;
      }
      for (const std::string_view part : SplitTitle(entry.title)) {
        add(part);
      }
      add(entry.channel);
    }
  }
  return phrases;
}

// ---------------------------------------------------------------------------
// Helper to browser
// ---------------------------------------------------------------------------

HelperEvent::HelperEvent() = default;
HelperEvent::~HelperEvent() = default;
HelperEvent::HelperEvent(const HelperEvent&) = default;
HelperEvent& HelperEvent::operator=(const HelperEvent&) = default;
HelperEvent::HelperEvent(HelperEvent&&) = default;
HelperEvent& HelperEvent::operator=(HelperEvent&&) = default;

ParsedHelperLine::ParsedHelperLine() = default;
ParsedHelperLine::~ParsedHelperLine() = default;
ParsedHelperLine::ParsedHelperLine(const ParsedHelperLine&) = default;
ParsedHelperLine& ParsedHelperLine::operator=(const ParsedHelperLine&) =
    default;
ParsedHelperLine::ParsedHelperLine(ParsedHelperLine&&) = default;
ParsedHelperLine& ParsedHelperLine::operator=(ParsedHelperLine&&) = default;

std::string_view HelperEventTypeName(HelperEventType type) {
  switch (type) {
    case HelperEventType::kReady:
      return "ready";
    case HelperEventType::kWake:
      return "wake";
    case HelperEventType::kSpeechStart:
      return "speech_start";
    case HelperEventType::kPartial:
      return "partial";
    case HelperEventType::kFinal:
      return "final";
    case HelperEventType::kSpeechEnd:
      return "speech_end";
    case HelperEventType::kError:
      return "error";
  }
  return "unknown";
}

std::string_view HelperLineErrorName(HelperLineError error) {
  switch (error) {
    case HelperLineError::kNone:
      return "none";
    case HelperLineError::kTooLong:
      return "line too long";
    case HelperLineError::kInvalidUtf8:
      return "invalid UTF-8";
    case HelperLineError::kNotJson:
      return "not JSON";
    case HelperLineError::kNotObject:
      return "not a JSON object";
    case HelperLineError::kDuplicateKey:
      return "duplicate key";
    case HelperLineError::kUnknownType:
      return "unknown type";
    case HelperLineError::kMissingField:
      return "missing field";
    case HelperLineError::kBadField:
      return "bad field";
    case HelperLineError::kFieldTooLong:
      return "field too long";
  }
  return "unknown";
}

ParsedHelperLine ParseHelperLine(std::string_view line) {
  ParsedHelperLine result;
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line.size() > kMaxHelperLineBytes) {
    result.error = HelperLineError::kTooLong;
    return result;
  }
  if (!IsValidUtf8(line)) {
    result.error = HelperLineError::kInvalidUtf8;
    return result;
  }
  LineParser parser(line);
  std::optional<std::vector<std::pair<std::string, JsonValue>>> members =
      parser.ParseObject();
  if (!members) {
    result.error = parser.error();
    return result;
  }
  std::string type_name;
  result.error = ReadString(*members, "type", kMaxHelperNameBytes,
                            /*required=*/true, &type_name);
  if (result.error != HelperLineError::kNone) {
    return result;
  }
  const std::optional<HelperEventType> type = EventTypeFromName(type_name);
  if (!type) {
    result.error = HelperLineError::kUnknownType;
    return result;
  }
  HelperEvent& event = result.event;
  event.type = *type;
  if (*type != HelperEventType::kReady && *type != HelperEventType::kError) {
    result.error = ReadCount(*members, "epoch", &event.epoch);
    if (result.error == HelperLineError::kNone) {
      result.error = ReadCount(*members, "audio_ms", &event.audio_ms);
    }
    if (result.error != HelperLineError::kNone) {
      return result;
    }
  }
  switch (*type) {
    case HelperEventType::kReady: {
      result.error = ReadString(*members, "engine", kMaxHelperNameBytes,
                                /*required=*/true, &event.engine);
      if (result.error != HelperLineError::kNone) {
        return result;
      }
      event.engine = SpeechSafeText(event.engine, kMaxHelperNameBytes);
      if (const JsonValue* models = FindMember(*members, "models")) {
        std::vector<std::string> names;
        if (models->kind == JsonValue::Kind::kString) {
          names.push_back(models->string);
        } else if (models->kind == JsonValue::Kind::kStrings) {
          names = models->strings;
        } else {
          result.error = HelperLineError::kBadField;
          return result;
        }
        if (names.size() > kMaxHelperModels) {
          result.error = HelperLineError::kFieldTooLong;
          return result;
        }
        for (const std::string& name : names) {
          if (name.size() > kMaxHelperNameBytes) {
            result.error = HelperLineError::kFieldTooLong;
            return result;
          }
          event.models.push_back(SpeechSafeText(name, kMaxHelperNameBytes));
        }
      }
      if (const JsonValue* protocol = FindMember(*members, "protocol")) {
        if (protocol->kind != JsonValue::Kind::kNumber ||
            protocol->number != kSpeechProtocolVersion) {
          result.error = HelperLineError::kBadField;
          return result;
        }
      }
      break;
    }
    case HelperEventType::kPartial:
    case HelperEventType::kFinal: {
      std::string text;
      result.error = ReadString(*members, "text", kMaxHelperTextBytes,
                                /*required=*/true, &text);
      if (result.error != HelperLineError::kNone) {
        return result;
      }
      event.text = SpeechSafeText(text, kMaxHelperTextBytes);
      if (*type == HelperEventType::kFinal) {
        const JsonValue* confidence = FindMember(*members, "confidence");
        if (!confidence) {
          result.error = HelperLineError::kMissingField;
          return result;
        }
        // NaN fails both comparisons.
        if (confidence->kind != JsonValue::Kind::kNumber ||
            !(confidence->number >= 0 && confidence->number <= 1)) {
          result.error = HelperLineError::kBadField;
          return result;
        }
        event.confidence = confidence->number;
      }
      break;
    }
    case HelperEventType::kError: {
      result.error = ReadString(*members, "code", kMaxHelperNameBytes,
                                /*required=*/true, &event.code);
      if (result.error != HelperLineError::kNone) {
        return result;
      }
      result.error = ReadString(*members, "message", kMaxHelperTextBytes,
                                /*required=*/false, &event.message);
      if (result.error != HelperLineError::kNone) {
        return result;
      }
      event.code = SpeechSafeText(event.code, kMaxHelperNameBytes);
      event.message = SpeechSafeText(event.message, kMaxHelperTextBytes);
      break;
    }
    case HelperEventType::kWake:
    case HelperEventType::kSpeechStart:
    case HelperEventType::kSpeechEnd:
      break;
  }
  return result;
}

HelperLineReader::HelperLineReader() = default;
HelperLineReader::~HelperLineReader() = default;

bool HelperLineReader::Feed(std::string_view bytes,
                            std::vector<std::string>* lines) {
  if (failed_) {
    return false;
  }
  while (!bytes.empty()) {
    const size_t newline = bytes.find('\n');
    const std::string_view piece = bytes.substr(0, newline);
    // A '\r' before the '\n' is allowed: one byte over the cap.
    if (partial_.size() + piece.size() > kMaxHelperLineBytes + 1) {
      failed_ = true;
      partial_.clear();
      return false;
    }
    partial_.append(piece);
    if (newline == std::string_view::npos) {
      return true;
    }
    if (!partial_.empty() && partial_.back() == '\r') {
      partial_.pop_back();
    }
    if (partial_.size() > kMaxHelperLineBytes) {
      failed_ = true;
      partial_.clear();
      return false;
    }
    lines->push_back(std::move(partial_));
    partial_.clear();
    bytes.remove_prefix(newline + 1);
  }
  return true;
}

HelperLineBudget::HelperLineBudget() = default;

bool HelperLineBudget::Take(int64_t now_ms) {
  if (last_ms_ >= 0 && now_ms > last_ms_) {
    tokens_ = std::min<double>(
        kLineBurst, tokens_ + static_cast<double>(now_ms - last_ms_) *
                                  kLinesPerSecond / 1000.0);
  }
  if (last_ms_ < 0 || now_ms > last_ms_) {
    last_ms_ = now_ms;
  }
  if (tokens_ < 1) {
    return false;
  }
  tokens_ -= 1;
  return true;
}

}  // namespace voice
}  // namespace molt_ai
