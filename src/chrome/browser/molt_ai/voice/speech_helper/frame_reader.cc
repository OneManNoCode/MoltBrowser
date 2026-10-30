// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/frame_reader.h"

#include <cstddef>
#include <string>

namespace molt_speech {

namespace {

uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// What the buffer may hold before Append(): less than one header plus a
// whole payload.
constexpr size_t kMaxHeldBytes = kFrameHeaderBytes + kMaxFramePayloadBytes;

}  // namespace

Frame::Frame() = default;
Frame::Frame(const Frame&) = default;
Frame& Frame::operator=(const Frame&) = default;
Frame::~Frame() = default;

std::string CheckFrameHeader(uint32_t type, uint32_t length) {
  if (length > kMaxFramePayloadBytes) {
    return "frame length " + std::to_string(length) + " is over " +
           std::to_string(kMaxFramePayloadBytes);
  }
  switch (type) {
    case static_cast<uint32_t>(FrameType::kAudio):
      if (length == 0 || length % 2 != 0) {
        return "AUDIO frame of " + std::to_string(length) +
               " bytes: not a whole number of samples";
      }
      return std::string();
    case static_cast<uint32_t>(FrameType::kConfig):
      if (length < 2) {
        return "CONFIG frame of " + std::to_string(length) + " bytes";
      }
      return std::string();
    case static_cast<uint32_t>(FrameType::kReset):
    case static_cast<uint32_t>(FrameType::kShutdown):
      if (length != 0) {
        return std::string(type == 3 ? "RESET" : "SHUTDOWN") +
               " frame with a payload";
      }
      return std::string();
    default:
      return "unknown frame type " + std::to_string(type);
  }
}

FrameReader::FrameReader() = default;
FrameReader::~FrameReader() = default;

bool FrameReader::Append(const uint8_t* data, size_t size) {
  if (failed_) {
    return false;
  }
  if (size > kMaxReadBytes || pending_bytes() >= kMaxHeldBytes) {
    failed_ = true;
    return false;
  }
  if (size == 0) {
    return true;
  }
  // Drop what was consumed before growing.
  if (start_ > 0) {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(start_));
    start_ = 0;
  }
  buffer_.insert(buffer_.end(), data, data + size);
  return true;
}

FrameReader::Status FrameReader::Fail(const std::string& message,
                                      std::string* error) {
  failed_ = true;
  buffer_.clear();
  start_ = 0;
  if (error) {
    *error = message;
  }
  return Status::kError;
}

FrameReader::Status FrameReader::Next(Frame* frame, std::string* error) {
  if (failed_) {
    return Fail("the frame stream already failed", error);
  }
  const size_t available = pending_bytes();
  if (available < kFrameHeaderBytes) {
    return Status::kNeedMore;
  }
  const uint8_t* head = buffer_.data() + start_;
  const uint32_t type = ReadLe32(head);
  const uint32_t length = ReadLe32(head + 4);
  std::string bad = CheckFrameHeader(type, length);
  if (!bad.empty()) {
    return Fail(bad, error);
  }
  if (available - kFrameHeaderBytes < length) {
    return Status::kNeedMore;
  }
  frame->type = static_cast<FrameType>(type);
  frame->payload.assign(reinterpret_cast<const char*>(head + kFrameHeaderBytes),
                        length);
  start_ += kFrameHeaderBytes + length;
  if (start_ == buffer_.size()) {
    buffer_.clear();
    start_ = 0;
  }
  return Status::kFrame;
}

}  // namespace molt_speech
