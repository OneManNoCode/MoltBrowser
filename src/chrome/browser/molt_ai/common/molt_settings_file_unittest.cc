// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_settings_file.h"

#include <optional>
#include <string>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/run_loop.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

class MoltSettingsFileTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    path_ = dir_.GetPath()
                .Append(FILE_PATH_LITERAL(".moltbrowser"))
                .Append(FILE_PATH_LITERAL("settings.json"));
    SetMoltSettingsFilePathForTesting(path_);
  }
  void TearDown() override { SetMoltSettingsFilePathForTesting(std::nullopt); }

  void Write(const std::string& contents) {
    ASSERT_TRUE(base::CreateDirectory(path_.DirName()));
    ASSERT_TRUE(base::WriteFile(path_, contents));
  }
  std::string Contents() {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path_, &contents));
    return contents;
  }
  base::DictValue Parsed() {
    std::optional<base::Value> parsed =
        base::JSONReader::Read(Contents(), base::JSON_PARSE_RFC);
    EXPECT_TRUE(parsed && parsed->is_dict());
    return parsed && parsed->is_dict() ? std::move(parsed->GetDict())
                                       : base::DictValue();
  }
  // The files in settings.json's directory: no temporary file is left.
  int FilesInDir() {
    int count = 0;
    base::FileEnumerator files(path_.DirName(), /*recursive=*/false,
                               base::FileEnumerator::FILES);
    for (base::FilePath file = files.Next(); !file.empty();
         file = files.Next()) {
      ++count;
    }
    return count;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  base::FilePath path_;
};

TEST_F(MoltSettingsFileTest, MergeSetsAndRemovesKeys) {
  base::DictValue settings;
  settings.Set("max_tokens", 512);
  settings.Set("voice_mode", true);
  base::DictValue changes;
  changes.Set("voice_mode", false);
  changes.Set("web_ai_apis", true);
  changes.Set("max_tokens", base::Value());
  MergeMoltSettings(settings, changes);
  EXPECT_EQ(settings.FindBool("voice_mode"), false);
  EXPECT_EQ(settings.FindBool("web_ai_apis"), true);
  EXPECT_FALSE(settings.Find("max_tokens"));
  EXPECT_EQ(settings.size(), 2u);
}

TEST_F(MoltSettingsFileTest, AnUpdateKeepsEveryOtherKey) {
  Write("{\"max_tokens\": 512, \"system_prompt\": \"Be brief.\",}");
  base::DictValue changes;
  changes.Set("voice_speech_rate", 1.25);
  EXPECT_TRUE(UpdateMoltSettingsFileAt(path_, [&](base::DictValue& settings) {
    MergeMoltSettings(settings, changes);
    return true;
  }));
  const base::DictValue written = Parsed();
  EXPECT_EQ(written.FindInt("max_tokens"), 512);
  EXPECT_EQ(*written.FindString("system_prompt"), "Be brief.");
  EXPECT_EQ(written.FindDouble("voice_speech_rate"), 1.25);
  // Written in place of the old file: nothing else is left next to it.
  EXPECT_EQ(FilesInDir(), 1);
}

TEST_F(MoltSettingsFileTest, AMissingFileAndDirectoryAreCreated) {
  ASSERT_FALSE(base::PathExists(path_.DirName()));
  base::DictValue changes;
  changes.Set("web_ai_apis", false);
  EXPECT_TRUE(UpdateMoltSettingsFileAt(path_, [&](base::DictValue& settings) {
    EXPECT_TRUE(settings.empty());
    MergeMoltSettings(settings, changes);
    return true;
  }));
  EXPECT_EQ(Parsed().FindBool("web_ai_apis"), false);
}

TEST_F(MoltSettingsFileTest, AFileThatIsntAnObjectIsLeftAlone) {
  const std::string oversized =
      "{\"a\": \"" + std::string(kMaxMoltSettingsFileBytes, 'x') + "\"}";
  for (const std::string& contents :
       {std::string("[1, 2]"), std::string("{\"max_tokens\": 5"),
        std::string("\"text\""), oversized}) {
    Write(contents);
    bool called = false;
    EXPECT_FALSE(UpdateMoltSettingsFileAt(path_, [&](base::DictValue&) {
      called = true;
      return true;
    }));
    EXPECT_FALSE(called);
    EXPECT_EQ(Contents(), contents);
    base::DictValue read;
    EXPECT_EQ(ReadMoltSettingsFileAt(path_, &read),
              MoltSettingsRead::kNotAnObject);
    EXPECT_FALSE(ReadMoltSettings().has_value());
  }
}

// A change that would make the file too big to read back is refused, and
// the file is left as it was: the readers (the built-in AI APIs' switch
// among them) and later changes keep working.
TEST_F(MoltSettingsFileTest, AChangeTooBigToReadBackIsRefused) {
  Write("{\"web_ai_apis\": false}");
  const std::string before = Contents();
  base::DictValue changes;
  changes.Set("system_prompt", std::string(kMaxMoltSettingsFileBytes, 'x'));
  EXPECT_FALSE(UpdateMoltSettingsFileAt(path_, [&](base::DictValue& settings) {
    MergeMoltSettings(settings, changes);
    return true;
  }));
  EXPECT_EQ(Contents(), before);
  EXPECT_EQ(FilesInDir(), 1);
  base::DictValue read;
  ASSERT_EQ(ReadMoltSettingsFileAt(path_, &read), MoltSettingsRead::kObject);
  EXPECT_EQ(read.FindBool("web_ai_apis"), false);

  base::DictValue small;
  small.Set("system_prompt", "Be brief.");
  EXPECT_TRUE(UpdateMoltSettingsFileAt(path_, [&](base::DictValue& settings) {
    MergeMoltSettings(settings, small);
    return true;
  }));
  EXPECT_EQ(Parsed().FindBool("web_ai_apis"), false);
  EXPECT_EQ(*Parsed().FindString("system_prompt"), "Be brief.");
}

