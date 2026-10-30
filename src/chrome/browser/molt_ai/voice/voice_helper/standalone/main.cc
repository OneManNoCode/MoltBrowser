// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice: voice mode's own voice. Frames on stdin (SPEAK, CANCEL,
// SHUTDOWN), frames on stdout (READY, AUDIO, DONE, ERROR):
// docs/design/VOICE_HELPER_PROTOCOL.md.
//
//   molt-voice --models <dir> [--threads 1|2]
//
// It reads nothing but its model directory (and what the C/C++ runtime and
// ONNX Runtime's CPU detection read at start), writes no file, opens no
// socket and starts no process. Built only by scripts/bundle-speech.sh.
//
// Threads: the main thread reads stdin (so a CANCEL is read at once, even
// while a request is being synthesized); one thread synthesizes the
// requests in order and writes their AUDIO and DONE frames; FrameOutput's
// thread writes stdout.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "chrome/browser/molt_ai/voice/voice_helper/standalone/frame_output.h"
#include "chrome/browser/molt_ai/voice/voice_helper/standalone/sherpa_voice.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_phonemes.h"
#include "chrome/browser/molt_ai/voice/voice_helper/voice_synth.h"

namespace molt_voice {
namespace {

constexpr int kErrorExitStatus = 2;
// Requests waiting behind the one being said; more is a protocol error
// (the browser sends one at a time).
constexpr size_t kMaxQueuedRequests = 16;
constexpr size_t kMaxTokensBytes = 64 << 10;

FrameOutput* g_output = nullptr;

[[noreturn]] void FailWith(ErrorCode code, const std::string& message) {
  if (g_output) {
    g_output->Push(EncodeError(code, message));
    g_output->Drain(500);
  }
  std::_Exit(kErrorExitStatus);
}

bool IsRegularFile(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

bool ReadFile(const std::string& path, size_t max, std::string* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  std::string data;
  char buffer[65536];
  size_t n = 0;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    data.append(buffer, n);
    if (data.size() > max) {
      fclose(f);
      return false;
    }
  }
  const bool ok = !ferror(f);
  fclose(f);
  *out = std::move(data);
  return ok;
}

int ReadStdin(uint8_t* buffer, size_t size) {
#if defined(_WIN32)
  return _read(0, buffer, static_cast<unsigned int>(size));
#else
  return static_cast<int>(read(0, buffer, size));
#endif
}

// The requests, shared by the reading and the synthesizing threads.
class Queue {
 public:
  struct Item {
    SpeakRequest request;
    // CANCELs read before it arrived.
    uint64_t cancels_before = 0;
  };

  // False if too many wait.
  bool Push(SpeakRequest request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.size() >= kMaxQueuedRequests) {
      return false;
    }
    items_.push_back(Item{std::move(request), cancels_});
    changed_.notify_all();
    return true;
  }

  // Cancels everything sent so far: what waits, and what is being said.
  void Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++cancels_;
    changed_.notify_all();
  }

