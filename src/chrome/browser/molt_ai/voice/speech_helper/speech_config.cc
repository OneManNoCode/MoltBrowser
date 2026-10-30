// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/speech_config.h"

#include <stdint.h>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molt_speech {

namespace {

bool IsAsciiLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// A strict, single-pass reader of the CONFIG object. It never builds a
// tree: known fields are read into a ConfigUpdate, everything else is
// checked and skipped.
class ConfigReader {
 public:
  explicit ConfigReader(std::string_view text) : text_(text) {}

  const ConfigUpdate& update() const { return update_; }
  const std::string& error() const { return error_; }

  ConfigStatus Read() {
    SkipSpace();
    if (!Consume('{')) {
      return Protocol("CONFIG is not a JSON object");
    }
    std::set<std::string> seen;
    SkipSpace();
    if (Consume('}')) {
      return Finish();
    }
    while (true) {
      SkipSpace();
      std::string key;
      if (!ReadString(&key)) {
        return status_;
      }
      if (!seen.insert(key).second) {
        return Protocol("CONFIG has a field twice");
      }
      SkipSpace();
      if (!Consume(':')) {
        return Protocol(Where("expected ':'"));
      }
      SkipSpace();
      if (!ReadField(key)) {
        return status_;
      }
      SkipSpace();
      if (Consume(',')) {
        continue;
      }
      if (Consume('}')) {
        return Finish();
      }
      return Protocol(Where("expected ',' or '}'"));
    }
  }

 private:
  ConfigStatus Finish() {
    SkipSpace();
    if (pos_ != text_.size()) {
      return Protocol(Where("text after the object"));
    }
    return ConfigStatus::kOk;
  }

  bool ReadField(const std::string& key) {
    if (key == "wake_phrases") {
      std::vector<std::string> phrases;
      if (!ReadStringArray(kMaxWakePhrases, &phrases)) {
        return false;
      }
      if (phrases.empty()) {
        Config("wake_phrases is empty");
        return false;
      }
      std::vector<std::string> normalized;
      for (const std::string& phrase : phrases) {
        std::string clean;
        if (!NormalizeWakePhrase(phrase, &clean)) {
          Config("a wake phrase breaks the rules (ASCII letters, apostrophes "
                 "and spaces, 1 to 4 words, at most 64 bytes)");
          return false;
        }
        if (std::find(normalized.begin(), normalized.end(), clean) ==
            normalized.end()) {
          normalized.push_back(clean);
        }
      }
      update_.has_wake_phrases = true;
      update_.values.wake_phrases = std::move(normalized);
      return true;
    }
    if (key == "wake_required") {
      bool value = false;
      if (!ReadBool(&value)) {
        return false;
      }
      update_.has_wake_required = true;
      update_.values.wake_required = value;
      return true;
    }
    if (key == "hotwords") {
      std::vector<std::string> hotwords;
      if (!ReadStringArray(kMaxConfigHotwords, &hotwords)) {
        return false;
      }
      update_.has_hotwords = true;
      update_.values.hotwords = std::move(hotwords);
      return true;
    }
    if (key == "vad_silence_ms") {
      int64_t value = 0;
      if (!ReadInteger(&value)) {
        return false;
      }
      if (value < kMinVadSilenceMs || value > kMaxVadSilenceMs) {
        Config("vad_silence_ms is out of range (100 to 3000)");
        return false;
      }
      update_.has_vad_silence_ms = true;
      update_.values.vad_silence_ms = static_cast<int>(value);
      return true;
    }
    // A field a newer browser may send: any JSON value.
    return SkipValue(1);
  }

  // --- Values of known fields ----------------------------------------------

