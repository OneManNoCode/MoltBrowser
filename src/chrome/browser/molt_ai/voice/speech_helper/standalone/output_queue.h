// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-speech's stdout: event lines are written by a thread of their own,
// so a browser that stops reading can't stall listening. If a line can't
// be written within 2 s, stdout breaks, or more than 1 MiB waits, the
// helper ends at once with status 3 and writes nothing more
// (docs/design/SPEECH_HELPER_PROTOCOL.md §3.8). Built only by
// scripts/bundle-speech.sh (CMake).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_STANDALONE_OUTPUT_QUEUE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_STANDALONE_OUTPUT_QUEUE_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace molt_speech {

inline constexpr int kStalledExitStatus = 3;
inline constexpr int64_t kMaxWriteMs = 2000;
inline constexpr size_t kMaxQueuedBytes = 1 << 20;

class OutputQueue {
 public:
  OutputQueue();
  OutputQueue(const OutputQueue&) = delete;
  OutputQueue& operator=(const OutputQueue&) = delete;
  // Never runs: the helper leaves with _Exit().
  ~OutputQueue();

  void Start();
  void Push(std::string line);
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
  // When the write in progress started (steady clock, ms), or 0.
  std::atomic<int64_t> write_started_ms_{0};
  std::thread writer_;
  std::thread watchdog_;
};

// Milliseconds on a monotonic clock.
int64_t NowMs();

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_STANDALONE_OUTPUT_QUEUE_H_
