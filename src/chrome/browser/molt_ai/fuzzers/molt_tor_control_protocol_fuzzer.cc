// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's Tor control-port parsing (tor/tor_control_protocol.h) on what
// the listener on the control port sends, and on the ControlPortWriteToFile
// file. Both are only as trustworthy as whatever process got to the port
// first: the parsers must hold up against a squatter, not just Tor.
//
// The whole input is the reply (seeds are plain control-port replies).
// Checks: no memory or UB error; a reply's verdict is final once it is
// complete (bytes after the end line can't change it, as the connection
// stops reading there); success implies the documented shape (an OK
// reply's status is 250, PROTOCOLINFO needs an OK reply, AUTHCHALLENGE
// fields have their fixed sizes, ports are 1..65535 on 127.0.0.1, a GETINFO
// value is one line); hex round-trips; SAFECOOKIE accepts exactly the
// server hash for these inputs and nothing a byte off. Framing of a stream
// of replies and events (CompleteReplyLength): it agrees with
// IsCompleteReply(), a reply taken off the front is complete on its own and
// gives the verdict the whole buffer gives, splitting always progresses,
// and an event (IsAsyncEvent, IsNewnymAppliedEvent) is a complete reply.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/check.h"
#include "chrome/browser/molt_ai/tor/tor_control_protocol.h"

namespace {

namespace control = molt_ai::tor::control;

bool IsUpperHex(std::string_view s) {
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

std::string Upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

// Bytes [offset, offset + size) of |input|, cycled; |input| must not be
// empty. Gives the SAFECOOKIE check fixed-size cookies and nonces that
// still come from the input.
std::string Slice(std::string_view input, size_t offset, size_t size) {
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out.push_back(input[(offset + i) % input.size()]);
  }
  return out;
}

void CheckPort(std::optional<uint16_t> port) {
  if (port) {
    CHECK(*port >= 1);
  }
}

// Every verdict a caller takes from |reply|.
struct Verdict {
  bool complete;
  int status;
  bool ok;
  std::optional<control::ProtocolInfo> protocol_info;
  std::optional<control::AuthChallenge> challenge;
  std::optional<uint16_t> socks_port;
  std::optional<std::string> version;

  explicit Verdict(std::string_view reply)
      : complete(control::IsCompleteReply(reply)),
        status(control::GetReplyStatus(reply)),
        ok(control::IsOkReply(reply)),
        protocol_info(control::ParseProtocolInfo(reply)),
        challenge(control::ParseAuthChallenge(reply)),
        socks_port(control::ParseSocksListenerReply(reply)),
        version(control::ParseGetInfoValue(reply, "version")) {}