  Item Pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return !items_.empty(); });
    Item item = std::move(items_.front());
    items_.pop_front();
    return item;
  }

  bool Cancelled(const Item& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancels_ > item.cancels_before;
  }

  // Waits until |until_ms| or a CANCEL of |item|.
  void WaitUntil(const Item& item, int64_t until_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (cancels_ == item.cancels_before) {
      const int64_t left = until_ms - NowMs();
      if (left <= 0) {
        return;
      }
      changed_.wait_for(lock, std::chrono::milliseconds(left));
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Item> items_;
  uint64_t cancels_ = 0;
};

class Hooks : public SynthHooks {
 public:
  Hooks(Queue* queue, const Queue::Item* item) : queue_(queue), item_(item) {}

  int64_t NowMs() override { return molt_voice::NowMs(); }
  bool Cancelled() override { return queue_->Cancelled(*item_); }
  void WaitUntil(int64_t until_ms) override {
    queue_->WaitUntil(*item_, until_ms);
  }
  void Emit(std::string frame) override { g_output->Push(std::move(frame)); }

 private:
  Queue* const queue_;
  const Queue::Item* const item_;
};

void SynthesizeLoop(Queue* queue,
                    const Phonemizer* phonemizer,
                    VoiceEngine* engine) {
  const SynthLimits limits;
  while (true) {
    const Queue::Item item = queue->Pop();
    Hooks hooks(queue, &item);
    const DoneInfo done =
        SpeakRequestNow(item.request, *phonemizer, *engine, hooks, limits);
    g_output->Push(EncodeDone(done));
  }
}

int Run(int argc, char** argv) {
  std::string models;
  int threads = 1;
  const char* const kUsage = "usage: molt-voice --models <dir> [--threads 1|2]";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--models" && i + 1 < argc) {
      models = argv[++i];
    } else if (arg == "--threads" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value != "1" && value != "2") {
        FailWith(ErrorCode::kArgs, "--threads must be 1 or 2");
      }
      threads = value[0] - '0';
    } else {
      FailWith(ErrorCode::kArgs, kUsage);
    }
  }
  if (models.empty()) {
    FailWith(ErrorCode::kArgs, kUsage);
  }

  const VoiceModelPaths paths = VoiceModelPathsIn(models);
  for (const std::string* file :
       {&paths.model, &paths.tokens, &paths.lexicon, &paths.model_card}) {
    if (!IsRegularFile(*file)) {
      // Names the file relative to the model directory only.
      FailWith(ErrorCode::kModel,
               "a model file is missing: " +
                   file->substr(std::min(file->size(), models.size() + 1)));
    }
  }
  std::string data;
  std::string error;
  std::vector<uint32_t> tokens;
  if (!ReadFile(paths.tokens, kMaxTokensBytes, &data) ||
      !LoadVoiceTokens(data, &tokens, &error)) {
    FailWith(ErrorCode::kModel, "tokens.txt is unreadable: " + error);
  }
  Lexicon lexicon;
  if (!ReadFile(paths.lexicon, kMaxLexiconBytes, &data) ||
      !lexicon.Load(std::move(data), &error)) {
    FailWith(ErrorCode::kModel, "the lexicon is unreadable: " + error);
  }
  static Phonemizer phonemizer(std::move(lexicon), std::move(tokens));
  std::unique_ptr<SherpaVoice> engine =
      SherpaVoice::Create(paths, threads, &error);
  if (!engine) {
    FailWith(ErrorCode::kModel, error);
  }
  g_output->Push(EncodeReady(engine->sample_rate(), VoiceName()));

  static Queue queue;
  std::thread(SynthesizeLoop, &queue, &phonemizer, engine.release()).detach();

  FrameReader reader(FrameReader::Direction::kToHelper);
  Frame frame;
  std::vector<uint8_t> buffer(kFrameHeaderBytes + kMaxFramePayloadBytes);
  while (true) {
    const int n = ReadStdin(buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      // The end of stdin (or it broke): the browser is gone or done.
      g_output->Drain(500);
      std::_Exit(0);
    }
    if (!reader.Append(buffer.data(), static_cast<size_t>(n))) {
      FailWith(ErrorCode::kInternal, "the frame reader refused input");
    }
    while (true) {
      const FrameReader::Status status = reader.Next(&frame, &error);
      if (status == FrameReader::Status::kNeedMore) {
        break;
      }
      if (status == FrameReader::Status::kError) {
        FailWith(ErrorCode::kProtocol, error);
      }
      switch (static_cast<ToHelper>(frame.type)) {
        case ToHelper::kSpeak: {
          SpeakRequest request;
          if (!ParseSpeak(frame.payload, &request, &error)) {
            FailWith(ErrorCode::kProtocol, error);
          }
          if (!queue.Push(std::move(request))) {
            FailWith(ErrorCode::kProtocol, "too many requests wait");
          }
          break;
        }
        case ToHelper::kCancel:
          queue.Cancel();
          break;
        case ToHelper::kShutdown:
          g_output->Drain(500);
          std::_Exit(0);
      }
    }
  }
}

}  // namespace
}  // namespace molt_voice

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#else
  // A browser that closed the pipe makes write() fail instead.
  signal(SIGPIPE, SIG_IGN);
#endif
  static molt_voice::FrameOutput output;
  molt_voice::g_output = &output;
  output.Start();
  return molt_voice::Run(argc, argv);
}
