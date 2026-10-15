// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/tor_control_protocol.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace control {
namespace {

std::string Bytes(int first, int count) {
  std::string out;
  for (int i = 0; i < count; ++i) {
    out.push_back(static_cast<char>(first + i));
  }
  return out;
}

// SAFECOOKIE vectors (control-spec §3.24). Inputs: cookie = 00..1F,
// client nonce = 20..3F, server nonce = 40..5F. Expected values computed
// independently with Python's hmac module and cross-checked with
// `openssl dgst -sha256 -hmac`:
//   HMAC-SHA256("Tor safe cookie authentication server-to-controller hash",
//               cookie|client_nonce|server_nonce)
constexpr char kServerHashHex[] =
    "3C8780AB52365C0D080750447E5F64DABC00428C6C434579C2043E18C1F85389";
constexpr char kClientHashHex[] =
    "B47642DF2D5ABB84F69E6D02D41BED6B44AEE33E69562528A82166FC98BC0B1E";

TEST(TorControlProtocolTest, SafeCookieServerHashMatchesVector) {
  const std::string cookie = Bytes(0x00, 32);
  const std::string client_nonce = Bytes(0x20, 32);
  const std::string server_nonce = Bytes(0x40, 32);
  EXPECT_EQ(kServerHashHex,
            HexEncode(ComputeSafeCookieHash(kServerToControllerHashKey,
                                            cookie, client_nonce,
                                            server_nonce)));
  std::optional<std::string> expected = HexDecode(kServerHashHex);
  ASSERT_TRUE(expected);
  EXPECT_TRUE(VerifySafeCookieServerHash(cookie, client_nonce, server_nonce,
                                         *expected));
}

TEST(TorControlProtocolTest, SafeCookieClientHashMatchesVector) {
  EXPECT_EQ(std::string("AUTHENTICATE ") + kClientHashHex,
            BuildSafeCookieAuthenticateCommand(
                Bytes(0x00, 32), Bytes(0x20, 32), Bytes(0x40, 32)));
}

TEST(TorControlProtocolTest, SafeCookieRejectsWrongServerHash) {
  const std::string cookie = Bytes(0x00, 32);
  const std::string client_nonce = Bytes(0x20, 32);
  const std::string server_nonce = Bytes(0x40, 32);
  std::string hash = *HexDecode(kServerHashHex);
  hash[0] ^= 0x01;
  EXPECT_FALSE(
      VerifySafeCookieServerHash(cookie, client_nonce, server_nonce, hash));
  // A squatter who doesn't know the cookie computes the hash over a
  // different cookie: must not verify.
  EXPECT_FALSE(VerifySafeCookieServerHash(Bytes(0x01, 32), client_nonce,
                                          server_nonce,
                                          *HexDecode(kServerHashHex)));
  // The client-direction HMAC must not be accepted as the server's.
  EXPECT_FALSE(VerifySafeCookieServerHash(cookie, client_nonce, server_nonce,
                                          *HexDecode(kClientHashHex)));
  // Size mismatches.
  EXPECT_FALSE(VerifySafeCookieServerHash(cookie, client_nonce, server_nonce,
                                          hash.substr(0, 31)));
  EXPECT_FALSE(VerifySafeCookieServerHash(cookie.substr(0, 31), client_nonce,
                                          server_nonce,
                                          *HexDecode(kServerHashHex)));
}

TEST(TorControlProtocolTest, AuthChallengeCommand) {
  EXPECT_EQ(
      "AUTHCHALLENGE SAFECOOKIE "
      "202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F",
      BuildAuthChallengeCommand(Bytes(0x20, 32)));
}

TEST(TorControlProtocolTest, ParseAuthChallenge) {
  const std::string reply = std::string("250 AUTHCHALLENGE SERVERHASH=") +
                            kServerHashHex +
                            " SERVERNONCE="
                            "404142434445464748494A4B4C4D4E4F"
                            "505152535455565758595A5B5C5D5E5F\r\n";
  std::optional<AuthChallenge> c = ParseAuthChallenge(reply);
  ASSERT_TRUE(c);
  EXPECT_EQ(*HexDecode(kServerHashHex), c->server_hash);
  EXPECT_EQ(Bytes(0x40, 32), c->server_nonce);
}

TEST(TorControlProtocolTest, ParseAuthChallengeRejectsMalformed) {
  const std::string nonce =
      "404142434445464748494A4B4C4D4E4F505152535455565758595A5B5C5D5E5F";
  // Error status.
  EXPECT_FALSE(ParseAuthChallenge("515 Authentication failed\r\n"));
  // Missing nonce.
  EXPECT_FALSE(ParseAuthChallenge(std::string("250 AUTHCHALLENGE SERVERHASH=") +
                                  kServerHashHex + "\r\n"));
  // Short hash.
  EXPECT_FALSE(ParseAuthChallenge(
      "250 AUTHCHALLENGE SERVERHASH=ABCD SERVERNONCE=" + nonce + "\r\n"));
  // Non-hex.
  EXPECT_FALSE(ParseAuthChallenge(
      std::string("250 AUTHCHALLENGE SERVERHASH=") +
      std::string(64, 'Z') + " SERVERNONCE=" + nonce + "\r\n"));
  // Unknown token.
  EXPECT_FALSE(ParseAuthChallenge(std::string("250 AUTHCHALLENGE SERVERHASH=") +
                                  kServerHashHex + " SERVERNONCE=" + nonce +
                                  " EXTRA=1\r\n"));
  // Multi-line reply.
  EXPECT_FALSE(ParseAuthChallenge(
      std::string("250-AUTHCHALLENGE SERVERHASH=") + kServerHashHex +
      " SERVERNONCE=" + nonce + "\r\n250 OK\r\n"));
  // Incomplete (no CRLF yet).
  EXPECT_FALSE(ParseAuthChallenge(std::string("250 AUTHCHALLENGE SERVERHASH=") +
                                  kServerHashHex + " SERVERNONCE=" + nonce));
}

TEST(TorControlProtocolTest, HexRoundTrip) {
  EXPECT_EQ("00FF10", HexEncode(std::string("\x00\xff\x10", 3)));
  EXPECT_EQ(std::string("\x00\xff\x10", 3), *HexDecode("00ff10"));
  EXPECT_FALSE(HexDecode("0"));
  EXPECT_FALSE(HexDecode("0G"));
}

TEST(TorControlProtocolTest, ReplyFraming) {
  EXPECT_FALSE(IsCompleteReply(""));
  EXPECT_FALSE(IsCompleteReply("250-version=0.4.8.10\r\n"));
  EXPECT_FALSE(IsCompleteReply("250 OK"));  // No CRLF yet.
  EXPECT_TRUE(IsCompleteReply("250 OK\r\n"));
  EXPECT_TRUE(IsCompleteReply("250-version=0.4.8.10\r\n250 OK\r\n"));
  // A "250 OK"-looking line inside a data block is NOT the end.
  const std::string data =
      "250+circuit-status=\r\n1 BUILT $A~a\r\n250 OK\r\n";
  EXPECT_FALSE(IsCompleteReply(data));
  EXPECT_TRUE(IsCompleteReply(data + ".\r\n250 OK\r\n"));
  // Garbage completes (so callers stop reading) but is never OK.
  EXPECT_TRUE(IsCompleteReply("HTTP/1.0 400 Bad\r\n"));
  EXPECT_EQ(-1, GetReplyStatus("HTTP/1.0 400 Bad\r\n"));
}

TEST(TorControlProtocolTest, ReplyStatusAndOk) {
  EXPECT_EQ(250, GetReplyStatus("250 OK\r\n"));
  EXPECT_TRUE(IsOkReply("250 OK\r\n"));
  EXPECT_EQ(515, GetReplyStatus("515 Authentication failed\r\n"));
  EXPECT_FALSE(IsOkReply("515 Authentication failed\r\n"));
  // The old code treated anything starting with "250" as auth success.
  EXPECT_FALSE(IsOkReply("250 something else\r\n"));
  EXPECT_FALSE(IsOkReply("250-OK\r\n552 Unrecognized\r\n"));
  EXPECT_FALSE(IsOkReply("650 STATUS_GENERAL\r\n"));
  EXPECT_TRUE(IsOkReply("250-a=1\r\n250 OK\r\n"));
}

TEST(TorControlProtocolTest, ParseProtocolInfo) {
  const char kReply[] =
      "250-PROTOCOLINFO 1\r\n"
      "250-AUTH METHODS=COOKIE,SAFECOOKIE "
      "COOKIEFILE=\"C:\\\\Users\\\\a b\\\\tor\\\\control_auth_cookie\"\r\n"
      "250-VERSION Tor=\"0.4.8.13\"\r\n"
      "250 OK\r\n";
  std::optional<ProtocolInfo> info = ParseProtocolInfo(kReply);
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->cookie_auth);
  EXPECT_TRUE(info->safe_cookie_auth);
  EXPECT_FALSE(info->null_auth);
  EXPECT_FALSE(info->hashed_password_auth);
  EXPECT_EQ("C:\\Users\\a b\\tor\\control_auth_cookie", info->cookie_file);
  EXPECT_EQ("0.4.8.13", info->tor_version);
}

