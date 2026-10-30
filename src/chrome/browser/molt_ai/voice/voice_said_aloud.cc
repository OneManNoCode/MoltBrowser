// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_said_aloud.h"

#include <stddef.h>

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_text.h"

namespace molt_ai {
namespace voice {

namespace {

bool IsUpper(char c) {
  return c >= 'A' && c <= 'Z';
}
bool IsLower(char c) {
  return c >= 'a' && c <= 'z';
}

class Words {
 public:
  explicit Words(size_t max_bytes) : max_bytes_(max_bytes) {}

  // False once |max_bytes| is reached.
  bool Add(std::string_view word) {
    if (word.empty()) {
      return true;
    }
    const size_t need = word.size() + (out_.empty() ? 0 : 1);
    if (out_.size() + need > max_bytes_) {
      return false;
    }
    if (!out_.empty()) {
      out_.push_back(' ');
    }
    for (char c : word) {
      out_.push_back(IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return true;
  }

  std::string Take() { return std::move(out_); }

 private:
  const size_t max_bytes_;
  std::string out_;
};

// A word as the voice says it: its camel-case parts apart ("MoltBrowser",
// "iPhone"), as voice_phonemes.h's Phonemizer splits it, and an acronym
// both as a word and letter by letter (the Phonemizer spells "GPU", and
// says "HTTP" as a word only when its lexicon has it).
bool AddWord(std::string_view word, Words* words) {
  bool capitals = word.size() >= 2 && word.size() <= 5;
  for (char c : word) {
    capitals = capitals && IsUpper(c);
  }
  if (capitals) {
    if (!words->Add(word)) {
      return false;
    }
    for (size_t i = 0; i < word.size(); ++i) {
      if (!words->Add(word.substr(i, 1))) {
        return false;
      }
    }
    return true;
  }
  size_t start = 0;
  for (size_t i = 1; i < word.size(); ++i) {
    if (IsUpper(word[i]) && IsLower(word[i - 1])) {
      if (!words->Add(word.substr(start, i - start))) {
        return false;
      }
      start = i;
    }
  }
  return words->Add(word.substr(start));
}

}  // namespace

std::string SaidAloudWords(std::string_view text, size_t max_bytes) {
  Words words(max_bytes);
  for (const molt_voice::SpokenSentence& sentence :
       molt_voice::NormalizeForSpeech(text)) {
    for (const molt_voice::SpokenWord& word : sentence.words) {
      bool room = true;
      switch (word.kind) {
        case molt_voice::WordKind::kPause:
          break;
        case molt_voice::WordKind::kSpell:
          // Each letter its own word ("g p u").
          for (size_t i = 0; i < word.text.size() && room; ++i) {
            room = words.Add(std::string_view(word.text).substr(i, 1));
          }
          break;
        case molt_voice::WordKind::kWord:
          room = AddWord(word.text, &words);
          break;
      }
      if (!room) {
        return words.Take();
      }
    }
  }
  return words.Take();
}

}  // namespace voice
}  // namespace molt_ai