  bool operator==(const Verdict& o) const {
    auto same_info = [](const std::optional<control::ProtocolInfo>& a,
                        const std::optional<control::ProtocolInfo>& b) {
      if (a.has_value() != b.has_value()) {
        return false;
      }
      return !a || (a->null_auth == b->null_auth &&
                    a->hashed_password_auth == b->hashed_password_auth &&
                    a->cookie_auth == b->cookie_auth &&
                    a->safe_cookie_auth == b->safe_cookie_auth &&
                    a->cookie_file == b->cookie_file &&
                    a->tor_version == b->tor_version);
    };
    auto same_challenge = [](const std::optional<control::AuthChallenge>& a,
                             const std::optional<control::AuthChallenge>& b) {
      if (a.has_value() != b.has_value()) {
        return false;
      }
      return !a || (a->server_hash == b->server_hash &&
                    a->server_nonce == b->server_nonce);
    };
    return complete == o.complete && status == o.status && ok == o.ok &&
           same_info(protocol_info, o.protocol_info) &&
           same_challenge(challenge, o.challenge) &&
           socks_port == o.socks_port && version == o.version;
  }
};

void CheckReply(std::string_view reply) {
  const Verdict v(reply);
  CHECK(v.status == -1 || (v.status >= 0 && v.status <= 999));
  if (v.status != -1) {
    CHECK(v.complete) << "a status from an incomplete reply";
  }
  if (v.ok) {
    CHECK(v.status == 250) << "an OK reply whose end line isn't 250";
  }
  if (v.protocol_info) {
    CHECK(v.ok) << "PROTOCOLINFO from a reply that isn't OK";
  }
  if (v.challenge) {
    CHECK(v.status == 250);
    CHECK(reply.substr(0, 18) == "250 AUTHCHALLENGE ");
    CHECK(v.challenge->server_hash.size() == control::kHashSize);
    CHECK(v.challenge->server_nonce.size() == control::kNonceSize);
  }
  CheckPort(v.socks_port);
  if (v.socks_port) {
    CHECK(v.status == 250);
  }
  if (v.version) {
    CHECK(v.status == 250);
    CHECK(v.version->find('\n') == std::string::npos)
        << "a GETINFO value spanning lines";
  }

  // The connection stops reading at the end line: once a prefix is
  // complete, what follows it must not change any verdict.
  if (v.complete) {
    CHECK(Verdict(std::string(reply) + "250 OK\r\n") == v)
        << "bytes after the end line changed the verdict";
    CHECK(Verdict(std::string(reply) + "550 no\r\n.\r\n") == v);
  }
  for (size_t cut : {reply.size() / 3, reply.size() / 2}) {
    const std::string_view prefix = reply.substr(0, cut);
    const Verdict p(prefix);
    if (p.complete) {
      CHECK(p == v) << "a complete prefix and the whole reply disagree";
    }
  }
}

void CheckFraming(std::string_view input) {
  const size_t length = control::CompleteReplyLength(input);
  CHECK(length <= input.size());
  CHECK((length > 0) == control::IsCompleteReply(input))
      << "framing and IsCompleteReply() disagree";
  if (length > 0) {
    CHECK(input[length - 1] == '\n') << "a reply that doesn't end a line";
    const std::string_view first = input.substr(0, length);
    CHECK(control::CompleteReplyLength(first) == length);
    CHECK(control::GetReplyStatus(first) == control::GetReplyStatus(input));
    CHECK(control::IsOkReply(first) == control::IsOkReply(input));
    CHECK(control::IsAsyncEvent(first) == control::IsAsyncEvent(input));
  }
  if (control::IsAsyncEvent(input)) {
    CHECK(length > 0);
  }
  if (control::IsNewnymAppliedEvent(input)) {
    CHECK(control::IsAsyncEvent(input));
    CHECK(length == input.size());
  }
  // Taking replies off the front of a stream ends.
  std::string_view rest = input;
  size_t pieces = 0;
  while (const size_t n = control::CompleteReplyLength(rest)) {
    CHECK(n <= rest.size());
    rest.remove_prefix(n);
    CHECK(++pieces <= input.size());
  }
  CHECK(!control::IsCompleteReply(rest));
}

void CheckHex(std::string_view input) {
  const std::string hex = control::HexEncode(input);
  CHECK(hex.size() == 2 * input.size());
  CHECK(IsUpperHex(hex));
  const std::optional<std::string> back = control::HexDecode(hex);
  CHECK(back && *back == input) << "hex doesn't round-trip";
  if (const std::optional<std::string> decoded = control::HexDecode(input)) {
    CHECK(control::HexEncode(*decoded) == Upper(input));
  }
}

void CheckSafeCookie(std::string_view input) {
  if (input.empty()) {
    return;
  }
  const std::string cookie = Slice(input, 0, control::kCookieSize);
  const std::string client_nonce = Slice(input, 1, control::kNonceSize);
  const std::string server_nonce = Slice(input, 2, control::kNonceSize);
  const std::string server_hash = control::ComputeSafeCookieHash(
      control::kServerToControllerHashKey, cookie, client_nonce, server_nonce);
  CHECK(server_hash.size() == control::kHashSize);
  CHECK(control::VerifySafeCookieServerHash(cookie, client_nonce,
                                            server_nonce, server_hash));

  // The controller-to-server hash must never pass as the server's.
  const std::string ours = control::ComputeSafeCookieHash(
      control::kControllerToServerHashKey, cookie, client_nonce, server_nonce);
  CHECK(ours != server_hash);
  CHECK(!control::VerifySafeCookieServerHash(cookie, client_nonce,
                                             server_nonce, ours));

  // One byte off anywhere, or a wrong size, fails.
  const size_t at = static_cast<unsigned char>(input[0]) % control::kHashSize;
  const char flip = static_cast<char>((input.size() % 255) + 1);
  std::string bad_hash = server_hash;
  bad_hash[at] ^= flip;
  CHECK(!control::VerifySafeCookieServerHash(cookie, client_nonce,
                                             server_nonce, bad_hash));
  std::string bad_cookie = cookie;
  bad_cookie[at] ^= flip;
  CHECK(!control::VerifySafeCookieServerHash(bad_cookie, client_nonce,
                                             server_nonce, server_hash));
  std::string bad_nonce = server_nonce;
  bad_nonce[at] ^= flip;
  CHECK(!control::VerifySafeCookieServerHash(cookie, client_nonce, bad_nonce,
                                             server_hash));
  CHECK(!control::VerifySafeCookieServerHash(cookie, client_nonce,
                                             server_nonce,
                                             server_hash.substr(1)));
  CHECK(!control::VerifySafeCookieServerHash(cookie.substr(1), client_nonce,
                                             server_nonce, server_hash));

  // The AUTHCHALLENGE reply Tor would send parses back to the same bytes.
  const std::string reply = "250 AUTHCHALLENGE SERVERHASH=" +
                            control::HexEncode(server_hash) +
                            " SERVERNONCE=" +
                            control::HexEncode(server_nonce) + "\r\n";
  const std::optional<control::AuthChallenge> challenge =
      control::ParseAuthChallenge(reply);
  CHECK(challenge && challenge->server_hash == server_hash &&
        challenge->server_nonce == server_nonce);

  const std::string command = control::BuildSafeCookieAuthenticateCommand(
      cookie, client_nonce, server_nonce);
  CHECK(command == "AUTHENTICATE " + control::HexEncode(ours));
  CHECK(control::BuildAuthChallengeCommand(client_nonce) ==
        "AUTHCHALLENGE SAFECOOKIE " + control::HexEncode(client_nonce));
}

// Whether some whole line of |contents| is "PORT=127.0.0.1:<port>" (leading
// zeros allowed: still one decimal number).
bool HasPortLine(std::string_view contents, uint16_t port) {
  constexpr std::string_view kPrefix = "PORT=127.0.0.1:";
  size_t start = 0;
  while (start <= contents.size()) {
    size_t end = contents.find('\n', start);
    std::string_view line = contents.substr(
        start, end == std::string_view::npos ? std::string_view::npos
                                             : end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.substr(0, kPrefix.size()) == kPrefix) {
      const std::string_view digits = line.substr(kPrefix.size());
      uint32_t value = 0;
      bool ok = !digits.empty() && digits.size() <= 5;
      for (char c : digits) {
        ok &= c >= '0' && c <= '9';
        value = value * 10 + static_cast<uint32_t>(c - '0');
      }
      if (ok && value == port) {
        return true;
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}

void CheckControlPortFile(std::string_view contents) {
  const std::optional<uint16_t> port = control::ParseControlPortFile(contents);
  CheckPort(port);
  if (port) {
    CHECK(HasPortLine(contents, *port))
        << "a control port that isn't a line of the file";
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  CheckReply(input);
  CheckFraming(input);
  CheckControlPortFile(input);
  CheckHex(input.substr(0, 256));
  CheckSafeCookie(input);
  return 0;
}