TEST(TorControlProtocolTest, ParseProtocolInfoNullAuthAndErrors) {
  std::optional<ProtocolInfo> info = ParseProtocolInfo(
      "250-PROTOCOLINFO 1\r\n250-AUTH METHODS=NULL\r\n250 OK\r\n");
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->null_auth);
  EXPECT_FALSE(info->safe_cookie_auth);
  EXPECT_FALSE(ParseProtocolInfo("514 Authentication required.\r\n"));
  EXPECT_FALSE(ParseProtocolInfo("250 OK\r\n"));  // No PROTOCOLINFO line.
  EXPECT_FALSE(ParseProtocolInfo(
      "250-PROTOCOLINFO 1\r\n250-AUTH METHODS=COOKIE "
      "COOKIEFILE=\"unterminated\r\n250 OK\r\n"));
}

TEST(TorControlProtocolTest, ParseControlPortFile) {
  EXPECT_EQ(9151, ParseControlPortFile("PORT=127.0.0.1:9151\n"));
  EXPECT_EQ(41234, ParseControlPortFile("PORT=127.0.0.1:41234"));
  EXPECT_EQ(5000, ParseControlPortFile(
                      "UNIX_PORT=/tmp/x\r\nPORT=127.0.0.1:5000\r\n"));
  EXPECT_FALSE(ParseControlPortFile("PORT=0.0.0.0:9151\n"));
  EXPECT_FALSE(ParseControlPortFile("PORT=10.0.0.1:9151\n"));
  EXPECT_FALSE(ParseControlPortFile("PORT=127.0.0.1:0\n"));
  EXPECT_FALSE(ParseControlPortFile("PORT=127.0.0.1:70000\n"));
  EXPECT_FALSE(ParseControlPortFile("PORT=127.0.0.1:9151x\n"));
  EXPECT_FALSE(ParseControlPortFile(""));
}

