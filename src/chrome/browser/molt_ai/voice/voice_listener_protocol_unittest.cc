// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_listener_protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

std::vector<uint8_t> Bytes(std::string_view text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

// ---- Frames ----------------------------------------------------------------

TEST(VoiceListenerProtocolTest, FrameHeaderIsLittleEndianTypeAndLength) {
  const std::string frame = EncodeHelperFrame(HelperFrameType::kReset, "");
  EXPECT_EQ(Bytes(frame), (std::vector<uint8_t>{3, 0, 0, 0, 0, 0, 0, 0}));
  const std::string config = EncodeHelperFrame(HelperFrameType::kConfig, "{}");
  EXPECT_EQ(Bytes(config),
            (std::vector<uint8_t>{2, 0, 0, 0, 2, 0, 0, 0, '{', '}'}));
  const std::string big(300, 'x');
  const std::string frame300 = EncodeHelperFrame(HelperFrameType::kConfig, big);
  ASSERT_EQ(frame300.size(), 308u);
  EXPECT_EQ(static_cast<uint8_t>(frame300[4]), 300 & 0xff);
  EXPECT_EQ(static_cast<uint8_t>(frame300[5]), 300 >> 8);
}

TEST(VoiceListenerProtocolTest, FramePayloadCap) {
  EXPECT_EQ(EncodeHelperFrame(HelperFrameType::kConfig,
                              std::string(kMaxFramePayloadBytes, 'a'))
                .size(),
            kFrameHeaderBytes + kMaxFramePayloadBytes);
  EXPECT_TRUE(EncodeHelperFrame(HelperFrameType::kConfig,
                                std::string(kMaxFramePayloadBytes + 1, 'a'))
                  .empty());
}

TEST(VoiceListenerProtocolTest, AudioFrameSamplesAreLittleEndian) {
  const std::vector<int16_t> samples = {1, -1, 0x1234, -32768};
  const std::string frame = EncodeAudioFrame(samples);
  EXPECT_EQ(Bytes(frame),
            (std::vector<uint8_t>{1, 0, 0, 0, 8, 0, 0, 0, 0x01, 0x00, 0xff,
                                  0xff, 0x34, 0x12, 0x00, 0x80}));
  const std::vector<int16_t> too_many(kMaxFramePayloadBytes / 2 + 1);
  EXPECT_TRUE(EncodeAudioFrame(too_many).empty());
  const std::vector<int16_t> most(kMaxFramePayloadBytes / 2);
  EXPECT_EQ(EncodeAudioFrame(most).size(),
            kFrameHeaderBytes + kMaxFramePayloadBytes);
}

TEST(VoiceListenerProtocolTest, ConfigJson) {
  HelperConfig config;
  config.wake_phrases = {"Hey Molt"};
  config.wake_required = true;
  config.hotwords = {"Bohemian Rhapsody", "Queen"};
  config.vad_silence_ms = 700;
  EXPECT_EQ(EncodeConfigJson(config),
            "{\"wake_phrases\":[\"hey molt\"],\"wake_required\":true,"
            "\"hotwords\":[\"bohemian rhapsody\",\"queen\"],"
            "\"vad_silence_ms\":700}");
}

TEST(VoiceListenerProtocolTest, ConfigReducesClampsAndCaps) {
  HelperConfig config;
  config.wake_phrases = {"\"}],\"wake_required\":false", "", "!!!"};
  config.wake_required = false;
  for (int i = 0; i < 100; ++i) {
    config.hotwords.push_back("word" + std::to_string(i));
  }
  config.hotwords.insert(config.hotwords.begin(), "a:5/b#c@d");
  config.vad_silence_ms = 99999;
  const std::string json = EncodeConfigJson(config);
  // Nothing of the phrases is syntax: quotes, brackets, colons, slashes.
  EXPECT_EQ(json.find("\\\""), std::string::npos);
  EXPECT_NE(json.find("\"wake_phrases\":[\"wake required false\"]"),
            std::string::npos);
  EXPECT_NE(json.find("\"wake_required\":false"), std::string::npos);
  EXPECT_NE(json.find("\"a five b c d\""), std::string::npos);
  EXPECT_NE(json.find("\"vad_silence_ms\":3000"), std::string::npos);
  // kMaxHotwords phrases: the first and words 0..62.
  EXPECT_NE(json.find("\"word62\""), std::string::npos);
  EXPECT_EQ(json.find("\"word63\""), std::string::npos);
  EXPECT_LT(json.size(), kMaxFramePayloadBytes);

  config.vad_silence_ms = -5;
  EXPECT_NE(EncodeConfigJson(config).find("\"vad_silence_ms\":200"),
            std::string::npos);
}

TEST(VoiceListenerProtocolTest, NumberAsWords) {
  EXPECT_EQ(NumberAsWords("0"), "zero");
  EXPECT_EQ(NumberAsWords("4"), "four");
  EXPECT_EQ(NumberAsWords("13"), "thirteen");
  EXPECT_EQ(NumberAsWords("40"), "forty");
  EXPECT_EQ(NumberAsWords("120"), "one hundred twenty");
  EXPECT_EQ(NumberAsWords("1985"), "nineteen eighty five");
  EXPECT_EQ(NumberAsWords("1900"), "nineteen hundred");
  EXPECT_EQ(NumberAsWords("1905"), "nineteen oh five");
  EXPECT_EQ(NumberAsWords("2022"), "twenty twenty two");
  EXPECT_EQ(NumberAsWords("2005"), "two thousand five");
  EXPECT_EQ(NumberAsWords("1000"), "one thousand");
  EXPECT_EQ(NumberAsWords("007"), "seven");
  EXPECT_EQ(NumberAsWords("2100000"), "two million one hundred thousand");
  EXPECT_EQ(NumberAsWords("999999999"),
            "nine hundred ninety nine million nine hundred ninety nine "
            "thousand nine hundred ninety nine");
  EXPECT_EQ(NumberAsWords("1234567890"), "");
  EXPECT_EQ(NumberAsWords(""), "");
  EXPECT_EQ(NumberAsWords("12a"), "");
}

TEST(VoiceListenerProtocolTest, ReduceSpeechPhrase) {
  EXPECT_EQ(ReduceSpeechPhrase("Bohemian Rhapsody"), "bohemian rhapsody");
  EXPECT_EQ(ReduceSpeechPhrase("  Beyoncé  —  Halo "), "beyonce halo");
  EXPECT_EQ(ReduceSpeechPhrase("Don\xE2\x80\x99t Stop Me Now"),
            "don't stop me now");
  EXPECT_EQ(ReduceSpeechPhrase("rock 'n' roll"), "rock n roll");
  EXPECT_EQ(ReduceSpeechPhrase("AC/DC"), "ac dc");
  EXPECT_EQ(ReduceSpeechPhrase("Live Aid 1985"),
            "live aid nineteen eighty five");
  EXPECT_EQ(ReduceSpeechPhrase("the 4th of July"), "the fourth of july");
  EXPECT_EQ(ReduceSpeechPhrase("21st 2nd 3rd 12th 20th"),
            "twenty first second third twelfth twentieth");
  EXPECT_EQ(ReduceSpeechPhrase("a:5/b#c@d:e"), "a five b c d e");
  EXPECT_EQ(ReduceSpeechPhrase("Straße Łódź"), "strasse lodz");
  // Scripts the model can't spell, and numbers too long to read, go.
  EXPECT_EQ(ReduceSpeechPhrase("Привет мир"), "");
  EXPECT_EQ(ReduceSpeechPhrase("call 0123456789012 now"), "call now");
  // Invisible characters don't break a word; controls do.
  EXPECT_EQ(ReduceSpeechPhrase("you\xE2\x80\x8Btube"), "youtube");
  EXPECT_EQ(ReduceSpeechPhrase("a\nb\tc"), "a b c");
  // Caps: whole words, at most kMaxPhraseWords and kMaxPhraseBytes.
  EXPECT_EQ(ReduceSpeechPhrase("one two three four five six seven eight nine"),
            "one two three four five six seven eight");
  const std::string long_words =
      std::string(40, 'a') + " " + std::string(30, 'b') + " c";
  EXPECT_EQ(ReduceSpeechPhrase(long_words), std::string(40, 'a'));
  EXPECT_EQ(ReduceSpeechPhrase(std::string(65, 'z')), "");
  EXPECT_EQ(ReduceSpeechPhrase("\xff\xfe broken \xc3"), "broken");
}

PageListEntry Entry(std::string title, std::string channel) {
  PageListEntry entry;
  entry.title = std::move(title);
  entry.channel = std::move(channel);
  return entry;
}

TEST(VoiceListenerProtocolTest, HotwordsFromPageMap) {
  PageMap map;
  PageList results;
  results.entries.push_back(
      Entry("Queen \xE2\x80\x93 Bohemian Rhapsody (Official Video Remastered)",
            "Queen Official"));
  results.entries.push_back(
      Entry("Bohemian Rhapsody | Live Aid 1985", "Queen Official"));
  results.entries.push_back(
      Entry("Panic! At The Disco: Bohemian Rhapsody", "Panic! At The Disco"));
  results.entries.push_back(Entry("Hey-ya - OutKast", ""));
  map.lists.push_back(results);
  PageList navigation;
  navigation.entries.push_back(Entry("Home", ""));
  map.lists.push_back(navigation);

  EXPECT_EQ(
      HotwordsFromPageMap(map),
      (std::vector<std::string>{
          "queen", "bohemian rhapsody", "official video remastered",
          "queen official", "live aid nineteen eighty five", "panic",
          "at the disco", "panic at the disco", "hey ya", "outkast", "home"}));
  EXPECT_EQ(HotwordsFromPageMap(map, 3),
            (std::vector<std::string>{"queen", "bohemian rhapsody",
                                      "official video remastered"}));
  EXPECT_TRUE(HotwordsFromPageMap(PageMap()).empty());
}

// ---- Lines -----------------------------------------------------------------

ParsedHelperLine Parse(std::string_view line) {
  return ParseHelperLine(line);
}

TEST(VoiceListenerProtocolTest, ParsesEveryType) {
  ParsedHelperLine ready = Parse(
      R"({"type":"ready","engine":"sherpa-onnx 2.0.1","models":["kroko-en",)"
      R"("kws","silero-vad"],"protocol":1})");
  ASSERT_EQ(ready.error, HelperLineError::kNone);
  EXPECT_EQ(ready.event.type, HelperEventType::kReady);
  EXPECT_EQ(ready.event.engine, "sherpa-onnx 2.0.1");
  EXPECT_EQ(ready.event.models,
            (std::vector<std::string>{"kroko-en", "kws", "silero-vad"}));

  ParsedHelperLine ready_one_model =
      Parse(R"({"type":"ready","engine":"e","models":"kroko-en"})");
  ASSERT_EQ(ready_one_model.error, HelperLineError::kNone);
  EXPECT_EQ(ready_one_model.event.models,
            (std::vector<std::string>{"kroko-en"}));

  for (const auto& [line, type] :
       std::vector<std::pair<std::string_view, HelperEventType>>{
           {R"({"type":"wake"})", HelperEventType::kWake},
           {R"({"type":"speech_start"})", HelperEventType::kSpeechStart},
           {R"({"type":"speech_end"})", HelperEventType::kSpeechEnd}}) {
    ParsedHelperLine parsed = Parse(line);
    ASSERT_EQ(parsed.error, HelperLineError::kNone) << line;
    EXPECT_EQ(parsed.event.type, type);
  }

  ParsedHelperLine partial = Parse(R"({"type":"partial","text":"open you"})");
  ASSERT_EQ(partial.error, HelperLineError::kNone);
  EXPECT_EQ(partial.event.text, "open you");

  ParsedHelperLine final_line = Parse(
      " { \"type\" : \"final\" , \"text\" : \"open YouTube\" , "
      "\"confidence\" : 0.875, \"words\": null, \"ok\": true } \r");
  ASSERT_EQ(final_line.error, HelperLineError::kNone);
  EXPECT_EQ(final_line.event.type, HelperEventType::kFinal);
  EXPECT_EQ(final_line.event.text, "open YouTube");
  EXPECT_DOUBLE_EQ(final_line.event.confidence, 0.875);

  ParsedHelperLine error = Parse(
      R"({"type":"error","code":"bad_frame","message":"unknown type 9"})");
  ASSERT_EQ(error.error, HelperLineError::kNone);
  EXPECT_EQ(error.event.code, "bad_frame");
  EXPECT_EQ(error.event.message, "unknown type 9");
}

TEST(VoiceListenerProtocolTest, ConfidenceForms) {
  for (std::string_view value :
       {"0", "1", "0.5", "1.0", "5e-1", "1E0", "0.000", "1e-400", "-0"}) {
    const std::string line =
        std::string(R"({"type":"final","text":"x","confidence":)") +
        std::string(value) + "}";
    EXPECT_EQ(Parse(line).error, HelperLineError::kNone) << value;
  }
  EXPECT_DOUBLE_EQ(Parse(R"({"type":"final","text":"x","confidence":2.5e-1})")
                       .event.confidence,
                   0.25);
  for (std::string_view value :
       {"1.5", "-0.1", "\"0.5\"", "true", "null", "[0.5]", "1e400", "2"}) {
    const std::string line =
        std::string(R"({"type":"final","text":"x","confidence":)") +
        std::string(value) + "}";
    const HelperLineError error = Parse(line).error;
    EXPECT_TRUE(error == HelperLineError::kBadField) << value;
  }
  for (std::string_view value :
       {"01", ".5", "1.", "+1", "0x1", "1e", "NaN", "Infinity", "--1"}) {
    const std::string line =
        std::string(R"({"type":"final","text":"x","confidence":)") +
        std::string(value) + "}";
    EXPECT_EQ(Parse(line).error, HelperLineError::kNotJson) << value;
  }
}

TEST(VoiceListenerProtocolTest, RefusesWhatIsNotTheProtocol) {
  struct Case {
    std::string_view line;
    HelperLineError error;
  };
  const Case cases[] = {
      {"", HelperLineError::kNotJson},
      {"hello", HelperLineError::kNotJson},
      {"[1]", HelperLineError::kNotObject},
      {"\"wake\"", HelperLineError::kNotObject},
      {R"({"type":"wake"} x)", HelperLineError::kNotJson},
      {R"({"type":"wake"}{"type":"wake"})", HelperLineError::kNotJson},
      {R"({"type":"wake",})", HelperLineError::kNotJson},
      {R"({'type':'wake'})", HelperLineError::kNotJson},
      {R"({"type":"wake","type":"final"})", HelperLineError::kDuplicateKey},
      {R"({"type":"command","text":"rm"})", HelperLineError::kUnknownType},
      {R"({"type":"WAKE"})", HelperLineError::kUnknownType},
      {R"({"text":"open"})", HelperLineError::kMissingField},
      {R"({"type":7})", HelperLineError::kBadField},
      {R"({"type":"partial"})", HelperLineError::kMissingField},
      {R"({"type":"partial","text":5})", HelperLineError::kBadField},
      {R"({"type":"final","text":"x"})", HelperLineError::kMissingField},
      {R"({"type":"ready"})", HelperLineError::kMissingField},
      {R"({"type":"ready","engine":"e","models":[1]})",
       HelperLineError::kBadField},
      {R"({"type":"ready","engine":"e","models":{"a":"b"}})",
       HelperLineError::kBadField},
      {R"({"type":"ready","engine":"e","protocol":2})",
       HelperLineError::kBadField},
      {R"({"type":"ready","engine":"e","models":["a","b","c","d","e","f","g","h","i"]})",
       HelperLineError::kFieldTooLong},
      {R"({"type":"error"})", HelperLineError::kMissingField},
      {R"({"type":"wake","x":{"y":1}})", HelperLineError::kBadField},
      {R"({"type":"wake","x":[[]]})", HelperLineError::kBadField},
      {"{\"type\":\"wake\",\"x\":\"a\tb\"}", HelperLineError::kNotJson},
      {R"({"type":"wake","x":"\q"})", HelperLineError::kNotJson},
      {R"({"type":"wake","x":"\u12"})", HelperLineError::kNotJson},
      {R"({"type":"partial","text":"\ud800"})", HelperLineError::kInvalidUtf8},
      {R"({"type":"partial","text":"\udc00x"})", HelperLineError::kInvalidUtf8},
      {R"({"type":"partial","text":"\ud800A"})", HelperLineError::kInvalidUtf8},
      {"{\"type\":\"partial\",\"text\":\"\xc3\x28\"}",
       HelperLineError::kInvalidUtf8},
      {"{\"type\":\"partial\",\"text\":\"\xed\xa0\x80\"}",
       HelperLineError::kInvalidUtf8},
      {R"({"type":"wake")", HelperLineError::kNotJson},
      {R"({"type":"wake)", HelperLineError::kNotJson},
  };
  for (const Case& test : cases) {
    EXPECT_EQ(Parse(test.line).error, test.error) << test.line;
  }
}

TEST(VoiceListenerProtocolTest, TextIsMadeSafeAndCapped) {
  // An escaped NUL, bidi override and zero-width space are dropped; line
  // breaks become spaces.
  ParsedHelperLine parsed = Parse(
      R"({"type":"final","text":"  open\u0000 ‮evil​\nyou tube ","confidence":1})");
  ASSERT_EQ(parsed.error, HelperLineError::kNone);
  EXPECT_EQ(parsed.event.text, "open evil you tube");

  ParsedHelperLine pair = Parse(R"({"type":"partial","text":"😀 ok"})");
  ASSERT_EQ(pair.error, HelperLineError::kNone);
  EXPECT_EQ(pair.event.text, "\xF0\x9F\x98\x80 ok");

  const std::string at_cap(kMaxHelperTextBytes, 'a');
  EXPECT_EQ(Parse(R"({"type":"partial","text":")" + at_cap + "\"}").error,
            HelperLineError::kNone);
  const std::string over_cap(kMaxHelperTextBytes + 1, 'a');
  EXPECT_EQ(Parse(R"({"type":"partial","text":")" + over_cap + "\"}").error,
            HelperLineError::kFieldTooLong);
  // The cap counts the text after its escapes are read: 600 escapes of a
  // two-byte letter fit the line but not the field.
  std::string escaped;
  for (size_t i = 0; i < 600; ++i) {
    escaped += "\\u00e9";
  }
  EXPECT_EQ(Parse(R"({"type":"partial","text":")" + escaped + "\"}").error,
            HelperLineError::kFieldTooLong);

  const std::string long_code(kMaxHelperNameBytes + 1, 'c');
  EXPECT_EQ(Parse(R"({"type":"error","code":")" + long_code + "\"}").error,
            HelperLineError::kFieldTooLong);
}