  bool ReadStringArray(size_t max_items, std::vector<std::string>* out) {
    if (!Consume('[')) {
      if (AtValueStart()) {
        Config("expected an array of strings");
      } else {
        Protocol(Where("expected a value"));
      }
      return false;
    }
    SkipSpace();
    if (Consume(']')) {
      return true;
    }
    while (true) {
      SkipSpace();
      if (Peek() != '"') {
        if (AtValueStart()) {
          Config("expected an array of strings");
        } else {
          Protocol(Where("expected a value"));
        }
        return false;
      }
      std::string item;
      if (!ReadString(&item)) {
        return false;
      }
      if (out->size() == max_items) {
        Config("too many items in an array");
        return false;
      }
      out->push_back(std::move(item));
      SkipSpace();
      if (Consume(',')) {
        continue;
      }
      if (Consume(']')) {
        return true;
      }
      Protocol(Where("expected ',' or ']'"));
      return false;
    }
  }

  bool ReadBool(bool* out) {
    if (ConsumeWord("true")) {
      *out = true;
      return true;
    }
    if (ConsumeWord("false")) {
      *out = false;
      return true;
    }
    if (AtValueStart()) {
      Config("expected true or false");
    } else {
      Protocol(Where("expected a value"));
    }
    return false;
  }

  bool ReadInteger(int64_t* out) {
    const char c = Peek();
    if (c != '-' && !(c >= '0' && c <= '9')) {
      if (AtValueStart()) {
        Config("expected an integer");
      } else {
        Protocol(Where("expected a value"));
      }
      return false;
    }
    bool is_integer = false;
    bool overflow = false;
    int64_t value = 0;
    if (!ReadNumber(&is_integer, &overflow, &value)) {
      return false;
    }
    if (!is_integer || overflow) {
      Config("expected an integer");
      return false;
    }
    *out = value;
    return true;
  }

  // --- JSON grammar ----------------------------------------------------------

  bool AtValueStart() const {
    const char c = Peek();
    return c == '{' || c == '[' || c == '"' || c == '-' ||
           (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n';
  }

  bool SkipValue(int depth) {
    if (depth > kMaxJsonDepth) {
      Protocol("CONFIG nests too deeply");
      return false;
    }
    SkipSpace();
    const char c = Peek();
    if (c == '{') {
      ++pos_;
      SkipSpace();
      if (Consume('}')) {
        return true;
      }
      while (true) {
        SkipSpace();
        std::string key;
        if (!ReadString(&key)) {
          return false;
        }
        SkipSpace();
        if (!Consume(':')) {
          Protocol(Where("expected ':'"));
          return false;
        }
        if (!SkipValue(depth + 1)) {
          return false;
        }
        SkipSpace();
        if (Consume(',')) {
          continue;
        }
        if (Consume('}')) {
          return true;
        }
        Protocol(Where("expected ',' or '}'"));
        return false;
      }
    }
    if (c == '[') {
      ++pos_;
      SkipSpace();
      if (Consume(']')) {
        return true;
      }
      while (true) {
        if (!SkipValue(depth + 1)) {
          return false;
        }
        SkipSpace();
        if (Consume(',')) {
          continue;
        }
        if (Consume(']')) {
          return true;
        }
        Protocol(Where("expected ',' or ']'"));
        return false;
      }
    }
    if (c == '"') {
      std::string ignored;
      return ReadString(&ignored);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      bool is_integer = false;
      bool overflow = false;
      int64_t value = 0;
      return ReadNumber(&is_integer, &overflow, &value);
    }
    if (ConsumeWord("true") || ConsumeWord("false") || ConsumeWord("null")) {
      return true;
    }
    Protocol(Where("expected a value"));
    return false;
  }

  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  bool ReadNumber(bool* is_integer, bool* overflow, int64_t* value) {
    bool negative = Consume('-');
    if (Peek() == '0') {
      ++pos_;
    } else if (Peek() >= '1' && Peek() <= '9') {
      while (Peek() >= '0' && Peek() <= '9') {
        const int digit = Peek() - '0';
        if (*value > (INT64_MAX - digit) / 10) {
          *overflow = true;
        } else {
          *value = *value * 10 + digit;
        }
        ++pos_;
      }
    } else {
      Protocol(Where("bad number"));
      return false;
    }
    *is_integer = true;
    if (Consume('.')) {
      *is_integer = false;
      if (!(Peek() >= '0' && Peek() <= '9')) {
        Protocol(Where("bad number"));
        return false;
      }
      while (Peek() >= '0' && Peek() <= '9') {
        ++pos_;
      }
    }
    if (Peek() == 'e' || Peek() == 'E') {
      *is_integer = false;
      ++pos_;
      if (Peek() == '+' || Peek() == '-') {
        ++pos_;
      }
      if (!(Peek() >= '0' && Peek() <= '9')) {
        Protocol(Where("bad number"));
        return false;
      }
      while (Peek() >= '0' && Peek() <= '9') {
        ++pos_;
      }
    }
    if (negative) {
      *value = -*value;
    }
    return true;
  }

  // A JSON string, unescaped, checked as UTF-8, at most kMaxJsonStringBytes.
  bool ReadString(std::string* out) {
    if (!Consume('"')) {
      Protocol(Where("expected a string"));
      return false;
    }
    out->clear();
    while (true) {
      if (pos_ >= text_.size()) {
        Protocol("CONFIG ends inside a string");
        return false;
      }
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) {
        Protocol(Where("control character in a string"));
        return false;
      }
      if (c == '\\') {
        ++pos_;
        if (!ReadEscape(out)) {
          return false;
        }
      } else if (c < 0x80) {
        out->push_back(static_cast<char>(c));
        ++pos_;
      } else {
        const size_t length = Utf8SequenceLength(pos_);
        if (length == 0) {
          Protocol(Where("invalid UTF-8"));
          return false;
        }
        out->append(text_.substr(pos_, length));
        pos_ += length;
      }
      if (out->size() > kMaxJsonStringBytes) {
        Protocol("a CONFIG string is over 1024 bytes");
        return false;
      }
    }
  }

