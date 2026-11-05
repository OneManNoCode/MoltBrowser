// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_test_audio.h"

#include <algorithm>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/molt_ai/common/molt_regular_file.h"

namespace molt_ai {
namespace voice {

namespace {

void WriteBytes(const base::FilePath& path, const std::string& bytes) {
  // A regular file only, never through a link (molt_regular_file.h).
  if (!AppendToMoltFile(path, bytes)) {
    LOG(WARNING) << "[MoltVoice] the voice test audio can't be written";
  }
}

}  // namespace

// static
base::FilePath VoiceTestAudio::PathFromCommandLine() {
  if (!base::CommandLine::InitializedForCurrentProcess()) {
    return base::FilePath();
  }
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(kVoiceTestAudioSwitch)) {
    return base::FilePath();
  }
  const base::FilePath path =
      command_line->GetSwitchValuePath(kVoiceTestAudioSwitch);
  return path.IsAbsolute() ? path : base::FilePath();
}

// static
scoped_refptr<VoiceTestAudio> VoiceTestAudio::MaybeCreate() {
  base::FilePath path = PathFromCommandLine();
  if (path.empty()) {
    return nullptr;
  }
  return base::WrapRefCounted(new VoiceTestAudio(std::move(path)));
}

VoiceTestAudio::VoiceTestAudio(base::FilePath path)
    : path_(std::move(path)),
      task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {}

VoiceTestAudio::~VoiceTestAudio() {
  std::string rest;
  {
    base::AutoLock lock(lock_);
    rest.swap(pending_);
  }
  if (!rest.empty()) {
    Post(std::move(rest));
  }
}

void VoiceTestAudio::Append(base::span<const int16_t> samples) {
  std::string full;
  {
    base::AutoLock lock(lock_);
    const size_t room = (kMaxVoiceTestAudioBytes - taken_) / 2;
    const size_t count = std::min(samples.size(), room);
    for (const int16_t sample : samples.first(count)) {
      const uint16_t bits = static_cast<uint16_t>(sample);
      pending_.push_back(static_cast<char>(bits & 0xff));
      pending_.push_back(static_cast<char>(bits >> 8));
    }
    taken_ += count * 2;
    if (pending_.size() < kVoiceTestAudioFlushBytes) {
      return;
    }
    full.swap(pending_);
  }
  Post(std::move(full));
}

void VoiceTestAudio::Post(std::string bytes) {
  task_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&WriteBytes, path_, std::move(bytes)));
}

}  // namespace voice
}  // namespace molt_ai
