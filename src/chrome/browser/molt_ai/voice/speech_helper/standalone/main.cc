// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-speech: touchless browsing's resident speech helper. Frames on
// stdin, JSON lines on stdout (docs/design/SPEECH_HELPER_PROTOCOL.md).
//
//   molt-speech --models <dir> [--asr-threads 1|2]
//
// It reads nothing but its model directory (and what the C/C++ runtime and
// ONNX Runtime's CPU detection read at start), writes no file, opens no
// socket and starts no process. Built only by scripts/bundle-speech.sh.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "chrome/browser/molt_ai/voice/speech_helper/frame_reader.h"
#include "chrome/browser/molt_ai/voice/speech_helper/listener.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_config.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_events.h"
#include "chrome/browser/molt_ai/voice/speech_helper/speech_text.h"
#include "chrome/browser/molt_ai/voice/speech_helper/standalone/output_queue.h"
#include "chrome/browser/molt_ai/voice/speech_helper/standalone/sherpa_engines.h"

namespace molt_speech {
namespace {

constexpr int kErrorExitStatus = 2;
// The vocabularies are a few hundred KiB; anything much bigger is wrong.
constexpr long kMaxVocabularyBytes = 4 << 20;

OutputQueue* g_output = nullptr;

[[noreturn]] void FailWith(const char* code, const std::string& message) {
  if (g_output) {
    g_output->Push(ErrorEvent(code, message));
    g_output->Drain(500);
  }
  std::_Exit(kErrorExitStatus);
}

bool IsRegularFile(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

bool ReadSmallFile(const std::string& path, std::string* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  std::string data;
  char buffer[16384];
  size_t n = 0;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    data.append(buffer, n);
    if (data.size() > static_cast<size_t>(kMaxVocabularyBytes)) {
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

Vocabularies LoadVocabularies(const ModelPaths& paths) {
  Vocabularies v;
  std::string text;
  std::string error;
  if (!ReadSmallFile(paths.kws_pieces, &text) ||
      !v.kws_pieces.Load(text, &error)) {
    FailWith("model", "kws/pieces.vocab is unreadable: " + error);
  }
  if (!ReadSmallFile(paths.kws_tokens, &text) ||
      !LoadTokenSet(text, &v.kws_tokens, &error)) {
    FailWith("model", "kws/tokens.txt is unreadable: " + error);
  }
  if (!ReadSmallFile(paths.asr_vocab, &text) ||
      !v.asr_pieces.Load(text, &error)) {
    FailWith("model", "asr/bpe.vocab is unreadable: " + error);
  }
  if (!ReadSmallFile(paths.asr_tokens, &text) ||
      !LoadTokenSet(text, &v.asr_tokens, &error)) {
    FailWith("model", "asr/tokens.txt is unreadable: " + error);
  }
  return v;
}

void Dispatch(const Frame& frame, Listener* listener) {
  switch (frame.type) {
    case FrameType::kAudio: {
      const size_t count = frame.payload.size() / 2;
      std::vector<int16_t> samples(count);
      const auto* p = reinterpret_cast<const uint8_t*>(frame.payload.data());
      for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<int16_t>(
            static_cast<uint16_t>(p[2 * i]) |
            static_cast<uint16_t>(p[2 * i + 1] << 8));
      }
      listener->Feed(samples.data(), samples.size());
      return;
    }
    case FrameType::kConfig: {
      ConfigUpdate update;
      std::string error;
      const ConfigStatus status =
          ParseConfigFrame(frame.payload, &update, &error);
      if (status == ConfigStatus::kProtocolError) {
        FailWith("protocol", error);
      }
      if (status == ConfigStatus::kConfigError) {
        FailWith("config", error);
      }
      switch (listener->ApplyConfig(update, &error)) {
        case Listener::ConfigResult::kOk:
          return;
        case Listener::ConfigResult::kConfigError:
          FailWith("config", error);
        case Listener::ConfigResult::kEngineError:
          FailWith("engine", error);
      }
      return;
    }
    case FrameType::kReset:
      listener->Reset();
      return;
    case FrameType::kShutdown:
      g_output->Drain(500);
      std::_Exit(0);
  }
}

int Run(int argc, char** argv) {
  std::string models;
  int asr_threads = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--models" && i + 1 < argc) {
      models = argv[++i];
    } else if (arg == "--asr-threads" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value != "1" && value != "2") {
        FailWith("args", "--asr-threads must be 1 or 2");
      }
      asr_threads = value[0] - '0';
    } else {
      FailWith("args", "usage: molt-speech --models <dir> [--asr-threads 1|2]");
    }
  }
  if (models.empty()) {
    FailWith("args", "usage: molt-speech --models <dir> [--asr-threads 1|2]");
  }

  const ModelPaths paths = ModelPathsIn(models);
  for (const std::string* file :
       {&paths.kws_encoder, &paths.kws_decoder, &paths.kws_joiner,
        &paths.kws_tokens, &paths.kws_pieces, &paths.vad, &paths.asr_encoder,
        &paths.asr_decoder, &paths.asr_joiner, &paths.asr_tokens,
        &paths.asr_vocab}) {
    if (!IsRegularFile(*file)) {
      // Names the file relative to the model directory only.
      FailWith("model", "a model file is missing: " +
                            file->substr(std::min(file->size(),
                                                  models.size() + 1)));
    }
  }
  Vocabularies vocabularies = LoadVocabularies(paths);

  std::string error;
  std::unique_ptr<SherpaEngines> engines = SherpaEngines::Create(
      paths, asr_threads,
      [](const std::string& message) { FailWith("engine", message); }, &error);
  if (!engines) {
    FailWith("model", error);
  }
  Listener listener(std::move(engines), std::move(vocabularies),
                    [](const std::string& line) { g_output->Push(line); });
  if (!listener.Start(&error)) {
    FailWith("model", error);
  }
  g_output->Push(ReadyEvent(EngineDescription(), ModelNames()));

  FrameReader reader;
  Frame frame;
  std::vector<uint8_t> buffer(kMaxReadBytes);
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
      FailWith("internal", "the frame reader refused input");
    }
    while (true) {
      const FrameReader::Status status = reader.Next(&frame, &error);
      if (status == FrameReader::Status::kNeedMore) {
        break;
      }
      if (status == FrameReader::Status::kError) {
        FailWith("protocol", error);
      }
      Dispatch(frame, &listener);
    }
  }
}

}  // namespace
}  // namespace molt_speech

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#else
  // A browser that closed the pipe makes write() fail instead.
  signal(SIGPIPE, SIG_IGN);
#endif
  static molt_speech::OutputQueue output;
  molt_speech::g_output = &output;
  output.Start();
  return molt_speech::Run(argc, argv);
}
