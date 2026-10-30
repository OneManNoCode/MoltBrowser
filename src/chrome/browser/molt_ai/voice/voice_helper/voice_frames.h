// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice helper protocol's frames (docs/design/VOICE_HELPER_PROTOCOL.md
// §2, §3), both ways: the browser writes SPEAK, CANCEL and SHUTDOWN to
// molt-voice's stdin; the helper writes READY, AUDIO, DONE and ERROR to its
// stdout. Each frame is an 8-byte header (type and payload length, both
// unsigned 32-bit little-endian) and the payload. Binary both ways, since
// the helper's output is audio.
//
// FrameReader checks each header as soon as its 8 bytes are in (a known
// type for its direction, a length that type allows), so a bad frame is
// refused without waiting for or buffering its payload. The Parse*()
// functions check each payload. Both sides treat what they read as
// untrusted: the helper reads page-derived text, the browser reads a
// helper that page-derived text may have taken over.
//
// Std-only, no exceptions: the helper (CMake), molt_ai_unittests and the
// fuzzers build the same code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_FRAMES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_FRAMES_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace molt_voice {

// READY's protocol field; a change to the protocol page bumps it.
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr size_t kFrameHeaderBytes = 8;
inline constexpr size_t kMaxFramePayloadBytes = 65536;
// SPEAK: the id, the rate and two reserved bytes, then the text.
inline constexpr size_t kSpeakHeaderBytes = 8;
inline constexpr size_t kMaxSpeakTextBytesInFrame = 4096;
inline constexpr int kMinRatePercent = 50;
inline constexpr int kMaxRatePercent = 250;
// READY's voice name and ERROR's message, in bytes.
inline constexpr size_t kMaxVoiceNameBytes = 64;
inline constexpr size_t kMaxErrorMessageBytes = 200;
inline constexpr int kMinSampleRate = 8000;
inline constexpr int kMaxSampleRate = 48000;
// An AUDIO frame holds at most this many samples (after its 4-byte id).
inline constexpr size_t kMaxAudioFrameSamples = (kMaxFramePayloadBytes - 4) / 2;
inline constexpr size_t kDoneBytes = 20;

// Browser to helper.
enum class ToHelper : uint32_t {
  kSpeak = 1,
  kCancel = 2,
  kShutdown = 3,
};

// Helper to browser.
enum class FromHelper : uint32_t {
  kReady = 1,
  kAudio = 2,
  kDone = 3,
  kError = 4,
};

// How a request ended (DONE's status).
enum class DoneStatus : uint32_t {
  kSpoken = 0,
  kCancelled = 1,
  kNothingToSay = 2,
  // Its audio reached the per-request limit; the rest wasn't said.
  kCut = 3,
  kEngineFailed = 4,
};
inline constexpr uint32_t kMaxDoneStatus = 4;

// ERROR's code; the helper exits with status 2 after it.
enum class ErrorCode : uint32_t {
  kArgs = 1,
  kModel = 2,
  kProtocol = 3,
  kEngine = 4,
  kInternal = 5,
};
inline constexpr uint32_t kMaxErrorCode = 5;

struct Frame {
  Frame();
  Frame(const Frame&);
  Frame& operator=(const Frame&);
  Frame(Frame&&);
  Frame& operator=(Frame&&);
  ~Frame();

  uint32_t type = 0;
  std::string payload;
};

class FrameReader {
 public:
  enum class Direction {
    kToHelper,
    kFromHelper,
  };
  enum class Status {
    kFrame,
    kNeedMore,
    kError,
  };

  explicit FrameReader(Direction direction);
  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;
  ~FrameReader();

  // Adds bytes read; call Next() until it says kNeedMore before adding
  // more. False after an error.
  bool Append(const uint8_t* data, size_t size);
  // The next whole frame, if there is one. After kError (with |error|, no
  // input echoed) the reader stays in error.
  Status Next(Frame* frame, std::string* error);

 private:
  const Direction direction_;
  std::string buffer_;
  size_t consumed_ = 0;
  bool failed_ = false;
};

// ---- Writing ----------------------------------------------------------------

std::string EncodeFrame(uint32_t type, std::string_view payload);

std::string EncodeSpeak(uint32_t id, int rate_percent, std::string_view text);
std::string EncodeCancel();
std::string EncodeShutdown();

std::string EncodeReady(int sample_rate, std::string_view voice);
// |count| at most kMaxAudioFrameSamples.
std::string EncodeAudio(uint32_t id, const int16_t* samples, size_t count);

struct DoneInfo {
  uint32_t id = 0;
  DoneStatus status = DoneStatus::kSpoken;
  // Audio written for the request, in ms.
  uint32_t audio_ms = 0;
  // From reading the SPEAK (or the request reaching the front of the
  // queue, whichever is later) to its first AUDIO frame; 0 without audio.
  uint32_t first_audio_ms = 0;
  // Time spent synthesizing it.
  uint32_t synth_ms = 0;
};
std::string EncodeDone(const DoneInfo& done);
std::string EncodeError(ErrorCode code, std::string_view message);

// ---- Reading payloads
// ---------------------------------------------------------

struct SpeakRequest {
  SpeakRequest();
  SpeakRequest(const SpeakRequest&);
  SpeakRequest& operator=(const SpeakRequest&);
  SpeakRequest(SpeakRequest&&);
  SpeakRequest& operator=(SpeakRequest&&);
  ~SpeakRequest();

  uint32_t id = 0;
  int rate_percent = 100;
  // Bytes as sent (voice_text.h cleans them).
  std::string text;
};
bool ParseSpeak(std::string_view payload,
                SpeakRequest* request,
                std::string* error);

struct ReadyInfo {
  ReadyInfo();
  ReadyInfo(const ReadyInfo&);
  ReadyInfo& operator=(const ReadyInfo&);
  ~ReadyInfo();

  uint32_t protocol = 0;
  int sample_rate = 0;
  // Printable ASCII.
  std::string voice;
};
bool ParseReady(std::string_view payload, ReadyInfo* ready);
bool ParseAudio(std::string_view payload,
                uint32_t* id,
                std::vector<int16_t>* samples);
bool ParseDone(std::string_view payload, DoneInfo* done);
// |message| printable ASCII.
bool ParseError(std::string_view payload,
                ErrorCode* code,
                std::string* message);

// Printable ASCII only, at most |max| bytes: what a message or name may be.
std::string PrintableAscii(std::string_view text, size_t max);

}  // namespace molt_voice

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_FRAMES_H_