// molt://ai-settings' Save writes only its form's keys, with values of
// their kind: never the switches other sections change with a click.
TEST_F(MoltSettingsFileTest, AFormWritesOnlyItsOwnFields) {
  base::DictValue settings;
  settings.Set("voice_mode", false);
  settings.Set("voice_always_listening", false);
  settings.Set("web_ai_apis", false);
  settings.Set("max_tokens", 512);
  settings.Set("system_prompt", "Be brief.");
  base::DictValue fields;
  fields.Set("max_tokens", 512);
  fields.Set("temperature", 0.7);
  fields.Set("system_prompt", "default");
  fields.Set("default_model", "model");
  fields.Set("auto_load_model", true);
  base::DictValue form;
  form.Set("max_tokens", 256);
  form.Set("temperature", 1);
  form.Set("system_prompt", base::Value(base::ListValue()));
  form.Set("default_model", 5);
  form.Set("auto_load_model", false);
  form.Set("voice_mode", true);
  form.Set("voice_always_listening", true);
  form.Set("web_ai_apis", true);
  form.Set("anything_else", "x");
  MergeMoltSettingsForm(settings, form, fields);
  EXPECT_EQ(settings.FindInt("max_tokens"), 256);
  EXPECT_EQ(settings.FindDouble("temperature"), 1.0);
  EXPECT_EQ(settings.FindBool("auto_load_model"), false);
  // Not of their field's kind: left as they were.
  EXPECT_EQ(*settings.FindString("system_prompt"), "Be brief.");
  EXPECT_FALSE(settings.Find("default_model"));
  // Not the form's: left as they were.
  EXPECT_EQ(settings.FindBool("voice_mode"), false);
  EXPECT_EQ(settings.FindBool("voice_always_listening"), false);
  EXPECT_EQ(settings.FindBool("web_ai_apis"), false);
  EXPECT_FALSE(settings.Find("anything_else"));
  EXPECT_EQ(settings.size(), 7u);
}

TEST_F(MoltSettingsFileTest, AnUpdateMayDecline) {
  EXPECT_FALSE(
      UpdateMoltSettingsFileAt(path_, [](base::DictValue&) { return false; }));
  EXPECT_FALSE(base::PathExists(path_));
  base::DictValue read;
  EXPECT_EQ(ReadMoltSettingsFileAt(path_, &read), MoltSettingsRead::kMissing);
  EXPECT_EQ(ReadMoltSettings(), base::DictValue());
}

// Two places change settings.json at the same time (the voice loop's speech
// rate and molt://ai-settings' switch, say): both keys survive, however
// their read-modify-writes interleave.
TEST_F(MoltSettingsFileTest, ChangesFromTwoPlacesBothSurvive) {
  Write("{\"max_tokens\": 512}");
  constexpr int kChanges = 40;
  for (const char* prefix : {"voice_", "web_ai_"}) {
    base::ThreadPool::PostTask(
        FROM_HERE, base::BindOnce(
                       [](std::string prefix) {
                         for (int i = 0; i < kChanges; ++i) {
                           base::DictValue changes;
                           changes.Set(prefix + std::to_string(i), i);
                           PostSetMoltSettings(std::move(changes));
                         }
                       },
                       std::string(prefix)));
  }
  task_environment_.RunUntilIdle();
  const base::DictValue written = Parsed();
  EXPECT_EQ(written.FindInt("max_tokens"), 512);
  for (int i = 0; i < kChanges; ++i) {
    EXPECT_EQ(written.FindInt("voice_" + std::to_string(i)), i);
    EXPECT_EQ(written.FindInt("web_ai_" + std::to_string(i)), i);
  }
  EXPECT_EQ(FilesInDir(), 1);
}

TEST_F(MoltSettingsFileTest, APostedChangeReportsBack) {
  base::RunLoop run_loop;
  base::DictValue changes;
  changes.Set("voice_mode", true);
  std::optional<bool> result;
  PostSetMoltSettings(
      std::move(changes),
      base::BindOnce(
          [](std::optional<bool>* result, base::OnceClosure quit, bool ok) {
            *result = ok;
            std::move(quit).Run();
          },
          &result, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_EQ(result, true);
  // Read on the sequence, after the change.
  base::RunLoop read_loop;
  MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadMoltSettings),
      base::BindOnce(
          [](base::OnceClosure quit, std::optional<base::DictValue> read) {
            ASSERT_TRUE(read.has_value());
            EXPECT_EQ(read->FindBool("voice_mode"), true);
            std::move(quit).Run();
          },
          read_loop.QuitClosure()));
  read_loop.Run();

  // Over a file that isn't an object: false, and the file kept.
  Write("not json");
  base::RunLoop refused_loop;
  base::DictValue more;
  more.Set("voice_mode", false);
  PostSetMoltSettings(
      std::move(more),
      base::BindOnce(
          [](std::optional<bool>* result, base::OnceClosure quit, bool ok) {
            *result = ok;
            std::move(quit).Run();
          },
          &result, refused_loop.QuitClosure()));
  refused_loop.Run();
  EXPECT_EQ(result, false);
  EXPECT_EQ(Contents(), "not json");
}

}  // namespace
}  // namespace molt_ai
