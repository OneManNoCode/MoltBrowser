// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_test_audio.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/scoped_command_line.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(VoiceTestAudioTest, NothingWithoutTheSwitchOrWithARelativePath) {
  base::test::ScopedCommandLine command_line;
  EXPECT_FALSE(VoiceTestAudio::MaybeCreate());
  command_line.GetProcessCommandLine()->AppendSwitchASCII(
      kVoiceTestAudioSwitch, "mic.pcm");
  EXPECT_FALSE(VoiceTestAudio::MaybeCreate());
}

// Every sample, little-endian, in order: half a second at a time, and the
// rest once the writer goes.
TEST(VoiceTestAudioTest, WritesWhatTheHelperGets) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("mic.pcm");
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchPath(kVoiceTestAudioSwitch,
                                                         path);
  scoped_refptr<VoiceTestAudio> audio = VoiceTestAudio::MaybeCreate();
  ASSERT_TRUE(audio);
  std::vector<int16_t> frame(160);
  int16_t next = -3000;
  const int kFrames = 60;  // 9600 samples: one write, then the rest
  for (int f = 0; f < kFrames; ++f) {
    for (int16_t& sample : frame) {
      sample = next++;
    }
    audio->Append(frame);
  }
  audio.reset();
  task_environment.RunUntilIdle();
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(path, &bytes));
  ASSERT_EQ(bytes.size(), static_cast<size_t>(kFrames) * 160 * 2);
  for (size_t i = 0; i < bytes.size() / 2; ++i) {
    const uint16_t bits = static_cast<uint8_t>(bytes[2 * i]) |
                          (static_cast<uint8_t>(bytes[2 * i + 1]) << 8);
    ASSERT_EQ(static_cast<int16_t>(bits), static_cast<int16_t>(-3000 + i))
        << i;
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
