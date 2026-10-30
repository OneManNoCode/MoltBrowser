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
            "{\"wake_phrases\":[\"Hey Molt\"],\"wake_required\":true,"
            "\"hotwords\":[\"Bohemian Rhapsody\",\"Queen\"],"
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
  // The case of ASCII words is kept: the recognizer's vocabulary is cased.
  EXPECT_EQ(ReduceSpeechPhrase("Bohemian Rhapsody"), "Bohemian Rhapsody");
  EXPECT_EQ(ReduceSpeechPhrase("  Beyonc\xC3\xA9  \xE2\x80\x94  Halo "),
            "beyonce Halo");
  EXPECT_EQ(ReduceSpeechPhrase("Don\xE2\x80\x99t Stop Me Now"),
            "Don't Stop Me Now");
  EXPECT_EQ(ReduceSpeechPhrase("rock 'n' roll"), "rock n roll");
  EXPECT_EQ(ReduceSpeechPhrase("AC/DC"), "AC DC");
  EXPECT_EQ(ReduceSpeechPhrase("Live Aid 1985"),
            "Live Aid nineteen eighty five");
  EXPECT_EQ(ReduceSpeechPhrase("the 4th of July"), "the fourth of July");
  EXPECT_EQ(ReduceSpeechPhrase("21st 2nd 3rd 12th"),
            "twenty first second third twelfth");
  EXPECT_EQ(ReduceSpeechPhrase("a:5/b#c@d:e"), "a five b c d e");
  EXPECT_EQ(ReduceSpeechPhrase("Stra\xC3\x9F"
                               "e \xC5\x81\xC3\xB3"
                               "d\xC5\xBA"),
            "strasse lodz");
  // Scripts the model can't spell, and numbers too long to read, go.
  EXPECT_EQ(
      ReduceSpeechPhrase("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 "
                         "\xD0\xBC\xD0\xB8\xD1\x80"),
      "");
  EXPECT_EQ(ReduceSpeechPhrase("call 0123456789012 now"), "call now");
  // Invisible characters don't break a word; controls do.
  EXPECT_EQ(ReduceSpeechPhrase("you\xE2\x80\x8Btube"), "youtube");
  EXPECT_EQ(ReduceSpeechPhrase("a\nb\tc"), "a b c");
  // Caps: whole words, at most kMaxPhraseWords and kMaxPhraseBytes.
  EXPECT_EQ(ReduceSpeechPhrase("one two three four five six seven eight"),
            "one two three four five six");
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
  results.entries.push_back(Entry("Ms-Jackson - OutKast", ""));
  map.lists.push_back(results);
  PageList navigation;
  navigation.entries.push_back(Entry("Home", ""));
  map.lists.push_back(navigation);

  EXPECT_EQ(
      HotwordsFromPageMap(map),
      (std::vector<std::string>{
          "Queen", "Bohemian Rhapsody", "Official Video Remastered",
          "Queen Official", "Live Aid nineteen eighty five", "Panic",
          "At The Disco", "Panic At The Disco", "Ms Jackson", "OutKast",
          "Home"}));
  EXPECT_EQ(HotwordsFromPageMap(map, 3),
            (std::vector<std::string>{"Queen", "Bohemian Rhapsody",
                                      "Official Video Remastered"}));
  // The same words in another case are one hotword.
  PageMap shouting;
  PageList list;
  list.entries.push_back(Entry("QUEEN", "queen"));
  shouting.lists.push_back(list);
  EXPECT_EQ(HotwordsFromPageMap(shouting), (std::vector<std::string>{"QUEEN"}));
  EXPECT_TRUE(HotwordsFromPageMap(PageMap()).empty());
}

