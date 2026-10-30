// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A stand-in for the voice helper (`molt-voice`) in voice mode's browser
// tests (voice_speech_browsertest.cc): it speaks protocol v1
// (docs/design/VOICE_HELPER_PROTOCOL.md) with a tone instead of a voice.
// The browser starts it as it starts the real one, `--models <model
// directory>`.
//
//   - READY at once: 16,000 Hz, the voice "fake".
//   - SPEAK: 10 ms of a 440 Hz tone per byte of text (at most 3 s), in
//     frames of 1,600 samples, then DONE `spoken`; an empty-looking text
//     ("...") gives DONE `nothing`.
//   - A text with "crash" in it: it exits with status 9 instead, as a crash
//     would.
//   - A text with "garbage" in it: it writes AUDIO for a request that isn't
//     being said, as a helper a hostile text took over might.
//   - A text with "silent" in it: no AUDIO and no DONE for it, ever (it
//     still reads what comes next).
//   - CANCEL: nothing to do (each request is written at once).
//   - SHUTDOWN or the end of input: exit 0. Anything malformed: an ERROR
//     frame (code 3) and exit 2.
//
// Plain C++, no //base.

#ifdef UNSAFE_BUFFERS_BUILD
// A C entry point's argv is a raw array, and this test-only program has no
// //base for spans.
#pragma allow_unsafe_buffers
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr uint32_t kRate = 16000;

void PutU32(uint32_t value, std::string* out) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

uint32_t GetU32(const std::string& s, size_t at) {
  return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3])) << 24;
}

void Write(uint32_t type, const std::string& payload) {
  std::string frame;
  PutU32(type, &frame);
  PutU32(static_cast<uint32_t>(payload.size()), &frame);
  frame += payload;
  fwrite(frame.data(), 1, frame.size(), stdout);
  fflush(stdout);
}

[[noreturn]] void Fail(const char* message) {
  std::string payload;
  PutU32(3, &payload);
  payload += message;
  Write(4, payload);
  exit(2);
}

bool ReadExactly(std::string* out, size_t size) {
  out->resize(size);
  size_t done = 0;
  while (done < size) {
    const size_t n = fread(&(*out)[done], 1, size - done, stdin);
    if (n == 0) {
      return false;
    }
    done += n;
  }
  return true;
}

void Audio(uint32_t id, size_t samples, size_t* written) {
  std::string payload;
  PutU32(id, &payload);
  for (size_t i = 0; i < samples; ++i) {
    const double t = static_cast<double>(*written + i) / kRate;
    const auto v = static_cast<int16_t>(
        8000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
    payload.push_back(static_cast<char>(v & 0xFF));
    payload.push_back(static_cast<char>((v >> 8) & 0xFF));
  }
  *written += samples;
  Write(2, payload);
}

void Done(uint32_t id, uint32_t status, size_t samples) {
  std::string payload;
  PutU32(id, &payload);
  PutU32(status, &payload);
  PutU32(static_cast<uint32_t>(samples * 1000 / kRate), &payload);
  PutU32(samples ? 5 : 0, &payload);
  PutU32(5, &payload);
  Write(3, payload);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (argc != 3 || std::string(argv[1]) != "--models") {
    Fail("usage: --models <dir>");
  }
  std::string ready;
  PutU32(1, &ready);
  PutU32(kRate, &ready);
  ready += "fake";
  Write(1, ready);
  std::string header;
  std::string payload;
  while (ReadExactly(&header, 8)) {
    const uint32_t type = GetU32(header, 0);
    const uint32_t length = GetU32(header, 4);
    if (type < 1 || type > 3 || (type == 1 && (length < 9 || length > 4104)) ||
        (type != 1 && length != 0)) {
      Fail("bad frame");
    }
    if (!ReadExactly(&payload, length)) {
      return 0;
    }
    if (type == 3) {
      return 0;
    }
    if (type == 2) {
      continue;  // CANCEL: every request was answered already
    }
    const uint32_t id = GetU32(payload, 0);
    const std::string text = payload.substr(8);
    if (text.find("crash") != std::string::npos) {
      exit(9);
    }
    if (text.find("garbage") != std::string::npos) {
      size_t written = 0;
      Audio(id + 7, 100, &written);
      continue;
    }
    if (text.find("silent") != std::string::npos) {
      continue;  // never answered
    }
    if (text.find_first_not_of(". ") == std::string::npos) {
      Done(id, 2, 0);
      continue;
    }
    const size_t total = std::min<size_t>(text.size() * kRate / 100, 3 * kRate);
    size_t written = 0;
    while (written < total) {
      Audio(id, std::min<size_t>(1600, total - written), &written);
    }
    Done(id, 0, written);
  }
  return 0;
}