TEST(VoiceListenerProtocolTest, LineCap) {
  std::string line = R"({"type":"wake","pad":")";
  line += std::string(kMaxHelperLineBytes - line.size() - 2, 'p');
  line += "\"}";
  ASSERT_EQ(line.size(), kMaxHelperLineBytes);
  EXPECT_EQ(Parse(line).error, HelperLineError::kNone);
  EXPECT_EQ(Parse(line + "\r").error, HelperLineError::kNone);
  line.insert(line.size() - 2, "p");
  EXPECT_EQ(Parse(line).error, HelperLineError::kTooLong);
}

TEST(VoiceListenerProtocolTest, LineReaderSplitsAcrossReads) {
  HelperLineReader reader;
  std::vector<std::string> lines;
  EXPECT_TRUE(reader.Feed("{\"type\":\"wa", &lines));
  EXPECT_TRUE(lines.empty());
  EXPECT_TRUE(reader.Feed("ke\"}\r\n{\"type\":\"speech_start\"}\n\n{", &lines));
  EXPECT_EQ(lines,
            (std::vector<std::string>{"{\"type\":\"wake\"}",
                                      "{\"type\":\"speech_start\"}", ""}));
  lines.clear();
  EXPECT_TRUE(reader.Feed("}\n", &lines));
  EXPECT_EQ(lines, (std::vector<std::string>{"{}"}));
  EXPECT_FALSE(reader.failed());
}

