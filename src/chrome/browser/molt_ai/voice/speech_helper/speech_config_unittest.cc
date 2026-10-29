// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/speech_config.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_speech {
namespace {

ConfigStatus Parse(const std::string& json, ConfigUpdate* update = nullptr) {
  ConfigUpdate ignored;
  std::string error;
  const ConfigStatus status =
      ParseConfigFrame(json, update ? update : &ignored, &error);
  EXPECT_EQ(status == ConfigStatus::kOk, error.empty()) << json;
  // Errors never echo the input.
  EXPECT_EQ(error.find("Rhapsody"), std::string::npos);
  return status;
}

TEST(SpeechConfigTest, ReadsEveryField) {
  ConfigUpdate update;
  ASSERT_EQ(Parse(R"({"wake_phrases": ["Hey Molt", "hello  molt", "hey molt"],
                      "wake_required": false,
                      "hotwords": ["Bohemian Rhapsody", "Queen"],
                      "vad_silence_ms": 800})",
                  &update),
            ConfigStatus::kOk);
  EXPECT_TRUE(update.has_wake_phrases);
  // Normalized and de-duplicated.
  EXPECT_EQ(update.values.wake_phrases,
            (std::vector<std::string>{"hey molt", "hello molt"}));
  EXPECT_TRUE(update.has_wake_required);
  EXPECT_FALSE(update.values.wake_required);
  EXPECT_EQ(update.values.hotwords,
            (std::vector<std::string>{"Bohemian Rhapsody", "Queen"}));
  EXPECT_EQ(update.values.vad_silence_ms, 800);

  SpeechConfig config = DefaultSpeechConfig();
  ApplyConfigUpdate(update, &config);
  EXPECT_FALSE(config.wake_required);
  EXPECT_EQ(config.vad_silence_ms, 800);
}

TEST(SpeechConfigTest, KeepsWhatACONFIGLeavesOut) {
  SpeechConfig config = DefaultSpeechConfig();
  EXPECT_EQ(config.wake_phrases, std::vector<std::string>{"hey molt"});
  EXPECT_TRUE(config.wake_required);
  EXPECT_EQ(config.vad_silence_ms, 500);

  ConfigUpdate update;
  ASSERT_EQ(Parse(R"({"hotwords":[]})", &update), ConfigStatus::kOk);
  EXPECT_FALSE(update.has_wake_phrases);
  EXPECT_FALSE(update.has_wake_required);
  EXPECT_TRUE(update.has_hotwords);
  ApplyConfigUpdate(update, &config);
  EXPECT_EQ(config.wake_phrases, std::vector<std::string>{"hey molt"});
  EXPECT_TRUE(config.wake_required);
  EXPECT_EQ(Parse("{}"), ConfigStatus::kOk);
  EXPECT_EQ(Parse(" \t\r\n{ } \n"), ConfigStatus::kOk);
}

TEST(SpeechConfigTest, SkipsFieldsItDoesNotKnow) {
  ConfigUpdate update;
  EXPECT_EQ(Parse(R"({"future": {"a": [1, -2.5e3, true, null, "x\u00e9"]},
                      "wake_required": true, "z": "\ud83d\ude00"})",
                  &update),
            ConfigStatus::kOk);
  EXPECT_TRUE(update.has_wake_required);
  EXPECT_FALSE(update.has_hotwords);
}

TEST(SpeechConfigTest, RefusesWhatIsNotStrictJson) {
  const char* bad[] = {
      "",
      "[]",
      "\"text\"",
      "{",
      "{}x",
      "{} {}",
      "{'wake_required': true}",
      "{\"wake_required\": true,}",
      "{\"a\": 01}",
      "{\"a\": 1.}",
      "{\"a\": .5}",
      "{\"a\": NaN}",
      "{\"a\": tru}",
      "{\"a\" 1}",
      "{\"a\": [1 2]}",
      "{/* comment */}",
      "{\"a\": \"\\x41\"}",
      "{\"a\": \"\\ud800\"}",
      "{\"a\": \"\\udc00\"}",
      "{\"a\": \"tab\there\"}",
      "{\"a\": \"\xC0\xAF\"}",          // Overlong UTF-8.
      "{\"a\": \"\xED\xA0\x80\"}",      // A surrogate in UTF-8.
      "{\"a\": \"\xF4\x90\x80\x80\"}",  // Past U+10FFFF.
      "{\"a\": 1, \"a\": 2}",
      "{\"wake_required\": true, \"wake_required\": false}",
  };
  for (const char* json : bad) {
    EXPECT_EQ(Parse(json), ConfigStatus::kProtocolError) << json;
  }
}