TEST(TorControlProtocolTest, ParseSocksListenerReply) {
  EXPECT_EQ(38211, ParseSocksListenerReply(
                       "250-net/listeners/socks=\"127.0.0.1:38211\"\r\n"
                       "250 OK\r\n"));
  EXPECT_EQ(9150, ParseSocksListenerReply(
                      "250-net/listeners/socks=\"unix:/tmp/s\" "
                      "\"127.0.0.1:9150\"\r\n250 OK\r\n"));
  EXPECT_FALSE(ParseSocksListenerReply(
      "250-net/listeners/socks=\"0.0.0.0:9050\"\r\n250 OK\r\n"));
  EXPECT_FALSE(ParseSocksListenerReply(
      "250-net/listeners/socks=\r\n250 OK\r\n"));
  EXPECT_FALSE(ParseSocksListenerReply("552 Unrecognized key\r\n"));
}

TEST(TorControlProtocolTest, ParseGetInfoValue) {
  const char kReply[] =
      "250-status/circuit-established=1\r\n250 OK\r\n";
  EXPECT_EQ("1", ParseGetInfoValue(kReply, "status/circuit-established"));
  EXPECT_FALSE(ParseGetInfoValue(kReply, "status/circuit"));
  EXPECT_FALSE(ParseGetInfoValue("551 Internal error\r\n",
                                 "status/circuit-established"));
}

}  // namespace
}  // namespace control
}  // namespace tor
}  // namespace molt_ai