TEST(VoiceListenerProtocolTest, LineReaderRefusesLongLines) {
  {
    HelperLineReader reader;
    std::vector<std::string> lines;
    EXPECT_TRUE(reader.Feed(std::string(kMaxHelperLineBytes, 'a'), &lines));
    EXPECT_TRUE(reader.Feed("\r\n", &lines));
    EXPECT_EQ(lines.size(), 1u);
  }
  {
    HelperLineReader reader;
    std::vector<std::string> lines;
    EXPECT_TRUE(reader.Feed("ok\n", &lines));
    EXPECT_TRUE(reader.Feed(std::string(kMaxHelperLineBytes, 'a'), &lines));
    EXPECT_FALSE(reader.Feed("a", &lines) && reader.Feed("\n", &lines));
    EXPECT_TRUE(reader.failed());
    EXPECT_FALSE(reader.Feed("{}\n", &lines));
    EXPECT_EQ(lines, (std::vector<std::string>{"ok"}));
  }
  {
    // A line of the cap plus a '\r' that isn't followed by '\n'.
    HelperLineReader reader;
    std::vector<std::string> lines;
    EXPECT_TRUE(
        reader.Feed(std::string(kMaxHelperLineBytes, 'a') + "\r", &lines));
    EXPECT_FALSE(reader.Feed("b\n", &lines));
  }
  {
    // One read far over the cap.
    HelperLineReader reader;
    std::vector<std::string> lines;
    EXPECT_FALSE(reader.Feed(std::string(1 << 20, 'a'), &lines));
  }
}

TEST(VoiceListenerProtocolTest, LineBudget) {
  HelperLineBudget budget;
  int taken = 0;
  while (budget.Take(1000) && taken < 1000) {
    ++taken;
  }
  EXPECT_EQ(taken, HelperLineBudget::kLineBurst);
  EXPECT_FALSE(budget.Take(1000));
  // A second later, kLinesPerSecond more.
  taken = 0;
  while (budget.Take(2000) && taken < 1000) {
    ++taken;
  }
  EXPECT_EQ(taken, HelperLineBudget::kLinesPerSecond);
  // A clock that goes back gives nothing.
  EXPECT_FALSE(budget.Take(500));
  // A steady few lines a second never run out.
  HelperLineBudget steady;
  for (int64_t ms = 0; ms < 600000; ms += 100) {
    ASSERT_TRUE(steady.Take(ms));
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