// The recognizer is biased toward the page's names, never toward a yes: a
// page can't tip what the user answers to "Say yes to ..." into one.
TEST(VoiceListenerProtocolTest, HotwordsHoldNoYes) {
  PageMap map;
  PageList list;
  list.entries.push_back(Entry("Yes", "OK"));
  list.entries.push_back(Entry("Sure, go ahead! | Yes please do", "Okay"));
  list.entries.push_back(Entry("Yesterday - Right Said Fred", "yeah"));
  // The grammar drops a wake phrase it hears first: "Hey ya" is a yes.
  list.entries.push_back(Entry("Hey-ya", ""));
  map.lists.push_back(list);
  EXPECT_EQ(HotwordsFromPageMap(map),
            (std::vector<std::string>{"Yesterday", "Right Said Fred"}));
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

// The helper's side of the example session in SPEECH_HELPER_PROTOCOL.md §5,
// as written there.
TEST(VoiceListenerProtocolTest, ParsesTheProtocolExample) {
  const std::vector<std::string_view> lines = {
      R"({"type":"ready","protocol":1,"engine":"sherpa-onnx 2.0.1, )"
      R"(onnxruntime 1.28.2","models":["kws-zipformer-gigaspeech-3.3m-)"
      R"(2024-01-01","silero-vad-v4","kroko-en-2025-08-06"],)"
      R"("sample_rate":16000})",
      R"({"type":"wake","epoch":0,"audio_ms":1940})",
      R"({"type":"speech_start","epoch":0,"audio_ms":2600})",
      R"({"type":"partial","epoch":0,"audio_ms":2600,"text":"Hey Molt, play"})",
      R"({"type":"partial","epoch":0,"audio_ms":3900,"text":"Hey Molt, play )"
      R"(Bohemian Rhapsody"})",
      R"({"type":"speech_end","epoch":0,"audio_ms":4500})",
      R"({"type":"final","epoch":0,"audio_ms":4500,"text":"Hey Molt, play )"
      R"(Bohemian Rhapsody.","confidence":0.91})"};
  const std::vector<HelperEventType> types = {
      HelperEventType::kReady,       HelperEventType::kWake,
      HelperEventType::kSpeechStart, HelperEventType::kPartial,
      HelperEventType::kPartial,     HelperEventType::kSpeechEnd,
      HelperEventType::kFinal};
  ASSERT_EQ(lines.size(), types.size());
  for (size_t i = 0; i < lines.size(); ++i) {
    ParsedHelperLine parsed = Parse(lines[i]);
    ASSERT_EQ(parsed.error, HelperLineError::kNone) << lines[i];
    EXPECT_EQ(parsed.event.type, types[i]) << lines[i];
  }
  EXPECT_EQ(Parse(lines[0]).event.models.size(), 3u);
  ParsedHelperLine final_line = Parse(lines.back());
  EXPECT_EQ(final_line.event.text, "Hey Molt, play Bohemian Rhapsody.");
  EXPECT_DOUBLE_EQ(final_line.event.confidence, 0.91);
  EXPECT_EQ(final_line.event.epoch, 0);
  EXPECT_EQ(final_line.event.audio_ms, 4500);
}

