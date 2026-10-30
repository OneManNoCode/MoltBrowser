// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_phonemes.h"

#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_text.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_voice {
namespace {

// A few lines of sherpa-onnx's lexicon-en-us.txt, as they are there.
constexpr char kLexicon[] =
    "# Lexicon: word(s) || space-separated phones\n"
    "kaldi || k ˈ æ l d i\n"
    "a || ˈ e ɪ\n"
    "book || b ˈ ʊ k\n"
    "browser || b ɹ ˈ aʊ z ɚ\n"
    "comfort || k ˈ ʌ m f ɚ t\n"
    "hello || h ə lˈ o ʊ\n"
    "i || ˈ a ɪ\n"
    "make || m ˈ e ɪ k\n"
    "molt || m ˈ o ʊ l t\n"
    "phone || f ˈ o ʊ n\n"
    "play || p lˈ e ɪ\n"
    "quiet || k w ˈ aɪ ə t\n"
    "stop || s t ˈ ɑː p\n"
    "the || ð ə\n"
    "watch || w ˈ ɑː t ʃ\n"
    "world || w ˈ ɜː l d\n"
    "youtube || jˈ uː t uː b\n"
    "a & || ˈ e ɪ ɐ n d\n"
    "Kaldi || k ˈ æ l d i\n";

// The voice's tokens: every code point of the IPA above, the space and
// punctuation (as Piper's tokens.txt has them).
constexpr char kTokens[] =
    "_ 0\n^ 1\n$ 2\n  3\n! 4\n, 8\n. 10\n? 13\na 14\nb 15\nd 17\ne 18\n"
    "f 19\nh 20\ni 21\nj 22\nk 23\nl 24\nm 25\nn 26\no 27\np 28\ns 31\n"
    "t 32\nu 33\nv 34\nw 35\nz 38\næ 39\nð 41\nŋ 44\nɐ 50\nɑ 51\nɔ 54\n"
    "ə 59\nɚ 60\nɛ 61\nɜ 62\nɡ 66\nɪ 74\nɹ 88\nʃ 96\nʊ 100\nʌ 102\nʒ 108\n"
    "ˈ 120\nˌ 121\nː 122\nθ 126\nᵻ 128\n";

Phonemizer MakePhonemizer() {
  Lexicon lexicon;
  std::string error;
  EXPECT_TRUE(lexicon.Load(kLexicon, &error)) << error;
  std::vector<uint32_t> tokens;
  EXPECT_TRUE(LoadVoiceTokens(kTokens, &tokens, &error)) << error;
  return Phonemizer(std::move(lexicon), std::move(tokens));
}

std::string Utf8(const std::vector<int32_t>& cps) {
  std::string out;
  for (int32_t c : cps) {
    const auto cp = static_cast<uint32_t>(c);
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string SentencePhonemes(const Phonemizer& phonemizer,
                             std::string_view text) {
  std::string out;
  for (const SpokenSentence& sentence : NormalizeForSpeech(text)) {
    out += Utf8(phonemizer.Sentence(sentence));
  }
  return out;
}

TEST(VoicePhonemesTest, Lexicon) {
  Lexicon lexicon;
  std::string error;
  ASSERT_TRUE(lexicon.Load(kLexicon, &error)) << error;
  // Multi-word and capitalized entries can't come out of voice_text.h.
  EXPECT_EQ(17u, lexicon.size());
  EXPECT_EQ("həlˈoʊ", lexicon.Find("hello"));
  EXPECT_EQ("", lexicon.Find("Kaldi"));
  EXPECT_EQ("", lexicon.Find("hell"));
  EXPECT_TRUE(lexicon.Has("youtube"));

  Lexicon bad;
  EXPECT_FALSE(bad.Load("hello h ə l o ʊ\n", &error));
  EXPECT_FALSE(bad.Load("hello || \xFF\n", &error));
  EXPECT_FALSE(bad.Load(std::string(kMaxLexiconBytes + 1, '#'), &error));
  EXPECT_TRUE(bad.Load("", &error));
  EXPECT_EQ(0u, bad.size());
}

TEST(VoicePhonemesTest, Tokens) {
  std::vector<uint32_t> tokens;
  std::string error;
  ASSERT_TRUE(LoadVoiceTokens(kTokens, &tokens, &error)) << error;
  EXPECT_TRUE(std::binary_search(tokens.begin(), tokens.end(), ' '));
  EXPECT_TRUE(std::binary_search(tokens.begin(), tokens.end(), 0x02C8u));
  EXPECT_FALSE(LoadVoiceTokens("ab 3\n", &tokens, &error));
  EXPECT_FALSE(LoadVoiceTokens("a x\n", &tokens, &error));
  EXPECT_FALSE(LoadVoiceTokens("", &tokens, &error));
}

TEST(VoicePhonemesTest, Words) {
  const Phonemizer p = MakePhonemizer();
  // The lexicon; function words as said in running text.
  EXPECT_EQ("həlˈoʊ", p.Word("Hello", false));
  EXPECT_EQ("ɐ", p.Word("a", false));
  EXPECT_EQ("ɔːɹ", p.Word("or", false));
  // Forms of known words.
  EXPECT_EQ("bˈʊks", p.Word("books", false));
  EXPECT_EQ("wˈɑːtʃɪz", p.Word("watches", false));
  EXPECT_EQ("plˈeɪd", p.Word("played", false));
  EXPECT_EQ("stˈɑːpt", p.Word("stopped", false));
  EXPECT_EQ("mˈeɪkɪŋ", p.Word("making", false));
  EXPECT_EQ("bˈʊks", p.Word("book's", false));
  EXPECT_EQ("ʌnplˈeɪd", p.Word("unplayed", false));
  // Joined words; the second part's stress becomes secondary.
  EXPECT_EQ("mˈoʊltbɹˌaʊzɚ", p.Word("MoltBrowser", false));
  EXPECT_EQ("kwˈaɪətkˌʌmfɚt", p.Word("quietcomfort", false));
  // Camel case of words the lexicon has.
  EXPECT_EQ("ˈaɪ fˈoʊn", p.Word("iPhone", false));
  // Acronyms and words without vowels are spelled; capitals in a title
  // are words.
  EXPECT_EQ("dʒˈiː pˈiː jˈuː", p.Word("GPU", false));
  EXPECT_EQ("sˈiː pˈiː pˈiː", p.Word("cpp", false));
  EXPECT_EQ("stˈɑːp", p.Word("STOP", false));
  EXPECT_EQ("ðə", p.Word("THE", true));
  // Unknown words: letter-to-sound.
  EXPECT_EQ("ˈiːlɪʃ", p.Word("Eilish", false));
  EXPECT_EQ("", p.Word("", false));
  EXPECT_EQ("", p.Word("'''", false));
}

TEST(VoicePhonemesTest, Sentences) {
  const Phonemizer p = MakePhonemizer();
  EXPECT_EQ("jˈuːtuːb wˈɜːld.", SentencePhonemes(p, "YouTube world"));
  EXPECT_EQ("həlˈoʊ, wˈɜːld?", SentencePhonemes(p, "Hello - world?"));
  // Nothing sayable: nothing.
  EXPECT_EQ("", SentencePhonemes(p, "\xF0\x9F\x8E\x89"));
  // Every code point is one of the voice's tokens: a word whose sounds
  // the voice lacks loses those sounds, not the sentence.
  Lexicon lexicon;
  std::string error;
  ASSERT_TRUE(lexicon.Load("odd || ʘ d\n", &error));
  const Phonemizer q(std::move(lexicon), {'d', ' ', '.'});
  EXPECT_EQ("d.", SentencePhonemes(q, "odd"));
}

TEST(VoicePhonemesTest, Units) {
  // Short: one unit.
  std::vector<int32_t> short_one(50, 'a');
  EXPECT_EQ(1u, Phonemizer::Units(short_one).size());
  // Long: cut after a pause in the second half of a unit, else at a
  // space; every unit within the limit, mid-sentence cuts ending in ",".
  std::vector<int32_t> words;
  for (int i = 0; i < 200; ++i) {
    words.insert(words.end(), {'b', 'a', 'b', ' '});
  }
  words.push_back('.');
  const std::vector<std::vector<int32_t>> units = Phonemizer::Units(words);
  ASSERT_GT(units.size(), 2u);
  for (size_t i = 0; i < units.size(); ++i) {
    EXPECT_LE(units[i].size(), kMaxUnitCodePoints + 1);
    EXPECT_NE(' ', units[i].front());
    if (i + 1 < units.size()) {
      EXPECT_EQ(',', units[i].back());
    }
  }
  EXPECT_EQ('.', units.back().back());
  // No space at all: cut at the limit.
  const std::vector<int32_t> solid(1000, 'a');
  for (const std::vector<int32_t>& unit : Phonemizer::Units(solid)) {
    EXPECT_LE(unit.size(), kMaxUnitCodePoints + 1);
  }
}

}  // namespace
}  // namespace molt_voice
