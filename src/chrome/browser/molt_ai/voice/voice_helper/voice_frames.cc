// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_frames.h"

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molt_voice {

namespace {

uint32_t ReadU32(std::string_view s, size_t at) {
  return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3])) << 24;
}

uint16_t ReadU16(std::string_view s, size_t at) {
  return static_cast<uint16_t>(
      static_cast<uint16_t>(static_cast<uint8_t>(s[at])) |
      static_cast<uint16_t>(static_cast<uint8_t>(s[at + 1])) << 8);
}

void PutU32(uint32_t value, std::string* out) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

void PutU16(uint16_t value, std::string* out) {
  out->push_back(static_cast<char>(value & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
}

// The payload lengths each frame type allows: [min, max], and whether it
// must be even after the first |even_after| bytes (AUDIO's samples).
struct Allowed {
  bool known = false;
  size_t min = 0;
  size_t max = 0;
  bool even_after_id = false;
};

Allowed AllowedFor(FrameReader::Direction direction, uint32_t type) {
  Allowed a;
  a.known = true;
  if (direction == FrameReader::Direction::kToHelper) {
    switch (static_cast<ToHelper>(type)) {
      case ToHelper::kSpeak:
        a.min = kSpeakHeaderBytes + 1;
        a.max = kSpeakHeaderBytes + kMaxSpeakTextBytesInFrame;
        return a;
      case ToHelper::kCancel:
      case ToHelper::kShutdown:
        return a;  // empty
    }
    return Allowed();
  }
  switch (static_cast<FromHelper>(type)) {
    case FromHelper::kReady:
      a.min = 8;
      a.max = 8 + kMaxVoiceNameBytes;
      return a;
    case FromHelper::kAudio:
      a.min = 4 + 2;
      a.max = kMaxFramePayloadBytes;
      a.even_after_id = true;
      return a;
    case FromHelper::kDone:
      a.min = kDoneBytes;
      a.max = kDoneBytes;
      return a;
    case FromHelper::kError:
      a.min = 4;
      a.max = 4 + kMaxErrorMessageBytes;
      return a;
  }
  return Allowed();
}

const char* TypeName(FrameReader::Direction direction, uint32_t type) {
  if (direction == FrameReader::Direction::kToHelper) {
    switch (static_cast<ToHelper>(type)) {
      case ToHelper::kSpeak:
        return "SPEAK";
      case ToHelper::kCancel:
        return "CANCEL";
      case ToHelper::kShutdown:
        return "SHUTDOWN";
    }
    return "?";
  }
  switch (static_cast<FromHelper>(type)) {
    case FromHelper::kReady:
      return "READY";
    case FromHelper::kAudio:
      return "AUDIO";
    case FromHelper::kDone:
      return "DONE";
    case FromHelper::kError:
      return "ERROR";
  }
  return "?";
}

}  // namespace

Frame::Frame() = default;
Frame::Frame(const Frame&) = default;
Frame& Frame::operator=(const Frame&) = default;
Frame::Frame(Frame&&) = default;
Frame& Frame::operator=(Frame&&) = default;
Frame::~Frame() = default;

SpeakRequest::SpeakRequest() = default;
SpeakRequest::SpeakRequest(const SpeakRequest&) = default;
SpeakRequest& SpeakRequest::operator=(const SpeakRequest&) = default;
SpeakRequest::SpeakRequest(SpeakRequest&&) = default;
SpeakRequest& SpeakRequest::operator=(SpeakRequest&&) = default;
SpeakRequest::~SpeakRequest() = default;

ReadyInfo::ReadyInfo() = default;
ReadyInfo::ReadyInfo(const ReadyInfo&) = default;
ReadyInfo& ReadyInfo::operator=(const ReadyInfo&) = default;
ReadyInfo::~ReadyInfo() = default;

FrameReader::FrameReader(Direction direction) : direction_(direction) {}
FrameReader::~FrameReader() = default;

bool FrameReader::Append(const uint8_t* data, size_t size) {
  if (failed_) {
    return false;
  }
  // Drop what was consumed before growing.
  if (consumed_ > 0) {
    buffer_.erase(0, consumed_);
    consumed_ = 0;
  }
  buffer_.append(reinterpret_cast<const char*>(data), size);
  return true;
}

FrameReader::Status FrameReader::Next(Frame* frame, std::string* error) {
  if (failed_) {
    *error = "the reader failed before";
    return Status::kError;
  }
  const std::string_view rest = std::string_view(buffer_).substr(consumed_);
  if (rest.size() < kFrameHeaderBytes) {
    return Status::kNeedMore;
  }
  const uint32_t type = ReadU32(rest, 0);
  const uint32_t length = ReadU32(rest, 4);
  const Allowed allowed = AllowedFor(direction_, type);
  if (!allowed.known) {
    failed_ = true;
    *error = "unknown frame type " + std::to_string(type);
    return Status::kError;
  }
  if (length < allowed.min || length > allowed.max ||
      (allowed.even_after_id && (length - 4) % 2 != 0)) {
    failed_ = true;
    *error = std::string(TypeName(direction_, type)) + " frame length " +
             std::to_string(length) + " is not allowed";
    return Status::kError;
  }
  if (rest.size() < kFrameHeaderBytes + length) {
    return Status::kNeedMore;
  }
  frame->type = type;
  frame->payload.assign(rest.substr(kFrameHeaderBytes, length));
  consumed_ += kFrameHeaderBytes + length;
  if (consumed_ == buffer_.size()) {
    buffer_.clear();
    consumed_ = 0;
  }
  return Status::kFrame;
}

std::string EncodeFrame(uint32_t type, std::string_view payload) {
  std::string out;
  out.reserve(kFrameHeaderBytes + payload.size());
  PutU32(type, &out);
  PutU32(static_cast<uint32_t>(payload.size()), &out);
  out.append(payload);
  return out;
}

std::string EncodeSpeak(uint32_t id, int rate_percent, std::string_view text) {
  std::string payload;
  PutU32(id, &payload);
  PutU16(static_cast<uint16_t>(rate_percent), &payload);
  PutU16(0, &payload);
  payload.append(text.substr(0, kMaxSpeakTextBytesInFrame));
  return EncodeFrame(static_cast<uint32_t>(ToHelper::kSpeak), payload);
}

std::string EncodeCancel() {
  return EncodeFrame(static_cast<uint32_t>(ToHelper::kCancel), "");
}

std::string EncodeShutdown() {
  return EncodeFrame(static_cast<uint32_t>(ToHelper::kShutdown), "");
}

std::string EncodeReady(int sample_rate, std::string_view voice) {
  std::string payload;
  PutU32(kProtocolVersion, &payload);
  PutU32(static_cast<uint32_t>(sample_rate), &payload);
  payload.append(PrintableAscii(voice, kMaxVoiceNameBytes));
  return EncodeFrame(static_cast<uint32_t>(FromHelper::kReady), payload);
}

std::string EncodeAudio(uint32_t id, const int16_t* samples, size_t count) {
  if (count > kMaxAudioFrameSamples) {
    count = kMaxAudioFrameSamples;
  }
  std::string payload;
  payload.reserve(4 + 2 * count);
  PutU32(id, &payload);
  for (size_t i = 0; i < count; ++i) {
    PutU16(static_cast<uint16_t>(samples[i]), &payload);
  }
  return EncodeFrame(static_cast<uint32_t>(FromHelper::kAudio), payload);
}

std::string EncodeDone(const DoneInfo& done) {
  std::string payload;
  PutU32(done.id, &payload);
  PutU32(static_cast<uint32_t>(done.status), &payload);
  PutU32(done.audio_ms, &payload);
  PutU32(done.first_audio_ms, &payload);
  PutU32(done.synth_ms, &payload);
  return EncodeFrame(static_cast<uint32_t>(FromHelper::kDone), payload);
}

std::string EncodeError(ErrorCode code, std::string_view message) {
  std::string payload;
  PutU32(static_cast<uint32_t>(code), &payload);
  payload.append(PrintableAscii(message, kMaxErrorMessageBytes));
  return EncodeFrame(static_cast<uint32_t>(FromHelper::kError), payload);
}

std::string PrintableAscii(std::string_view text, size_t max) {
  std::string out;
  for (char c : text) {
    if (out.size() >= max) {
      break;
    }
    out.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  }
  return out;
}

bool ParseSpeak(std::string_view payload,
                SpeakRequest* request,
                std::string* error) {
  if (payload.size() <= kSpeakHeaderBytes ||
      payload.size() > kSpeakHeaderBytes + kMaxSpeakTextBytesInFrame) {
    *error = "SPEAK has no text, or too much";
    return false;
  }
  const uint32_t id = ReadU32(payload, 0);
  const int rate = ReadU16(payload, 4);
  if (ReadU16(payload, 6) != 0) {
    *error = "SPEAK's reserved field isn't 0";
    return false;
  }
  if (rate < kMinRatePercent || rate > kMaxRatePercent) {
    *error = "SPEAK's rate " + std::to_string(rate) + " is out of range";
    return false;
  }
  request->id = id;
  request->rate_percent = rate;
  request->text.assign(payload.substr(kSpeakHeaderBytes));
  return true;
}

bool ParseReady(std::string_view payload, ReadyInfo* ready) {
  if (payload.size() < 8 || payload.size() > 8 + kMaxVoiceNameBytes) {
    return false;
  }
  const uint32_t protocol = ReadU32(payload, 0);
  const uint32_t rate = ReadU32(payload, 4);
  if (rate < static_cast<uint32_t>(kMinSampleRate) ||
      rate > static_cast<uint32_t>(kMaxSampleRate)) {
    return false;
  }
  const std::string_view name = payload.substr(8);
  for (char c : name) {
    if (c < 0x20 || c >= 0x7F) {
      return false;
    }
  }
  ready->protocol = protocol;
  ready->sample_rate = static_cast<int>(rate);
  ready->voice.assign(name);
  return true;
}

bool ParseAudio(std::string_view payload,
                uint32_t* id,
                std::vector<int16_t>* samples) {
  if (payload.size() < 6 || payload.size() > kMaxFramePayloadBytes ||
      (payload.size() - 4) % 2 != 0) {
    return false;
  }
  *id = ReadU32(payload, 0);
  const size_t count = (payload.size() - 4) / 2;
  samples->resize(count);
  for (size_t i = 0; i < count; ++i) {
    (*samples)[i] = static_cast<int16_t>(ReadU16(payload, 4 + 2 * i));
  }
  return true;
}

bool ParseDone(std::string_view payload, DoneInfo* done) {
  if (payload.size() != kDoneBytes) {
    return false;
  }
  const uint32_t status = ReadU32(payload, 4);
  if (status > kMaxDoneStatus) {
    return false;
  }
  done->id = ReadU32(payload, 0);
  done->status = static_cast<DoneStatus>(status);
  done->audio_ms = ReadU32(payload, 8);
  done->first_audio_ms = ReadU32(payload, 12);
  done->synth_ms = ReadU32(payload, 16);
  return true;
}

bool ParseError(std::string_view payload,
                ErrorCode* code,
                std::string* message) {
  if (payload.size() < 4 || payload.size() > 4 + kMaxErrorMessageBytes) {
    return false;
  }
  const uint32_t value = ReadU32(payload, 0);
  if (value < 1 || value > kMaxErrorCode) {
    return false;
  }
  for (char c : payload.substr(4)) {
    if (c < 0x20 || c >= 0x7F) {
      return false;
    }
  }
  *code = static_cast<ErrorCode>(value);
  message->assign(payload.substr(4));
  return true;
}

}  // namespace molt_voice
