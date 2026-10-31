// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationStorage's own directory (a profile's voice routines) and the
// one-time move out of the shared storage (Take()).

#include "chrome/browser/molt_ai/automation/automation_storage.h"

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace {

Script Named(const std::string& id, const std::string& name) {
  Script script;
  script.id = id;
  script.name = name;
  Step step;
  step.type = StepType::NAVIGATE;
  step.target = "https://news.example/";
  script.steps.push_back(std::move(step));
  return script;
}

TEST(AutomationStorageTest, ADirectoryOfItsOwn) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const base::FilePath shared_root = temp.GetPath().AppendASCII("home");
  const base::FilePath own = temp.GetPath().AppendASCII("Profile 1");
  AutomationStorage shared;
  shared.SetRootForTesting(shared_root);
  AutomationStorage mine(own);
  ASSERT_TRUE(mine.Save(Named("voice_1", "my news")));
  // In the directory itself, not under "automations".
  EXPECT_TRUE(base::PathExists(own.AppendASCII("voice_1.molt")));
  EXPECT_TRUE(base::PathExists(own.AppendASCII("audit.log")));
  EXPECT_EQ(mine.ListIds(), std::vector<std::string>{"voice_1"});
  // The shared storage doesn't see it, nor it the shared one's.
  EXPECT_TRUE(shared.ListIds().empty());
  ASSERT_TRUE(shared.Save(Named("wf_1", "Prices")));
  EXPECT_EQ(mine.ListIds(), std::vector<std::string>{"voice_1"});
  EXPECT_FALSE(mine.Load("wf_1"));
  EXPECT_FALSE(shared.Load("voice_1"));
}

TEST(AutomationStorageTest, TakenOnce) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  AutomationStorage shared;
  shared.SetRootForTesting(temp.GetPath());
  ASSERT_TRUE(shared.Save(Named("voice_7", "my news")));

  std::optional<Script> taken = shared.Take("voice_7");
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->id, "voice_7");
  EXPECT_EQ(taken->name, "my news");
  // Gone from the listing at once; a second taker gets nothing.
  EXPECT_TRUE(shared.ListIds().empty());
  EXPECT_FALSE(shared.Load("voice_7"));
  EXPECT_FALSE(shared.Take("voice_7"));
  // Put back, it is there again, and can be taken again.
  ASSERT_TRUE(shared.ReturnTaken("voice_7"));
  EXPECT_EQ(shared.ListIds(), std::vector<std::string>{"voice_7"});
  ASSERT_TRUE(shared.Take("voice_7"));
  shared.FinishTake("voice_7");
  EXPECT_TRUE(shared.ListIds().empty());
  EXPECT_FALSE(shared.ReturnTaken("voice_7"));
  EXPECT_FALSE(shared.Take("voice_7"));
}

TEST(AutomationStorageTest, TakeRefusesWhatItCantRead) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  AutomationStorage shared;
  shared.SetRootForTesting(temp.GetPath());
  ASSERT_TRUE(shared.EnsureDirectory());
  const base::FilePath dir = temp.GetPath().AppendASCII("automations");
  ASSERT_TRUE(base::WriteFile(dir.AppendASCII("broken.molt"), "{not json"));
  EXPECT_FALSE(shared.Take("broken"));
  // Left as it was.
  EXPECT_TRUE(base::PathExists(dir.AppendASCII("broken.molt")));
  EXPECT_FALSE(base::PathExists(dir.AppendASCII("broken.molt.taken")));
  // Nor ids that aren't file names.
  EXPECT_FALSE(shared.Take("../broken"));
  EXPECT_FALSE(shared.Take(""));
  // A file whose id says another is known by its file's name.
  Script other = Named("someone_else", "theirs");
  ASSERT_TRUE(
      base::WriteFile(dir.AppendASCII("mine.molt"), other.ToJSONString()));
  std::optional<Script> taken = shared.Take("mine");
  ASSERT_TRUE(taken);
  EXPECT_EQ(taken->id, "mine");
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
