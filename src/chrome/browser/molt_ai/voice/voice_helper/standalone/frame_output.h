// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's stdout: frames are written by a thread of their own, so a
// browser that stops reading can't stall the helper's reading of its
// input (a CANCEL must get through). If a write takes longer than 2 s,
// stdout breaks, or more than kMaxQueuedBytes wait, the helper ends at once
// with status 3 and writes nothing more (docs/design/
// VOICE_HELPER_PROTOCOL.md §3.6). Built only by scripts/bundle-speech.sh
// (CMake).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_STANDALONE_FRAME_OUTPUT_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_STANDALONE_FRAME_OUTPUT_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace molt_voice {

inline constexpr int kStalledExitStatus = 3;
inline constexpr int64_t kMaxWriteMs = 2000;
// A unit's audio arrives at once: up to 30 s of 22,050 Hz (1.3 MB), plus
// the lookahead.
inline constexpr size_t kMaxQueuedBytes = 4 << 20;

class FrameOutput {
 public:
  FrameOutput();
  FrameOutput(const FrameOutput&) = delete;
  FrameOutput& operator=(const FrameOutput&) = delete;
  // Never runs: the helper leaves with _Exit().
  ~FrameOutput();

  void Start();
  void Push(std::string frame);
  // Waits until everything queued was written, or |timeout_ms| passed.
  void Drain(int timeout_ms);

 private:
  void WriterLoop();
  void WatchdogLoop();

  std::mutex mutex_;
  std::condition_variable wake_writer_;
  std::condition_variable drained_;
  std::deque<std::string> queue_;
  size_t queued_bytes_ = 0;
  bool writing_ = false;
  std::atomic<int64_t> write_started_ms_{0};
  std::thread writer_;
  std::thread watchdog_;
};

// Milliseconds on a monotonic clock.
int64_t NowMs();

}  // namespace molt_voice

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_STANDALONE_FRAME_OUTPUT_H_
