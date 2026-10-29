// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/standalone/output_queue.h"

#include <errno.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace molt_speech {

namespace {

// Writes all of |data| to stdout, or ends the helper.
void WriteAll(const std::string& data) {
  size_t done = 0;
  while (done < data.size()) {
#if defined(_WIN32)
    const unsigned int chunk =
        static_cast<unsigned int>(std::min<size_t>(data.size() - done, 1 << 20));
    const int n = _write(1, data.data() + done, chunk);
#else
    const ssize_t n = write(1, data.data() + done, data.size() - done);
#endif
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      std::_Exit(kStalledExitStatus);
    }
    done += static_cast<size_t>(n);
  }
}

}  // namespace

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

OutputQueue::OutputQueue() = default;
OutputQueue::~OutputQueue() = default;

void OutputQueue::Start() {
  writer_ = std::thread([this] { WriterLoop(); });
  watchdog_ = std::thread([this] { WatchdogLoop(); });
}

void OutputQueue::Push(std::string line) {
  std::lock_guard<std::mutex> lock(mutex_);
  queued_bytes_ += line.size();
  if (queued_bytes_ > kMaxQueuedBytes) {
    std::_Exit(kStalledExitStatus);
  }
  queue_.push_back(std::move(line));
  wake_writer_.notify_one();
}

void OutputQueue::Drain(int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  drained_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                    [this] { return queue_.empty() && !writing_; });
}

void OutputQueue::WriterLoop() {
  while (true) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_writer_.wait(lock, [this] { return !queue_.empty(); });
      line = std::move(queue_.front());
      queue_.pop_front();
      writing_ = true;
    }
    write_started_ms_.store(NowMs());
    WriteAll(line);
    write_started_ms_.store(0);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queued_bytes_ -= line.size();
      writing_ = false;
      if (queue_.empty()) {
        drained_.notify_all();
      }
    }
  }
}

void OutputQueue::WatchdogLoop() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int64_t started = write_started_ms_.load();
    if (started != 0 && NowMs() - started > kMaxWriteMs) {
      std::_Exit(kStalledExitStatus);
    }
  }
}

}  // namespace molt_speech
