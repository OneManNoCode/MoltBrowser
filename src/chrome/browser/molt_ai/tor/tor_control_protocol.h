// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Pure (socket-free, thread-free) helpers for Tor's control protocol
// (torspec control-spec.txt): reply framing, PROTOCOLINFO, the SAFECOOKIE
// AUTHCHALLENGE handshake, and the formats Tor uses to report the ports it
// picked for `SocksPort auto` / `ControlPort auto`.
//
// Everything security-relevant about talking to the control port lives here
// so it can be unit-tested in isolation (tor_control_protocol_unittest.cc):
//   * SAFECOOKIE (control-spec §3.24): we prove knowledge of the cookie AND
//     verify Tor's SERVERHASH before sending ours, so a process squatting on
//     the control port can neither learn the cookie nor impersonate Tor.
//   * Replies are judged by their *final* status line, never by "contains
//     250" — a hostile listener can't make an error look like success.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_TOR_CONTROL_PROTOCOL_H_
#define CHROME_BROWSER_MOLT_AI_TOR_TOR_CONTROL_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>

namespace molt_ai {
namespace tor {
namespace control {

// Fixed sizes from control-spec §3.24 (AUTHCHALLENGE) and §5.1 (cookie file).
inline constexpr size_t kCookieSize = 32;
inline constexpr size_t kNonceSize = 32;
inline constexpr size_t kHashSize = 32;  // HMAC-SHA256 output.

// HMAC-SHA256 keys defined by control-spec §3.24. The message in both cases
// is CookieString | ClientNonce | ServerNonce.
inline constexpr char kServerToControllerHashKey[] =
    "Tor safe cookie authentication server-to-controller hash";
inline constexpr char kControllerToServerHashKey[] =
    "Tor safe cookie authentication controller-to-server hash";

// Uppercase hex of |bytes|.
std::string HexEncode(std::string_view bytes);

// Strict hex decode (even length, [0-9A-Fa-f] only). nullopt on any error.
std::optional<std::string> HexDecode(std::string_view hex);

// True once |buffer| contains a complete synchronous reply, i.e. an
// EndReplyLine ("NNN<SP>...CRLF") that is not inside a "+" data block. Also
// true for a malformed line, so a caller never waits forever on garbage (the
// status then parses as -1).
bool IsCompleteReply(std::string_view buffer);

// Status code of the reply's EndReplyLine (e.g. 250, 515), or -1 if |reply|
// has no well-formed end line.
int GetReplyStatus(std::string_view reply);

// True iff every status line of |reply| is a 250 line and the end line is
// exactly "250 OK".
bool IsOkReply(std::string_view reply);

// Length in bytes of the first complete reply at the start of |buffer|,
// synchronous or asynchronous (control-spec §2.3, §4.1): up to and
// including the "\n" of its EndReplyLine, "+" data blocks included. 0 while
// |buffer| doesn't hold a whole reply yet. As in IsCompleteReply(), a
// malformed line ends a reply, so a reader never waits on garbage.
size_t CompleteReplyLength(std::string_view buffer);

// True iff |reply| is an asynchronous event (status 650, control-spec
// §4.1), which Tor sends between synchronous replies once a connection has
// asked for events with SETEVENTS.
bool IsAsyncEvent(std::string_view reply);

// True iff |reply| is exactly "650 SIGNAL NEWNYM", the SIGNAL event Tor
// sends when it has applied a NEWNYM. Tor answers SIGNAL NEWNYM with
// "250 OK" at once, but within ten seconds of the previous one it only
// applies it when that time is up, and new streams keep using the old
// circuits until then; this event says the switch has happened.
bool IsNewnymAppliedEvent(std::string_view reply);

// Parsed PROTOCOLINFO reply (control-spec §3.21).
struct ProtocolInfo {
  ProtocolInfo();
  ProtocolInfo(const ProtocolInfo&);
  ProtocolInfo& operator=(const ProtocolInfo&);
  ~ProtocolInfo();

  bool null_auth = false;
  bool hashed_password_auth = false;
  bool cookie_auth = false;
  bool safe_cookie_auth = false;
  std::string cookie_file;  // Unescaped COOKIEFILE, if present.
  std::string tor_version;  // VERSION Tor="...", if present.
};
std::optional<ProtocolInfo> ParseProtocolInfo(std::string_view reply);

// "AUTHCHALLENGE SAFECOOKIE <hex client nonce>". |client_nonce| must be
// kNonceSize raw bytes.
std::string BuildAuthChallengeCommand(std::string_view client_nonce);

// Parsed "250 AUTHCHALLENGE SERVERHASH=<hex> SERVERNONCE=<hex>". Both fields
// are returned as raw bytes (kHashSize / kNonceSize). nullopt unless the reply
// is a single 250 line carrying both well-formed fields.
struct AuthChallenge {
  std::string server_hash;
  std::string server_nonce;
};
std::optional<AuthChallenge> ParseAuthChallenge(std::string_view reply);

// HMAC-SHA256(|key|, cookie | client_nonce | server_nonce), raw 32 bytes.
std::string ComputeSafeCookieHash(std::string_view key,
                                  std::string_view cookie,
                                  std::string_view client_nonce,
                                  std::string_view server_nonce);

// Constant-time check that |server_hash| is the server-to-controller HMAC for
// these inputs. False on any size mismatch.
bool VerifySafeCookieServerHash(std::string_view cookie,
                                std::string_view client_nonce,
                                std::string_view server_nonce,
                                std::string_view server_hash);

// "AUTHENTICATE <hex controller-to-server HMAC>".
std::string BuildSafeCookieAuthenticateCommand(std::string_view cookie,
                                               std::string_view client_nonce,
                                               std::string_view server_nonce);

// Parses the file written by `ControlPortWriteToFile` ("PORT=127.0.0.1:N").
// Only an IPv4-loopback listener is accepted. nullopt if none.
std::optional<uint16_t> ParseControlPortFile(std::string_view contents);

// Parses the reply to "GETINFO net/listeners/socks" (a list of quoted
// "addr:port" strings) and returns the first IPv4-loopback port.
std::optional<uint16_t> ParseSocksListenerReply(std::string_view reply);

// Value of a single-line GETINFO answer "250-<key>=<value>" (or the end-line
// form "250 <key>=<value>"). nullopt if |key| isn't present.
std::optional<std::string> ParseGetInfoValue(std::string_view reply,
                                             std::string_view key);

}  // namespace control
}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_TOR_CONTROL_PROTOCOL_H_
