// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/tor_control_protocol.h"

#include <array>
#include <vector>

#include "base/containers/span.h"
#include "crypto/hmac.h"

namespace molt_ai {
namespace tor {
namespace control {

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

// Splits |text| into lines terminated by "\n" (a preceding "\r" is dropped).
// A trailing fragment without a terminator is NOT returned: it is an
// incomplete line still arriving on the socket.
std::vector<std::string_view> CompleteLines(std::string_view text) {
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      break;
    }
    std::string_view line = text.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.push_back(line);
    start = nl + 1;
  }
  return lines;
}

// A status line is "NNN" followed by '-', '+' or ' '.
bool IsStatusLine(std::string_view line) {
  return line.size() >= 4 && IsDigit(line[0]) && IsDigit(line[1]) &&
         IsDigit(line[2]) &&
         (line[3] == '-' || line[3] == '+' || line[3] == ' ');
}

int StatusOf(std::string_view line) {
  return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

// The status lines of |reply| (data-block contents skipped), stopping after
// the end line. |complete| reports whether an end line was seen.
std::vector<std::string_view> StatusLines(std::string_view reply,
                                          bool* complete) {
  std::vector<std::string_view> out;
  *complete = false;
  bool in_data = false;
  for (std::string_view line : CompleteLines(reply)) {
    if (in_data) {
      if (line == ".") {
        in_data = false;
      }
      continue;
    }
    if (!IsStatusLine(line)) {
      // Malformed: stop here. Callers treat this as a failed reply.
      out.push_back(line);
      *complete = true;
      return out;
    }
    out.push_back(line);
    if (line[3] == '+') {
      in_data = true;
    } else if (line[3] == ' ') {
      *complete = true;
      return out;
    }
  }
  return out;
}

// Parses a control-spec QuotedString starting at |text[0] == '"'|. On success
// stores the unescaped contents and returns the index just past the closing
// quote; returns npos on malformed input.
size_t ParseQuotedString(std::string_view text, std::string* out) {
  out->clear();
  if (text.empty() || text[0] != '"') {
    return std::string_view::npos;
  }
  for (size_t i = 1; i < text.size(); ++i) {
    char c = text[i];
    if (c == '"') {
      return i + 1;
    }
    if (c == '\\') {
      if (i + 1 >= text.size()) {
        return std::string_view::npos;
      }
      char e = text[++i];
      switch (e) {
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        default:
          out->push_back(e);
          break;
      }
      continue;
    }
    out->push_back(c);
  }
  return std::string_view::npos;
}

// Parses "127.0.0.1:<port>" strictly. Anything other than the IPv4 loopback
// literal (or a port outside 1..65535) is rejected.
std::optional<uint16_t> ParseLoopbackHostPort(std::string_view addr) {
  constexpr std::string_view kPrefix = "127.0.0.1:";
  if (addr.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  std::string_view port = addr.substr(kPrefix.size());
  if (port.empty() || port.size() > 5) {
    return std::nullopt;
  }
  uint32_t value = 0;
  for (char c : port) {
    if (!IsDigit(c)) {
      return std::nullopt;
    }
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  if (value == 0 || value > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(value);
}

// Returns the text after "<key>=" in a "250-"/"250 " line, or nullopt.
std::optional<std::string_view> FindGetInfoLine(std::string_view reply,
                                                std::string_view key) {
  bool complete = false;
  for (std::string_view line : StatusLines(reply, &complete)) {
    if (!IsStatusLine(line) || StatusOf(line) != 250 || line[3] == '+') {
      continue;
    }
    std::string_view rest = line.substr(4);
    if (rest.size() > key.size() && rest.substr(0, key.size()) == key &&
        rest[key.size()] == '=') {
      return rest.substr(key.size() + 1);
    }
  }
  return std::nullopt;
}

}  // namespace

ProtocolInfo::ProtocolInfo() = default;
ProtocolInfo::ProtocolInfo(const ProtocolInfo&) = default;
ProtocolInfo& ProtocolInfo::operator=(const ProtocolInfo&) = default;
ProtocolInfo::~ProtocolInfo() = default;

std::string HexEncode(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out.push_back(kHexDigits[c >> 4]);
    out.push_back(kHexDigits[c & 0x0F]);
  }
  return out;
}

std::optional<std::string> HexDecode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return std::nullopt;
  }
  std::string out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = HexValue(hex[i]);
    int lo = HexValue(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

bool IsCompleteReply(std::string_view buffer) {
  bool complete = false;
  StatusLines(buffer, &complete);
  return complete;
}

int GetReplyStatus(std::string_view reply) {
  bool complete = false;
  std::vector<std::string_view> lines = StatusLines(reply, &complete);
  if (!complete || lines.empty()) {
    return -1;
  }
  std::string_view end = lines.back();
  if (!IsStatusLine(end) || end[3] != ' ') {
    return -1;
  }
  return StatusOf(end);
}

bool IsOkReply(std::string_view reply) {
  bool complete = false;
  std::vector<std::string_view> lines = StatusLines(reply, &complete);
  if (!complete || lines.empty()) {
    return false;
  }
  for (std::string_view line : lines) {
    if (!IsStatusLine(line) || StatusOf(line) != 250) {
      return false;
    }
  }
  return lines.back() == "250 OK";
}

std::optional<ProtocolInfo> ParseProtocolInfo(std::string_view reply) {
  if (!IsOkReply(reply)) {
    return std::nullopt;
  }
  ProtocolInfo info;
  bool saw_header = false;
  bool complete = false;
  for (std::string_view line : StatusLines(reply, &complete)) {
    std::string_view rest = line.substr(4);
    if (rest.substr(0, 13) == "PROTOCOLINFO ") {
      saw_header = true;
    } else if (rest.substr(0, 13) == "AUTH METHODS=") {
      std::string_view methods = rest.substr(13);
      size_t sp = methods.find(' ');
      std::string_view tail;
      if (sp != std::string_view::npos) {
        tail = methods.substr(sp + 1);
        methods = methods.substr(0, sp);
      }
      while (!methods.empty()) {
        size_t comma = methods.find(',');
        std::string_view m = methods.substr(0, comma);
        if (m == "NULL") {
          info.null_auth = true;
        } else if (m == "HASHEDPASSWORD") {
          info.hashed_password_auth = true;
        } else if (m == "COOKIE") {
          info.cookie_auth = true;
        } else if (m == "SAFECOOKIE") {
          info.safe_cookie_auth = true;
        }
        if (comma == std::string_view::npos) {
          break;
        }
        methods.remove_prefix(comma + 1);
      }
      constexpr std::string_view kCookieFile = "COOKIEFILE=";
      if (tail.substr(0, kCookieFile.size()) == kCookieFile) {
        if (ParseQuotedString(tail.substr(kCookieFile.size()),
                              &info.cookie_file) == std::string_view::npos) {
          return std::nullopt;
        }
      }
    } else if (rest.substr(0, 13) == "VERSION Tor=\"") {
      if (ParseQuotedString(rest.substr(12), &info.tor_version) ==
          std::string_view::npos) {
        return std::nullopt;
      }
    }
  }
  if (!saw_header) {
    return std::nullopt;
  }
  return info;
}

std::string BuildAuthChallengeCommand(std::string_view client_nonce) {
  return "AUTHCHALLENGE SAFECOOKIE " + HexEncode(client_nonce);
}

std::optional<AuthChallenge> ParseAuthChallenge(std::string_view reply) {
  bool complete = false;
  std::vector<std::string_view> lines = StatusLines(reply, &complete);
  // Exactly one line: "250 AUTHCHALLENGE SERVERHASH=... SERVERNONCE=...".
  if (!complete || lines.size() != 1) {
    return std::nullopt;
  }
  std::string_view line = lines[0];
  constexpr std::string_view kPrefix = "250 AUTHCHALLENGE ";
  if (line.substr(0, kPrefix.size()) != kPrefix) {
    return std::nullopt;
  }
  std::string_view rest = line.substr(kPrefix.size());
  std::optional<std::string> hash;
  std::optional<std::string> nonce;
  while (!rest.empty()) {
    size_t sp = rest.find(' ');
    std::string_view token = rest.substr(0, sp);
    rest = sp == std::string_view::npos ? std::string_view()
                                        : rest.substr(sp + 1);
    if (token.substr(0, 11) == "SERVERHASH=") {
      hash = HexDecode(token.substr(11));
    } else if (token.substr(0, 12) == "SERVERNONCE=") {
      nonce = HexDecode(token.substr(12));
    } else {
      return std::nullopt;
    }
  }
  if (!hash || !nonce || hash->size() != kHashSize ||
      nonce->size() != kNonceSize) {
    return std::nullopt;
  }
  AuthChallenge challenge;
  challenge.server_hash = std::move(*hash);
  challenge.server_nonce = std::move(*nonce);
  return challenge;
}

std::string ComputeSafeCookieHash(std::string_view key,
                                  std::string_view cookie,
                                  std::string_view client_nonce,
                                  std::string_view server_nonce) {
  std::string message;
  message.reserve(cookie.size() + client_nonce.size() + server_nonce.size());
  message.append(cookie);
  message.append(client_nonce);
  message.append(server_nonce);
  std::array<uint8_t, kHashSize> mac = crypto::hmac::SignSha256(
      base::as_byte_span(key), base::as_byte_span(message));
  return std::string(mac.begin(), mac.end());
}

bool VerifySafeCookieServerHash(std::string_view cookie,
                                std::string_view client_nonce,
                                std::string_view server_nonce,
                                std::string_view server_hash) {
  if (cookie.size() != kCookieSize || client_nonce.size() != kNonceSize ||
      server_nonce.size() != kNonceSize || server_hash.size() != kHashSize) {
    return false;
  }
  std::string message;
  message.reserve(cookie.size() + client_nonce.size() + server_nonce.size());
  message.append(cookie);
  message.append(client_nonce);
  message.append(server_nonce);
  return crypto::hmac::VerifySha256(
      base::as_byte_span(std::string_view(kServerToControllerHashKey)),
      base::as_byte_span(message),
      base::as_byte_span(server_hash).first<kHashSize>());
}

std::string BuildSafeCookieAuthenticateCommand(std::string_view cookie,
                                               std::string_view client_nonce,
                                               std::string_view server_nonce) {
  return "AUTHENTICATE " +
         HexEncode(ComputeSafeCookieHash(kControllerToServerHashKey, cookie,
                                         client_nonce, server_nonce));
}

std::optional<uint16_t> ParseControlPortFile(std::string_view contents) {
  // Terminate the last line so CompleteLines() returns it too. Kept in a
  // named local: the string_views below point into it.
  const std::string terminated = std::string(contents) + "\n";
  for (std::string_view line : CompleteLines(terminated)) {
    constexpr std::string_view kPrefix = "PORT=";
    if (line.substr(0, kPrefix.size()) != kPrefix) {
      continue;
    }
    if (std::optional<uint16_t> port =
            ParseLoopbackHostPort(line.substr(kPrefix.size()))) {
      return port;
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> ParseSocksListenerReply(std::string_view reply) {
  if (GetReplyStatus(reply) != 250) {
    return std::nullopt;
  }
  std::optional<std::string_view> value =
      FindGetInfoLine(reply, "net/listeners/socks");
  if (!value) {
    return std::nullopt;
  }
  std::string_view rest = *value;
  while (!rest.empty()) {
    if (rest[0] == ' ') {
      rest.remove_prefix(1);
      continue;
    }
    std::string entry;
    size_t consumed = ParseQuotedString(rest, &entry);
    if (consumed == std::string_view::npos) {
      return std::nullopt;
    }
    if (std::optional<uint16_t> port = ParseLoopbackHostPort(entry)) {
      return port;
    }
    rest.remove_prefix(consumed);
  }
  return std::nullopt;
}

std::optional<std::string> ParseGetInfoValue(std::string_view reply,
                                             std::string_view key) {
  if (GetReplyStatus(reply) != 250) {
    return std::nullopt;
  }
  std::optional<std::string_view> value = FindGetInfoLine(reply, key);
  if (!value) {
    return std::nullopt;
  }
  return std::string(*value);
}

}  // namespace control
}  // namespace tor
}  // namespace molt_ai
