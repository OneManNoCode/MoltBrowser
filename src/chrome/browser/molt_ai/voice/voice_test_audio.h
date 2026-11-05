// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// With --molt-voice-test-audio=<absolute path> (a test switch, as
// --molt-voice-test-log is: nothing without it, and the browser never sets
// it), what voice mode's microphone gave after the browser's processing, as
// voice mode's speech helper gets it: 16 kHz mono 16-bit little-endian
// samples, raw, appended. The spoken test kit (tools/voice_e2e,
// docs/VOICE_E2E.md) reads it to tell what reached the recognizer: a quiet
// microphone from audio the recognizer didn't make out. A regular file only,
// never through a link, readable by its owner alone (molt_regular_file.h),
// at most kMaxVoiceTestAudioBytes.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_AUDIO_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_AUDIO_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace base {
class SequencedTaskRunner;
}

namespace molt_ai {
namespace voice {

inline constexpr char kVoiceTestAudioSwitch[] = "molt-voice-test-audio";
// About 35 minutes of audio.
inline constexpr size_t kMaxVoiceTestAudioBytes = 64 * 1024 * 1024;
// Written half a second at a time.
inline constexpr size_t kVoiceTestAudioFlushBytes = 16000;

class VoiceTestAudio : public base::RefCountedThreadSafe<VoiceTestAudio> {
 public:
  // The file the command line asks for: --molt-voice-test-audio with an
  // absolute path. Empty without it (or with a relative path).
  static base::FilePath PathFromCommandLine();
  // A writer for that file; null without the switch.
  static scoped_refptr<VoiceTestAudio> MaybeCreate();

  VoiceTestAudio(const VoiceTestAudio&) = delete;
  VoiceTestAudio& operator=(const VoiceTestAudio&) = delete;

  // Any thread (the capture thread): |samples| appended, written on the
  // file's own sequence once half a second has gathered.
  void Append(base::span<const int16_t> samples);

 private:
  friend class base::RefCountedThreadSafe<VoiceTestAudio>;

  explicit VoiceTestAudio(base::FilePath path);
  ~VoiceTestAudio();

  void Post(std::string bytes);

  const base::FilePath path_;
  const scoped_refptr<base::SequencedTaskRunner> task_runner_;
  base::Lock lock_;
  std::string pending_ GUARDED_BY(lock_);
  size_t taken_ GUARDED_BY(lock_) = 0;
};

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_TEST_AUDIO_H_