  bool ReadEscape(std::string* out) {
    const char c = Peek();
    ++pos_;
    switch (c) {
      case '"':
        out->push_back('"');
        return true;
      case '\\':
        out->push_back('\\');
        return true;
      case '/':
        out->push_back('/');
        return true;
      case 'b':
        out->push_back('\b');
        return true;
      case 'f':
        out->push_back('\f');
        return true;
      case 'n':
        out->push_back('\n');
        return true;
      case 'r':
        out->push_back('\r');
        return true;
      case 't':
        out->push_back('\t');
        return true;
      case 'u': {
        uint32_t code = 0;
        if (!ReadHex4(&code)) {
          return false;
        }
        if (code >= 0xDC00 && code <= 0xDFFF) {
          Protocol(Where("lone surrogate"));
          return false;
        }
        if (code >= 0xD800 && code <= 0xDBFF) {
          uint32_t low = 0;
          if (!Consume('\\') || !Consume('u') || !ReadHex4(&low) ||
              low < 0xDC00 || low > 0xDFFF) {
            Protocol(Where("lone surrogate"));
            return false;
          }
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        AppendUtf8(code, out);
        return true;
      }
      default:
        Protocol(Where("bad escape"));
        return false;
    }
  }

  bool ReadHex4(uint32_t* out) {
    if (text_.size() - pos_ < 4) {
      Protocol(Where("bad \\u escape"));
      return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + static_cast<size_t>(i)];
      uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<uint32_t>(c - 'A' + 10);
      } else {
        Protocol(Where("bad \\u escape"));
        return false;
      }
      value = (value << 4) | digit;
    }
    pos_ += 4;
    *out = value;
    return true;
  }

