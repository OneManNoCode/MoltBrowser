// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A stand-in for the speech helper (`molt-speech`) in voice mode's browser
// tests (voice_session_browsertest.cc): it speaks protocol v1
// (docs/design/SPEECH_HELPER_PROTOCOL.md) and plays a script instead of
// recognizing speech. The browser starts it with its model directory as the
// one argument, as it starts the real one; the script is
// <model directory>/fake_script.txt, one event per line:
//
//   ready                       at once
//   <ms> wake                   once <ms> of audio has arrived
//   <ms> speech_start
//   <ms> partial <text>
//   <ms> final <confidence> <text>
//   <ms> speech_end
//   <ms> line <raw line>        any line, as a hijacked helper might write
//   <ms> exit <code>            ends with <code>, as a crash would
//
// Times count the 16 kHz samples of AUDIO frames received. It checks every
// frame as the real helper must: an unknown type or a frame over 65536
// bytes is a protocol error (an `error` line, then exit 2). It exits 0 on
// SHUTDOWN or end of file. Plain C++, no //base.

#ifdef UNSAFE_BUFFERS_BUILD
// A C entry point's argv is a raw array, and this test-only program has no
// //base for spans.
#pragma allow_unsafe_buffers
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct Event {
  long long at_ms = 0;
  std::string kind;
  std::string rest;
};

void Emit(const std::string& line) {
  fputs(line.c_str(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

std::string Escaped(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

// Plays |event|; false when it ends the helper.
bool Play(const Event& event) {
  if (event.kind == "wake" || event.kind == "speech_start" ||
      event.kind == "speech_end") {
    Emit("{\"type\":\"" + event.kind + "\"}");
  } else if (event.kind == "partial") {
    Emit("{\"type\":\"partial\",\"text\":\"" + Escaped(event.rest) + "\"}");
  } else if (event.kind == "final") {
    const size_t space = event.rest.find(' ');
    const std::string confidence = event.rest.substr(0, space);
    const std::string text =
        space == std::string::npos ? std::string() : event.rest.substr(space + 1);
    Emit("{\"type\":\"final\",\"text\":\"" + Escaped(text) +
         "\",\"confidence\":" + confidence + "}");
  } else if (event.kind == "line") {
    Emit(event.rest);
  } else if (event.kind == "exit") {
    exit(atoi(event.rest.c_str()));
  }
  return true;
}

bool ReadExactly(void* buffer, size_t size) {
  return fread(buffer, 1, size, stdin) == size;
}

uint32_t LittleEndian32(const unsigned char* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  if (argc != 2) {
    Emit("{\"type\":\"error\",\"code\":\"usage\"}");
    return 2;
  }
  std::vector<Event> events;
  std::ifstream script(std::string(argv[1]) + "/fake_script.txt");
  bool ready = false;
  std::string text;
  while (std::getline(script, text)) {
    if (text == "ready") {
      ready = true;
      continue;
    }
    Event event;
    const size_t first = text.find(' ');
    if (first == std::string::npos) {
      continue;
    }
    event.at_ms = atoll(text.substr(0, first).c_str());
    const size_t second = text.find(' ', first + 1);
    event.kind = text.substr(first + 1, second == std::string::npos
                                            ? std::string::npos
                                            : second - first - 1);
    if (second != std::string::npos) {
      event.rest = text.substr(second + 1);
    }
    events.push_back(event);
  }
  if (ready) {
    Emit("{\"type\":\"ready\",\"engine\":\"fake\",\"models\":[\"script\"],"
         "\"protocol\":1}");
  }

  size_t next = 0;
  long long samples = 0;
  std::vector<unsigned char> payload;
  for (;;) {
    unsigned char header[8];
    if (!ReadExactly(header, sizeof(header))) {
      return 0;  // end of file
    }
    const uint32_t type = LittleEndian32(header);
    const uint32_t length = LittleEndian32(header + 4);
    if (length > 65536 || type < 1 || type > 4) {
      Emit("{\"type\":\"error\",\"code\":\"bad_frame\"}");
      return 2;
    }
    payload.resize(length);
    if (length > 0 && !ReadExactly(payload.data(), length)) {
      return 0;
    }
    if (type == 4) {
      return 0;  // SHUTDOWN
    }
    if (type != 1) {
      continue;  // CONFIG, RESET
    }
    samples += length / 2;
    const long long ms = samples * 1000 / 16000;
    while (next < events.size() && events[next].at_ms <= ms) {
      Play(events[next++]);
    }
  }
}
