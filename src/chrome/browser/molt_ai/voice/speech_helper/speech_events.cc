// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/speech_helper/speech_events.h"

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/speech_helper/speech_text.h"

namespace molt_speech {

namespace {

std::string Timed(std::string_view type, const EventTime& time) {
  return "{\"type\":\"" + std::string(type) +
         "\",\"epoch\":" + std::to_string(time.epoch) +
         ",\"audio_ms\":" + std::to_string(time.audio_ms);
}

std::string PrintableAscii(std::string_view text, size_t max_bytes) {
  std::string out;
  for (const char c : text) {
    if (out.size() == max_bytes) {
      break;
    }
    if (c >= 0x20 && c < 0x7F) {
      out.push_back(c);
    }
  }
  return out;
}

// "0.93": two decimals, without the C library's locale.
std::string TwoDecimals(double value) {
  if (!(value > 0)) {  // Also NaN.
    value = 0;
  }
  if (value > 1) {
    value = 1;
  }
  const int hundredths = static_cast<int>(value * 100 + 0.5);
  std::string out = std::to_string(hundredths / 100) + ".";
  out.push_back(static_cast<char>('0' + (hundredths % 100) / 10));
  out.push_back(static_cast<char>('0' + hundredths % 10));
  return out;
}

}  // namespace

std::string JsonQuote(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (u < 0x20 || u == 0x7F) {
      out += "\\u00";
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0xF]);
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::string ReadyEvent(std::string_view engine,
                       const std::vector<std::string>& models) {
  std::string line = "{\"type\":\"ready\",\"protocol\":" +
                     std::to_string(kProtocolVersion) + ",\"engine\":" +
                     JsonQuote(PrintableAscii(engine, 200)) + ",\"models\":[";
  for (size_t i = 0; i < models.size() && i < 8; ++i) {
    if (i > 0) {
      line.push_back(',');
    }
    line += JsonQuote(PrintableAscii(models[i], 100));
  }
  line += "],\"sample_rate\":16000}\n";
  return line;
}

std::string WakeEvent(const EventTime& time) {
  return Timed("wake", time) + "}\n";
}

std::string SpeechStartEvent(const EventTime& time) {
  return Timed("speech_start", time) + "}\n";
}

std::string PartialEvent(const EventTime& time, std::string_view text) {
  return Timed("partial", time) + ",\"text\":" +
         JsonQuote(CleanEventText(text, kMaxEventTextBytes)) + "}\n";
}

std::string SpeechEndEvent(const EventTime& time) {
  return Timed("speech_end", time) + "}\n";
}

std::string FinalEvent(const EventTime& time,
                       std::string_view text,
                       double confidence) {
  return Timed("final", time) + ",\"text\":" +
         JsonQuote(CleanEventText(text, kMaxEventTextBytes)) +
         ",\"confidence\":" + TwoDecimals(confidence) + "}\n";
}

std::string ErrorEvent(std::string_view code, std::string_view message) {
  return "{\"type\":\"error\",\"code\":" + JsonQuote(PrintableAscii(code, 32)) +
         ",\"message\":" +
         JsonQuote(PrintableAscii(message, kMaxErrorMessageBytes)) + "}\n";
}

}  // namespace molt_speech