  static void AppendUtf8(uint32_t code, std::string* out) {
    if (code < 0x80) {
      out->push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  // The length of the well-formed UTF-8 sequence at |at| (Unicode's table
  // 3-7: no overlong forms, surrogates or values past U+10FFFF), or 0.
  size_t Utf8SequenceLength(size_t at) const {
    const auto byte = [this](size_t i) -> unsigned char {
      return i < text_.size() ? static_cast<unsigned char>(text_[i]) : 0;
    };
    const unsigned char b0 = byte(at);
    const auto cont = [](unsigned char b, int lo, int hi) {
      return b >= lo && b <= hi;
    };
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      return cont(byte(at + 1), 0x80, 0xBF) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
      const int lo = b0 == 0xE0 ? 0xA0 : 0x80;
      const int hi = b0 == 0xED ? 0x9F : 0xBF;
      return cont(byte(at + 1), lo, hi) && cont(byte(at + 2), 0x80, 0xBF) ? 3
                                                                          : 0;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
      const int lo = b0 == 0xF0 ? 0x90 : 0x80;
      const int hi = b0 == 0xF4 ? 0x8F : 0xBF;
      return cont(byte(at + 1), lo, hi) && cont(byte(at + 2), 0x80, 0xBF) &&
                     cont(byte(at + 3), 0x80, 0xBF)
                 ? 4
                 : 0;
    }
    return 0;
  }

  // --- Characters -----------------------------------------------------------

  char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

  bool Consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
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
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  std::string Where(const std::string& what) const {
    return "CONFIG: " + what + " at byte " + std::to_string(pos_);
  }

  ConfigStatus Protocol(const std::string& message) {
    if (status_ == ConfigStatus::kOk) {
      status_ = ConfigStatus::kProtocolError;
      error_ = message;
    }
    return status_;
  }

  ConfigStatus Config(const std::string& message) {
    if (status_ == ConfigStatus::kOk) {
      status_ = ConfigStatus::kConfigError;
      error_ = "CONFIG: " + message;
    }
    return status_;
  }

  const std::string_view text_;
  size_t pos_ = 0;
  ConfigStatus status_ = ConfigStatus::kOk;
  ConfigUpdate update_;
  std::string error_;
};

}  // namespace

SpeechConfig::SpeechConfig() = default;
SpeechConfig::SpeechConfig(const SpeechConfig&) = default;
SpeechConfig& SpeechConfig::operator=(const SpeechConfig&) = default;
SpeechConfig::~SpeechConfig() = default;

ConfigUpdate::ConfigUpdate() = default;
ConfigUpdate::ConfigUpdate(const ConfigUpdate&) = default;
ConfigUpdate& ConfigUpdate::operator=(const ConfigUpdate&) = default;
ConfigUpdate::~ConfigUpdate() = default;

SpeechConfig DefaultSpeechConfig() {
  SpeechConfig config;
  config.wake_phrases = {"hey molt"};
  return config;
}

ConfigStatus ParseConfigFrame(std::string_view json,
                              ConfigUpdate* update,
                              std::string* error) {
  ConfigReader reader(json);
  const ConfigStatus status = reader.Read();
  if (status == ConfigStatus::kOk) {
    *update = reader.update();
  } else if (error) {
    *error = reader.error();
  }
  return status;
}

void ApplyConfigUpdate(const ConfigUpdate& update, SpeechConfig* config) {
  if (update.has_wake_phrases) {
    config->wake_phrases = update.values.wake_phrases;
  }
  if (update.has_wake_required) {
    config->wake_required = update.values.wake_required;
  }
  if (update.has_hotwords) {
    config->hotwords = update.values.hotwords;
  }
  if (update.has_vad_silence_ms) {
    config->vad_silence_ms = update.values.vad_silence_ms;
  }
}

bool NormalizeWakePhrase(std::string_view phrase, std::string* out) {
  if (phrase.empty() || phrase.size() > kMaxWakePhraseBytes) {
    return false;
  }
  std::string result;
  size_t words = 0;
  bool in_word = false;
  bool word_has_letter = false;
  for (const char c : phrase) {
    if (c == ' ') {
      if (in_word && !word_has_letter) {
        return false;
      }
      in_word = false;
      continue;
    }
    if (!IsAsciiLetter(c) && c != '\'') {
      return false;
    }
    if (!in_word) {
      if (++words > kMaxWakePhraseWords) {
        return false;
      }
      if (!result.empty()) {
        result.push_back(' ');
      }
      in_word = true;
      word_has_letter = false;
    }
    word_has_letter = word_has_letter || IsAsciiLetter(c);
    result.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                          : c);
  }
  if (words == 0 || (in_word && !word_has_letter)) {
    return false;
  }
  *out = std::move(result);
  return true;
}

}  // namespace molt_speech
