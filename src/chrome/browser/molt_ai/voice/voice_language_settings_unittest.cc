// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_language_settings.h"

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

class VoiceLanguageSettingsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    path_ = dir_.GetPath()
                .Append(FILE_PATH_LITERAL(".moltbrowser"))
                .Append(FILE_PATH_LITERAL("settings.json"));
    SetMoltSettingsFilePathForTesting(path_);
  }
  void TearDown() override { SetMoltSettingsFilePathForTesting(std::nullopt); }

  // WriteVoiceLanguageSetting() on settings.json's own sequence.
  bool WriteOnSequence(const std::string& code) {
    base::RunLoop loop;
    bool written = false;
    MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
        FROM_HERE, base::BindOnce(&WriteVoiceLanguageSetting, code),
        base::BindOnce(
            [](bool* written, base::OnceClosure quit, bool ok) {
              *written = ok;
              std::move(quit).Run();
            },
            &written, loop.QuitClosure()));
    loop.Run();
    return written;
  }
  base::DictValue Parsed() {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path_, &contents));
    std::optional<base::Value> parsed =
        base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
    EXPECT_TRUE(parsed && parsed->is_dict());
    return parsed && parsed->is_dict() ? std::move(parsed->GetDict())
                                       : base::DictValue();
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  base::FilePath path_;
};

TEST_F(VoiceLanguageSettingsTest, MissingMeansTheBrowsersLanguage) {
  EXPECT_EQ(ReadVoiceLanguageSetting(), "");
}

TEST_F(VoiceLanguageSettingsTest, WrittenReadAndRemovedKeepingTheRest) {
  ASSERT_TRUE(base::CreateDirectory(path_.DirName()));
  ASSERT_TRUE(base::WriteFile(path_, R"({"voice_mode": true})"));
  EXPECT_TRUE(WriteOnSequence("es"));
  EXPECT_EQ(ReadVoiceLanguageSetting(), "es");
  EXPECT_EQ(Parsed().FindBool("voice_mode"), true);
  EXPECT_TRUE(WriteOnSequence("en"));
  EXPECT_EQ(ReadVoiceLanguageSetting(), "en");
  // "": the key goes, the rest stays.
  EXPECT_TRUE(WriteOnSequence(""));
  EXPECT_EQ(ReadVoiceLanguageSetting(), "");
  EXPECT_FALSE(Parsed().Find(kVoiceLanguageSettingsKey));
  EXPECT_EQ(Parsed().FindBool("voice_mode"), true);
}

TEST_F(VoiceLanguageSettingsTest, OnlyTheLanguagesVoiceModeHas) {
  EXPECT_FALSE(WriteOnSequence("fr"));
  EXPECT_FALSE(WriteOnSequence("ES"));
  EXPECT_FALSE(base::PathExists(path_));
  // A value written by hand that isn't one of them reads as missing.
  ASSERT_TRUE(base::CreateDirectory(path_.DirName()));
  ASSERT_TRUE(base::WriteFile(path_, R"({"voice_language": "de"})"));
  EXPECT_EQ(ReadVoiceLanguageSetting(), "");
  ASSERT_TRUE(base::WriteFile(path_, R"({"voice_language": 7})"));
  EXPECT_EQ(ReadVoiceLanguageSetting(), "");
  // A file that isn't an object is left alone.
  ASSERT_TRUE(base::WriteFile(path_, "not json"));
  EXPECT_FALSE(WriteOnSequence("es"));
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(path_, &contents));
  EXPECT_EQ(contents, "not json");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