TEST(VoiceListenerProtocolTest, EpochAndAudioTime) {
  ParsedHelperLine wake = Parse(R"({"type":"wake","epoch":2,"audio_ms":1940})");
  ASSERT_EQ(wake.error, HelperLineError::kNone);
  EXPECT_EQ(wake.event.epoch, 2);
  EXPECT_EQ(wake.event.audio_ms, 1940);
  ParsedHelperLine bare = Parse(R"({"type":"speech_end"})");
  ASSERT_EQ(bare.error, HelperLineError::kNone);
  EXPECT_EQ(bare.event.epoch, -1);
  EXPECT_EQ(bare.event.audio_ms, -1);
  EXPECT_EQ(Parse(R"({"type":"final","epoch":9007199254740992,"text":"x",)"
                  R"("confidence":1})")
                .event.epoch,
            9007199254740992);
  for (std::string_view value :
       {"-1", "1.5", "\"2\"", "true", "1e300", "9007199254740993e3"}) {
    const std::string line =
        std::string(R"({"type":"wake","epoch":)") + std::string(value) + "}";
    EXPECT_EQ(Parse(line).error, HelperLineError::kBadField) << value;
  }
  // On ready and error they are other fields, left alone.
  EXPECT_EQ(Parse(R"({"type":"ready","engine":"e","epoch":"x"})").error,
            HelperLineError::kNone);
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

// ---- The order of events --------------------------------------------------

HelperEvent Event(HelperEventType type, int64_t epoch, int64_t audio_ms) {
  HelperEvent event;
  event.type = type;
  if (type != HelperEventType::kReady && type != HelperEventType::kError) {
    event.epoch = epoch;
    event.audio_ms = audio_ms;
  }
  return event;
}

constexpr HelperEventType kReady = HelperEventType::kReady;
constexpr HelperEventType kWake = HelperEventType::kWake;
constexpr HelperEventType kStart = HelperEventType::kSpeechStart;
constexpr HelperEventType kPartial = HelperEventType::kPartial;
constexpr HelperEventType kEnd = HelperEventType::kSpeechEnd;
constexpr HelperEventType kFinal = HelperEventType::kFinal;
constexpr HelperEventType kError = HelperEventType::kError;

// Whether the whole of |types| is accepted, at epoch 0 and rising times.
bool Accepts(const std::vector<HelperEventType>& types) {
  HelperEventSequence sequence;
  int64_t ms = 0;
  for (HelperEventType type : types) {
    if (!sequence.Accept(Event(type, 0, ms += 100), 0)) {
      return false;
    }
  }
  return true;
}

TEST(VoiceListenerProtocolTest, SequencesThatFollowTheProtocol) {
  // With the wake phrase, its command in the same breath or after a pause.
  EXPECT_TRUE(Accepts({kReady, kWake, kStart, kPartial, kPartial, kEnd, kFinal,
                       kWake, kStart, kEnd, kFinal}));
  // A wake phrase alone, no command within 6 s.
  EXPECT_TRUE(Accepts({kReady, kWake, kFinal, kWake, kFinal}));
  // Without it.
  EXPECT_TRUE(
      Accepts({kReady, kStart, kPartial, kEnd, kFinal, kStart, kEnd, kFinal}));
  // An error at any time.
  EXPECT_TRUE(Accepts({kError}));
  EXPECT_TRUE(Accepts({kReady, kWake, kStart, kError}));
  EXPECT_TRUE(Accepts({kReady}));
}

TEST(VoiceListenerProtocolTest, SequencesThatDont) {
  EXPECT_FALSE(Accepts({kWake}));             // before ready
  EXPECT_FALSE(Accepts({kReady, kReady}));    // ready twice
  EXPECT_FALSE(Accepts({kReady, kPartial}));  // outside an utterance
  EXPECT_FALSE(Accepts({kReady, kEnd}));
  EXPECT_FALSE(Accepts({kReady, kFinal}));  // nothing to answer
  EXPECT_FALSE(Accepts({kReady, kWake, kWake}));
  EXPECT_FALSE(Accepts({kReady, kStart, kWake}));
  EXPECT_FALSE(Accepts({kReady, kStart, kStart}));
  EXPECT_FALSE(Accepts({kReady, kStart, kFinal}));  // before speech_end
  EXPECT_FALSE(Accepts({kReady, kStart, kEnd, kPartial}));
  EXPECT_FALSE(Accepts({kReady, kStart, kEnd, kEnd}));
  EXPECT_FALSE(Accepts({kReady, kStart, kEnd, kStart}));  // final first
  EXPECT_FALSE(Accepts({kReady, kWake, kStart, kEnd, kFinal, kFinal}));
  // The helper exits after an error: nothing follows it, not another one.
  EXPECT_FALSE(Accepts({kError, kError}));
  EXPECT_FALSE(Accepts({kError, kReady}));
  EXPECT_FALSE(Accepts({kReady, kError, kError}));
  EXPECT_FALSE(Accepts({kReady, kError, kWake}));
  EXPECT_FALSE(Accepts({kReady, kWake, kStart, kError, kEnd}));
}

TEST(VoiceListenerProtocolTest, SequenceEpochsAndTimes) {
  HelperEventSequence sequence;
  ASSERT_TRUE(sequence.Accept(Event(kReady, 0, 0), 0));
  EXPECT_TRUE(sequence.ready());
  // Stamps are required.
  HelperEvent bare;
  bare.type = kWake;
  EXPECT_FALSE(HelperEventSequence(sequence).Accept(bare, 0));
  // A RESET the browser didn't send.
  EXPECT_FALSE(HelperEventSequence(sequence).Accept(Event(kWake, 1, 10), 0));

  ASSERT_TRUE(sequence.Accept(Event(kWake, 0, 100), 0));
  ASSERT_TRUE(sequence.Accept(Event(kStart, 0, 200), 0));
  // Time going back.
  EXPECT_FALSE(
      HelperEventSequence(sequence).Accept(Event(kPartial, 0, 150), 0));
  // The same time is fine (speech_end and final come together).
  ASSERT_TRUE(sequence.Accept(Event(kPartial, 0, 200), 0));
  // The browser sent a RESET; until the helper reads it, epoch 0 goes on.
  ASSERT_TRUE(sequence.Accept(Event(kPartial, 0, 300), 1));
  // Read: the utterance in progress was dropped, a new one may start.
  ASSERT_TRUE(sequence.Accept(Event(kStart, 1, 400), 1));
  ASSERT_TRUE(sequence.Accept(Event(kEnd, 1, 500), 1));
  ASSERT_TRUE(sequence.Accept(Event(kFinal, 1, 500), 1));
  // Epochs never go back.
  EXPECT_FALSE(HelperEventSequence(sequence).Accept(Event(kStart, 0, 600), 1));
  // A RESET also closes a wake: no final for it after.
  ASSERT_TRUE(sequence.Accept(Event(kWake, 1, 700), 2));
  EXPECT_FALSE(HelperEventSequence(sequence).Accept(Event(kFinal, 2, 800), 2));
  EXPECT_TRUE(sequence.Accept(Event(kWake, 2, 800), 2));
}

// ---- Failures in a row -----------------------------------------------------

constexpr int64_t kMinute = 60000;
constexpr int64_t kHour = 60 * kMinute;

TEST(VoiceListenerProtocolTest, FailuresInARowGiveUp) {
  HelperFailureCount count;
  count.Clear();
  // Crashes before it got ready, or soon after.
  count.OnStarted();
  EXPECT_EQ(count.OnEnded(1000), 1);
  EXPECT_FALSE(count.exhausted());
  count.OnStarted();
  count.OnReady(5000);
  EXPECT_EQ(count.OnEnded(5000 + kMinute - 1), 2);
  EXPECT_FALSE(count.exhausted());
  count.OnStarted();
  EXPECT_EQ(count.OnEnded(kHour), 3);
  EXPECT_TRUE(count.exhausted());
  // Voice mode turned on again: a fresh count.
  count.Clear();
  EXPECT_EQ(count.failures(), 0);
  EXPECT_FALSE(count.exhausted());
}

TEST(VoiceListenerProtocolTest, AStableRunStartsTheCountAfresh) {
  // Every end counts the same, whatever ended it (a limit, a protocol
  // error, a stall): a helper that ran a minute after its `ready` and then
  // ended, three times hours apart, never makes voice mode give up.
  HelperFailureCount count;
  count.Clear();
  int64_t now = 0;
  for (int run = 0; run < 10; ++run) {
    count.OnStarted();
    count.OnReady(now += 4000);
    EXPECT_EQ(count.OnEnded(now += 3 * kHour), 1) << run;
    EXPECT_FALSE(count.exhausted()) << run;
  }
  // Exactly kStableRunMs is a stable run.
  count.OnStarted();
  count.OnReady(now += 1000);
  EXPECT_EQ(count.OnEnded(now + HelperFailureCount::kStableRunMs), 1);
}

TEST(VoiceListenerProtocolTest, OnlyTheCurrentHelpersReadyCounts) {
  HelperFailureCount count;
  count.Clear();
  // Ready long ago, then ended: the next helper never got ready, so its
  // end isn't a stable run's.
  count.OnStarted();
  count.OnReady(0);
  EXPECT_EQ(count.OnEnded(kHour), 1);
  count.OnStarted();
  EXPECT_EQ(count.OnEnded(2 * kHour), 2);
  count.OnStarted();
  count.OnReady(2 * kHour + 4000);
  EXPECT_EQ(count.OnEnded(2 * kHour + 5000), 3);
  EXPECT_TRUE(count.exhausted());
}

// The helper's audio_ms, mapped back to when that audio was captured.
TEST(VoiceListenerProtocolTest, AudioClockMapsTheHelpersTime) {
  HelperAudioClock clock;
  EXPECT_FALSE(clock.CapturedAt(0).has_value());
  // Two 50 ms frames, captured back to back, then one after a gap (frames
  // dropped while the helper's input was backed up).
  clock.OnSent(800, 1050);
  clock.OnSent(800, 1100);
  clock.OnSent(800, 2000);
  EXPECT_EQ(clock.sent_samples(), 2400);
  EXPECT_EQ(clock.CapturedAt(0), 1000);
  EXPECT_EQ(clock.CapturedAt(25), 1025);
  EXPECT_EQ(clock.CapturedAt(50), 1050);
  EXPECT_EQ(clock.CapturedAt(75), 1075);
  EXPECT_EQ(clock.CapturedAt(100), 1100);
  EXPECT_EQ(clock.CapturedAt(125), 1975);
  EXPECT_EQ(clock.CapturedAt(150), 2000);
  // Past what was sent, or negative: nothing.
  EXPECT_FALSE(clock.CapturedAt(151).has_value());
  EXPECT_FALSE(clock.CapturedAt(-1).has_value());
  EXPECT_FALSE(clock.CapturedAt(int64_t{1} << 53).has_value());
  // The lag of an event that came at a time, never below 0.
  EXPECT_EQ(clock.LagMs(50, 1300), 250);
  EXPECT_EQ(clock.LagMs(150, 1900), 0);
  EXPECT_FALSE(clock.LagMs(151, 5000).has_value());
}

TEST(VoiceListenerProtocolTest, AudioClockRoundsTowardTheEarlier) {
  HelperAudioClock clock;
  // 815 samples: the helper says 50 ms (rounded down), never 51.
  clock.OnSent(815, 2000);
  EXPECT_FALSE(clock.CapturedAt(51).has_value());
  // 15 samples (0.9375 ms) before the end, taken as a whole millisecond.
  EXPECT_EQ(clock.CapturedAt(50), 1999);
  // Empty or oversized frames aren't audio the helper got.
  clock.OnSent(0, 3000);
  clock.OnSent(kMaxFramePayloadBytes / 2 + 1, 3000);
  EXPECT_EQ(clock.sent_samples(), 815);
}

TEST(VoiceListenerProtocolTest, AudioClockForgetsAndResets) {
  HelperAudioClock clock;
  for (size_t i = 0; i < HelperAudioClock::kMaxSpans + 10; ++i) {
    clock.OnSent(800, 1000 + 50 * static_cast<int64_t>(i + 1));
  }
  // The oldest frames are forgotten: their audio maps to nothing.
  EXPECT_FALSE(clock.CapturedAt(0).has_value());
  EXPECT_FALSE(clock.CapturedAt(499).has_value());
  EXPECT_EQ(clock.CapturedAt(500), 1500);
  const int64_t last_ms =
      clock.sent_samples() / HelperAudioClock::kSamplesPerMs;
  EXPECT_EQ(clock.CapturedAt(last_ms), 1000 + last_ms);
  // A new helper's clock starts again at 0.
  clock.Reset();
  EXPECT_EQ(clock.sent_samples(), 0);
  EXPECT_FALSE(clock.CapturedAt(0).has_value());
  clock.OnSent(800, 90050);
  EXPECT_EQ(clock.CapturedAt(0), 90000);
  EXPECT_FALSE(clock.CapturedAt(51).has_value());
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
