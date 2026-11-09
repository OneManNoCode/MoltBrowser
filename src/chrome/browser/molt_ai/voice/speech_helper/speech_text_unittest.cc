// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/speech_text.h"

#include <algorithm>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_speech {
namespace {

// "\xE2\x96\x81" is U+2581, the word-start mark.
#define WS "\xE2\x96\x81"

// A few pieces of the GigaSpeech spotter's vocabulary, with its scores.
constexpr char kKwsVocab[] =
    "<blk>\t0.000000\n" WS "HE\t-7.893394\n" WS "HEY\t-15.000000\n"
    "Y\t-6.611588\n" WS "MO\t-9.077404\n" WS "M\t-6.991071\n"
    "OL\t-9.346213\n" "L\t-5.999022\n" "T\t-3.568268\n" WS
    "\t-4.250000\n" "M\t-6.100000\n" "O\t-5.500000\n" "H\t-6.000000\n"
    "E\t-5.000000\n" "'\t-8.000000\n" "S\t-2.973654\n";

PieceVocab KwsVocab() {
  PieceVocab vocab;
  std::string error;
  EXPECT_TRUE(vocab.Load(kKwsVocab, &error)) << error;
  return vocab;
}

TokenSet TokensOf(const PieceVocab& vocab) {
  TokenSet tokens;
  // Every piece of the test vocabularies is a token unless a test says so.
  for (const char* piece :
       {"<blk>", WS "HE", WS "HEY", "Y", WS "MO", WS "M", "OL", "L", "T", WS,
        "M", "O", "H", "E", "'", "S"}) {
    if (vocab.Contains(piece)) {
      tokens.insert(piece);
    }
  }
  return tokens;
}

TEST(SpeechTextTest, LoadsVocabularies) {
  PieceVocab vocab;
  std::string error;
  EXPECT_TRUE(vocab.Load("a -1.5\nb\t2\n\n" WS "c -0.25\r\n", &error));
  EXPECT_EQ(vocab.size(), 3u);
  EXPECT_TRUE(vocab.Contains(WS "c"));
  for (const char* bad :
       {"a", "a x", "a -1e5", "a 1.", "a - 1", "a 1 2", "a 1\na 2",
        " a 1"}) {
    EXPECT_FALSE(vocab.Load(bad, &error)) << bad;
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(vocab.size(), 0u);
  }
  EXPECT_FALSE(vocab.Load(std::string(65, 'p') + " 1", &error));

  TokenSet tokens;
  EXPECT_TRUE(LoadTokenSet("<blk> 0\n" WS "the 4\n' 10\n", &tokens, &error));
  EXPECT_EQ(tokens.size(), 3u);
  EXPECT_EQ(tokens.count(WS "the"), 1u);
  EXPECT_FALSE(LoadTokenSet("x\n", &tokens, &error));
  EXPECT_FALSE(LoadTokenSet("x y\n", &tokens, &error));
}

TEST(SpeechTextTest, SplitsAsTheModelsSentencePieceDoes) {
  const PieceVocab vocab = KwsVocab();
  std::vector<std::string> pieces;
  // The highest-scoring split, the one the spotter was trained with
  // (docs/VOICE_BENCH.md §3.8): "▁MO L T" (-18.64) over "▁M OL T"
  // (-19.91); "▁HE Y" (-14.51) over "▁HEY" (-15).
  ASSERT_TRUE(vocab.Split("MOLT", &pieces));
  EXPECT_EQ(pieces, (std::vector<std::string>{WS "MO", "L", "T"}));
  ASSERT_TRUE(vocab.Split("HEY", &pieces));
  EXPECT_EQ(pieces, (std::vector<std::string>{WS "HE", "Y"}));
  EXPECT_FALSE(vocab.Split("HEX", &pieces));
  EXPECT_FALSE(vocab.Split("", &pieces));
  EXPECT_FALSE(vocab.Split(std::string(65, 'S'), &pieces));

  // Ties go to the longer piece.
  PieceVocab tie;
  std::string error;
  ASSERT_TRUE(tie.Load(WS "ab -2\n" WS "a -1\nb -1\n", &error));
  ASSERT_TRUE(tie.Split("ab", &pieces));
  EXPECT_EQ(pieces, (std::vector<std::string>{WS "ab"}));
}

TEST(SpeechTextTest, MakesKeywordLines) {
  const PieceVocab vocab = KwsVocab();
  const TokenSet tokens = TokensOf(vocab);
  std::string line;
  ASSERT_TRUE(WakeKeywordLine("hey molt", vocab, tokens, &line));
  EXPECT_EQ(line, WS "HE Y " WS "MO L T #0.15 @HEY_MOLT");
  // A word the vocabulary can't spell, or a piece missing from tokens.txt.
  EXPECT_FALSE(WakeKeywordLine("hey max", vocab, tokens, &line));
  TokenSet fewer = tokens;
  fewer.erase("T");
  EXPECT_FALSE(WakeKeywordLine("hey molt", vocab, fewer, &line));
  EXPECT_FALSE(WakeKeywordLine("", vocab, tokens, &line));
  EXPECT_FALSE(WakeKeywordLine("hey #molt", vocab, tokens, &line));
}

// "hey" said short ("he molt") is a second line under the phrase's label;
// other phrases have just their own.
TEST(SpeechTextTest, MakesTheShortHeyLineToo) {
  const PieceVocab vocab = KwsVocab();
  const TokenSet tokens = TokensOf(vocab);
  std::vector<std::string> lines;
  ASSERT_TRUE(WakeKeywordLines("hey molt", vocab, tokens, &lines));
  EXPECT_EQ(lines, (std::vector<std::string>{
                       WS "HE Y " WS "MO L T #0.15 @HEY_MOLT",
                       WS "HE " WS "MO L T #0.15 @HEY_MOLT"}));
  lines.clear();
  ASSERT_TRUE(WakeKeywordLines("molt", vocab, tokens, &lines));
  EXPECT_EQ(lines,
            (std::vector<std::string>{WS "MO L T #0.15 @MOLT"}));
  // The phrase itself must be spelled; its short form needn't be.
  lines.clear();
  EXPECT_FALSE(WakeKeywordLines("hey max", vocab, tokens, &lines));
  EXPECT_TRUE(lines.empty());
}

// A cased vocabulary like Kroko's: every ASCII letter, digit and the
// apostrophe, the word-start mark, and a few longer pieces.
PieceVocab RecognizerVocab(TokenSet* tokens) {
  std::string text = WS " -2.0\n" WS "the -1.8\nre -4.0\n";
  for (char c = 'a'; c <= 'z'; ++c) {
    text += std::string(1, c) + " -5.0\n";
    text += std::string(1, static_cast<char>(c - 'a' + 'A')) + " -6.0\n";
  }
  for (char c = '0'; c <= '9'; ++c) {
    text += std::string(1, c) + " -7.0\n";
  }
  text += "' -6.5\n";
  text += "\xC2\xA3 -9.0\n";  // £: a piece, but not in tokens below.
  PieceVocab vocab;
  std::string error;
  EXPECT_TRUE(vocab.Load(text, &error)) << error;
  std::string token_text;
  size_t pos = 0;
  int id = 0;
  while (pos < text.size()) {
    const size_t space = text.find(' ', pos);
    const std::string piece = text.substr(pos, space - pos);
    if (piece != "\xC2\xA3") {
      token_text += piece + " " + std::to_string(id++) + "\n";
    }
    pos = text.find('\n', pos) + 1;
  }
  EXPECT_TRUE(LoadTokenSet(token_text, tokens, &error)) << error;
  return vocab;
}

TEST(SpeechTextTest, CleansHotwords) {
  TokenSet tokens;
  const PieceVocab vocab = RecognizerVocab(&tokens);
  EXPECT_EQ(HotwordsArgument({"Bohemian Rhapsody", "Queen"}, vocab, tokens),
            "Bohemian Rhapsody/Queen");
  // Nothing sherpa-onnx's parser reads as syntax survives: ':' scores,
  // '/' separators, '#' thresholds, '@' phrases, line breaks.
  EXPECT_EQ(HotwordsArgument({"AC/DC :5.0", "#1 hits @home\nnext"}, vocab,
                             tokens),
            "AC DC 5 0/1 hits home next");
  // Words with a character the recognizer can't spell are dropped; words
  // of apostrophes only too; duplicates once.
  EXPECT_EQ(HotwordsArgument({"Beyonc\xC3\xA9 live", "'' don't", "Queen",
                              "Queen", "\xC2\xA3" "5"},
                             vocab, tokens),
            "Beyonc live/don't/Queen/5");
  EXPECT_EQ(HotwordsArgument({"", ":::", "\xFF\xFE"}, vocab, tokens), "");

  // At most six words and 64 bytes a phrase.
  EXPECT_EQ(HotwordsArgument({"a b c d e f g h"}, vocab, tokens),
            "a b c d e f");
  const std::string long_word(40, 'x');
  EXPECT_EQ(HotwordsArgument({long_word + " " + long_word}, vocab, tokens),
            long_word);
  EXPECT_EQ(HotwordsArgument({std::string(65, 'y')}, vocab, tokens), "");

  // At most 200 phrases and 4096 bytes in all.
  std::vector<std::string> many;
  for (int i = 0; i < 300; ++i) {
    many.push_back("name" + std::to_string(i));
  }
  std::string arg = HotwordsArgument(many, vocab, tokens);
  EXPECT_EQ(std::count(arg.begin(), arg.end(), '/'), 199);
  std::vector<std::string> long_phrases;
  for (int i = 0; i < 200; ++i) {
    long_phrases.push_back(std::string(50, 'q') + std::to_string(i));
  }
  arg = HotwordsArgument(long_phrases, vocab, tokens);
  EXPECT_LE(arg.size(), kMaxHotwordsBytes);
  EXPECT_GT(arg.size(), kMaxHotwordsBytes - 60);

  // A vocabulary without the word-start mark can spell nothing.
  PieceVocab no_mark;
  std::string error;
  ASSERT_TRUE(no_mark.Load("a -1\n", &error));
  EXPECT_EQ(HotwordsArgument({"a"}, no_mark, TokenSet{"a"}), "");
}

TEST(SpeechTextTest, CleansEventText) {
  EXPECT_EQ(CleanEventText("Open YouTube.", 1000), "Open YouTube.");
  EXPECT_EQ(CleanEventText("  a\t\nb  \r\n", 1000), "a b");
  // Control characters go (C0, DEL, C1; the white-space ones, NEL and
  // U+2028/9 among them, become a space), and so do bytes that aren't
  // well-formed UTF-8.
  EXPECT_EQ(CleanEventText("a\x01" "b\x7f" "c\xC2\x85" "d\xE2\x80\xA8" "e"
                           "\xC2\x9F" "f",
                           1000),
            "abc d ef");
  EXPECT_EQ(CleanEventText("x\xFF\xC0\xAF" "y\xED\xA0\x80" "z", 1000), "xyz");
  EXPECT_EQ(CleanEventText("caf\xC3\xA9 \xE2\x82\xAC" "5", 1000),
            "caf\xC3\xA9 \xE2\x82\xAC" "5");
  // Cut at a word near the end, else at a character.
  EXPECT_EQ(CleanEventText("one two three", 10), "one two");
  EXPECT_EQ(CleanEventText(std::string(20, 'a'), 10), std::string(10, 'a'));
  EXPECT_EQ(CleanEventText("\xC3\xA9\xC3\xA9\xC3\xA9", 5), "\xC3\xA9\xC3\xA9");
  std::string long_text;
  for (int i = 0; i < 400; ++i) {
    long_text += "word ";
  }
  const std::string cut = CleanEventText(long_text, 1000);
  EXPECT_LE(cut.size(), 1000u);
  EXPECT_EQ(cut.back(), 'd');
}

TEST(SpeechTextTest, FindsTheWakePhraseInATranscript) {
  const std::vector<std::string> en = {"hey molt"};
  // What the recognizer wrote for the spoken suite's "Hey Molt".
  for (const char* text :
       {"Hey Molt open YouTube", "Hey, Malt, play", "Hey mold. So",
        "He mold. Type below comma world period.", "Hey, Molt, roulette",
        "hey moult pause", "HEY MOLD"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 2u) << text;
  }
  // Run together: as many words as the phrase, one more, or fewer.
  for (const char* text : {"Himalt, bookmarked this page.", "Himalde. Open"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 1u) << text;
  }
  for (const char* text : {"him old. Play the one", "him alled. Open YouTube",
                           "him old"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 2u) << text;
  }
  // Run together with its last sound, the stop, lost: what the spoken
  // suite's microphone gave for "Hey Molt" said over a playing video.
  for (const char* text : {"him all. Full screen", "him all louder"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 2u) << text;
  }
  for (const char* text : {"Hamol, pause", "Himal next video"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 1u) << text;
  }
  // Run together with the greeting's "h" lost: one word, a vowel and then
  // the name's sounds as they are ("Immalt" in the spoken suite's file
  // run), not words that only share its consonants.
  for (const char* text : {"Immalt, said the volume to half.", "Emolt. Play",
                           "Amalt"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 1u) << text;
  }
  for (const char* text : {"omelet recipe", "Imelda", "I melt", "emulate",
                           "Immaltese", "Amulet", "almond", "Oyemolt"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 0u) << text;
  }
  // But not with the greeting heard as it is ("hey mall" is a mall, "he
  // mall" too), nor longer than the phrase or going on past the sound.
  // (Speech that begins "him all" is taken: few sentences start so.)
  for (const char* text : {"hey mall", "he mall", "hey malls", "Himalaya trip",
                           "himallow", "himalayan"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 0u) << text;
  }
  // Not the phrase, or not at the start: the bench's chatter written to
  // sound like it, and lost ones.
  for (const char* text :
       {"hey Walt, are you coming with us", "snakes molt their skin",
        "hello Mo, how was your day", "Hoping YouTube.", "open YouTube",
        "hey mall", "Hey Matt", "the Himalayas", "Himalaya trip",
        "play hey molt", "", " , ", "hey", "molt", "a molt"}) {
    EXPECT_EQ(WakePhraseWordsAtStart(text, en), 0u) << text;
  }
  // Several phrases, each as long as it is.
  const std::vector<std::string> two = {"hey molt browser", "hey molt"};
  EXPECT_EQ(WakePhraseWordsAtStart("hey malt browser go back", two), 3u);
  EXPECT_EQ(WakePhraseWordsAtStart("hey malt go back", two), 2u);
  // "oye molt": "oye" has no consonants, so only "oye" itself, and the
  // phrase never runs together ("molt" alone would be it).
  const std::vector<std::string> es = {"oye molt", "hey molt"};
  EXPECT_EQ(WakePhraseWordsAtStart("Oye, malt, pausa", es), 2u);
  EXPECT_EQ(WakePhraseWordsAtStart("Hey Molt pausa", es), 2u);
  EXPECT_EQ(WakePhraseWordsAtStart("Oyemolt pausa", es), 0u);
  EXPECT_EQ(WakePhraseWordsAtStart("molt pausa", es), 0u);
  EXPECT_EQ(WakePhraseWordsAtStart("hoy malt", {"oye molt"}), 0u);
  // Few consonants: as written only.
  EXPECT_EQ(WakePhraseWordsAtStart("hey you there", {"hey you"}), 2u);
  EXPECT_EQ(WakePhraseWordsAtStart("hi you there", {"hey you"}), 0u);
  // Bytes from 0x80 are letters; nothing else is read past the phrase.
  EXPECT_EQ(WakePhraseWordsAtStart("hola m\xC3\xB3lt abre",
                                   {"hola m\xC3\xB3lt"}),
            2u);
  EXPECT_EQ(WakePhraseWordsAtStart(std::string(100000, 'a'), en), 0u);
}

TEST(SpeechTextTest, FindsTheWakeNameAfterALostStart) {
  const std::vector<std::string> en = {"hey molt"};
  // What the recognizer wrote when the phrase's start was lost.
  EXPECT_EQ(WakeNameWordsAtStart("Emold. Click", en), 1u);
  EXPECT_EQ(WakeNameWordsAtStart("Malt zoom in", en), 1u);
  EXPECT_EQ(WakeNameWordsAtStart("A mold. Read the title", en), 2u);
  EXPECT_EQ(WakeNameWordsAtStart("pain molt close tech", en), 2u);
  // Not the name, or past the second word.
  EXPECT_EQ(WakeNameWordsAtStart("Walt. Switch to tab two", en), 0u);
  EXPECT_EQ(WakeNameWordsAtStart("click the molt button", en), 0u);
  EXPECT_EQ(WakeNameWordsAtStart("", en), 0u);
  // Spanish's name is "molt" too.
  EXPECT_EQ(WakeNameWordsAtStart("Oi, malt, pausa", {"oye molt"}), 2u);
  // A name with fewer than three consonants is only taken as written.
  EXPECT_EQ(WakeNameWordsAtStart("hey you there", {"hey you"}), 2u);
  EXPECT_EQ(WakeNameWordsAtStart("hey yo there", {"hey you"}), 0u);
}

// What the recognizer had written at the pause after "Hey Molt": only the
// phrase begun, its last word perhaps cut short.
TEST(SpeechTextTest, KnowsTheWakePhraseBegun) {
  const std::vector<std::string> en = {"hey molt"};
  for (const char* text : {"Hey", "Hey,", "Hey, Mal", "Hey mol", "Hey, Malt",
                           "Hey molt.", "He mo"}) {
    SCOPED_TRACE(text);
    EXPECT_TRUE(OnlyAWakePhraseBegun(text, en));
  }
  // A word after the phrase, a word that doesn't begin it, or nothing.
  for (const char* text : {"Hey Molt pause", "Hey, Malt, said", "Hey there",
                           "help", "scroll down", "", "  "}) {
    SCOPED_TRACE(text);
    EXPECT_FALSE(OnlyAWakePhraseBegun(text, en));
  }
  EXPECT_TRUE(OnlyAWakePhraseBegun("Oye, mo", {"oye molt"}));
}

}  // namespace
}  // namespace molt_speech