TEST(SpeechConfigTest, BoundsNestingAndStrings) {
  std::string deep = "{\"a\":";
  for (int i = 0; i < 8; ++i) {
    deep += "[";
  }
  deep += "1";
  for (int i = 0; i < 8; ++i) {
    deep += "]";
  }
  deep += "}";
  EXPECT_EQ(Parse(deep), ConfigStatus::kProtocolError);
  std::string ok = "{\"a\":[[[[[[[1]]]]]]]}";
  EXPECT_EQ(Parse(ok), ConfigStatus::kOk);

  EXPECT_EQ(Parse("{\"hotwords\":[\"" + std::string(1024, 'a') + "\"]}"),
            ConfigStatus::kOk);
  EXPECT_EQ(Parse("{\"hotwords\":[\"" + std::string(1025, 'a') + "\"]}"),
            ConfigStatus::kProtocolError);
  // Escapes count after unescaping.
  std::string escaped;
  for (int i = 0; i < 1024; ++i) {
    escaped += "\\u0041";
  }
  EXPECT_EQ(Parse("{\"x\":\"" + escaped + "\"}"), ConfigStatus::kOk);
}

TEST(SpeechConfigTest, ChecksTheKnownFields) {
  const char* bad[] = {
      R"({"wake_required": 1})",
      R"({"wake_required": "true"})",
      R"({"wake_phrases": "hey molt"})",
      R"({"wake_phrases": []})",
      R"({"wake_phrases": [1]})",
      R"({"wake_phrases": ["hey molt 2"]})",
      R"({"wake_phrases": ["hey/molt"]})",
      R"({"wake_phrases": ["hey molt:9"]})",
      "{\"wake_phrases\": [\"h\xc3\xa9 molt\"]}",
      R"({"wake_phrases": ["one two three four five"]})",
      R"({"wake_phrases": ["' molt"]})",
      R"({"wake_phrases": [""]})",
      R"({"wake_phrases": ["   "]})",
      R"({"wake_phrases": ["a","b","c","d","e","f","g","h","i"]})",
      R"({"hotwords": "Queen"})",
      R"({"hotwords": [["Queen"]]})",
      R"({"hotwords": [null]})",
      R"({"vad_silence_ms": 99})",
      R"({"vad_silence_ms": 3001})",
      R"({"vad_silence_ms": 500.5})",
      R"({"vad_silence_ms": 5e2})",
      R"({"vad_silence_ms": "500"})",
      R"({"vad_silence_ms": 99999999999999999999999})",
      R"({"vad_silence_ms": -500})",
  };
  for (const char* json : bad) {
    EXPECT_EQ(Parse(json), ConfigStatus::kConfigError) << json;
  }
  std::string long_phrase = "{\"wake_phrases\":[\"" + std::string(65, 'a') +
                            "\"]}";
  EXPECT_EQ(Parse(long_phrase), ConfigStatus::kConfigError);

  std::string many = "{\"hotwords\":[";
  for (int i = 0; i < 1001; ++i) {
    many += i ? ",\"x\"" : "\"x\"";
  }
  many += "]}";
  EXPECT_EQ(Parse(many), ConfigStatus::kConfigError);

  EXPECT_EQ(Parse(R"({"vad_silence_ms": 100})"), ConfigStatus::kOk);
  EXPECT_EQ(Parse(R"({"vad_silence_ms": 3000})"), ConfigStatus::kOk);
  EXPECT_EQ(Parse(R"({"wake_phrases": ["o'malley molt"]})"),
            ConfigStatus::kOk);
}

TEST(SpeechConfigTest, NormalizesWakePhrases) {
  std::string out;
  EXPECT_TRUE(NormalizeWakePhrase("Hey  MOLT", &out));
  EXPECT_EQ(out, "hey molt");
  EXPECT_TRUE(NormalizeWakePhrase(" hey molt ", &out));
  EXPECT_EQ(out, "hey molt");
  EXPECT_TRUE(NormalizeWakePhrase("computer", &out));
  EXPECT_EQ(out, "computer");
  EXPECT_FALSE(NormalizeWakePhrase("hey\tmolt", &out));
  EXPECT_FALSE(NormalizeWakePhrase("a b c d e", &out));
  EXPECT_FALSE(NormalizeWakePhrase("''", &out));
  EXPECT_FALSE(NormalizeWakePhrase(std::string(65, 'a'), &out));
}

}  // namespace
}  // namespace molt_speech
