// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The frames the browser writes to molt-speech's stdin
// (docs/design/SPEECH_HELPER_PROTOCOL.md §2): a little-endian uint32 type, a
// little-endian uint32 length, then that many bytes of payload.
//
// FrameReader reassembles them from reads of any size and checks each
// header as soon as its 8 bytes are in, so a bad frame is refused before
// its payload arrives (a 4 GiB length is never waited for or buffered).
// Std-only: the helper (built outside GN), molt_ai_unittests and
// molt_speech_protocol_fuzzer build the same code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_FRAME_READER_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_FRAME_READER_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace molt_speech {

// Protocol version 1's frame types.
enum class FrameType : uint32_t {
  kAudio = 1,
  kConfig = 2,
  kReset = 3,
  kShutdown = 4,
};

inline constexpr size_t kFrameHeaderBytes = 8;
// The largest payload of any frame.
inline constexpr uint32_t kMaxFramePayloadBytes = 65536;
// The most FrameReader holds: one whole frame and one read of the
// largest size the helper makes.
inline constexpr size_t kMaxReadBytes = 65536;

struct Frame {
  Frame();
  Frame(const Frame&);
  Frame& operator=(const Frame&);
  ~Frame();

  FrameType type = FrameType::kAudio;
  // For AUDIO: little-endian int16 samples (an even, non-zero length).
  // For CONFIG: the JSON text (not yet checked). Empty for RESET and
  // SHUTDOWN.
  std::string payload;
};

class FrameReader {
 public:
  enum class Status {
    // |frame| holds the next frame.
    kFrame,
    // Not enough bytes yet; Append() more.
    kNeedMore,
    // A protocol error (|error| says which, without echoing input). The
    // stream can't be resynchronized: the helper writes an error line and
    // exits, and the reader refuses everything after.
    kError,
  };

  FrameReader();
  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;
  ~FrameReader();

  // Adds bytes read from stdin. Call Next() until it returns kNeedMore
  // before appending again; then at most one frame and one read are held.
  // False (and the reader is in error) if |size| exceeds kMaxReadBytes or
  // the caller didn't drain the frames first.
  bool Append(const uint8_t* data, size_t size);

  Status Next(Frame* frame, std::string* error);

  // Bytes held that don't make a whole frame yet. At end of input, a
  // non-zero value means the browser stopped in the middle of a frame.
  size_t pending_bytes() const { return buffer_.size() - start_; }

 private:
  Status Fail(const std::string& message, std::string* error);

  std::vector<uint8_t> buffer_;
  size_t start_ = 0;
  bool failed_ = false;
};

// Checks a frame header (the first 8 bytes of a frame). Empty if it is
// acceptable, else why not. Exposed for the tests.
std::string CheckFrameHeader(uint32_t type, uint32_t length);

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_FRAME_READER_H_
