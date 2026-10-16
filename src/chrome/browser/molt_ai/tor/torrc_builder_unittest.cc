// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/torrc_builder.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

// A public Tor Browser default obfs4 bridge (format reference only).
constexpr char kObfs4Line[] =
    "obfs4 192.95.36.142:443 CDF2E852BF539B82BD10E27E9115A31734E378C2 "
    "cert=qUVQ0srL1JI/vO6V6m/24anYXiJD3QP2HgzUKQtQ7GRqqUvs7P+tG43RtAqdhLOALP7DJQ "
    "iat-mode=1";

// Shape of Tor Browser's built-in snowflake lines (pt_config.json).
constexpr char kSnowflakeLine[] =
    "snowflake 192.0.2.3:80 2B280B23E1107BB62ABFC40DDCC8824814F80A72 "
    "fingerprint=2B280B23E1107BB62ABFC40DDCC8824814F80A72 "
    "url=https://snowflake-broker.torproject.net/ "
    "ampcache=https://cdn.ampproject.org/ front=www.google.com "
    "ice=stun:stun.l.google.com:19302,stun:stun.antisip.com:3478 "
    "utls-imitate=hellorandomizedalpn";

TorrcOptions BaseOptions() {
  TorrcOptions o;
  o.data_dir = "/Users/me/Library/Application Support/MoltBrowser/tor";
  o.cookie_file = o.data_dir + "/control_auth_cookie";
  o.control_port_file = o.data_dir + "/control_port";
  o.log_file = o.data_dir + "/tor.log";
  return o;
}

bool HasLine(const std::string& torrc, const std::string& line) {
  return torrc.find("\n" + line + "\n") != std::string::npos;
}

TEST(TorrcBuilderTest, BaseConfigUsesAutoPortsAndCookieAuth) {
  std::string error;
  std::optional<std::string> torrc = BuildTorrc(BaseOptions(), &error);
  ASSERT_TRUE(torrc) << error;
  EXPECT_TRUE(
      HasLine(*torrc, "SocksPort auto IsolateSOCKSAuth KeepAliveIsolateSOCKSAuth"));
  EXPECT_TRUE(HasLine(*torrc, "ControlPort auto"));
  EXPECT_TRUE(HasLine(*torrc, "CookieAuthentication 1"));
  EXPECT_TRUE(HasLine(*torrc,
                      "ControlPortWriteToFile /Users/me/Library/Application "
                      "Support/MoltBrowser/tor/control_port"));
  // Never the old fixed ports, never bridges/exit pinning by default.
  EXPECT_EQ(std::string::npos, torrc->find("9050"));
  EXPECT_EQ(std::string::npos, torrc->find("9051"));
  EXPECT_EQ(std::string::npos, torrc->find("UseBridges"));
  EXPECT_EQ(std::string::npos, torrc->find("ExitNodes"));
  EXPECT_EQ(std::string::npos, torrc->find("StrictNodes"));
}

TEST(TorrcBuilderTest, ExitCountryAddsStrictNodesOnlyWhenSet) {
  TorrcOptions o = BaseOptions();
  o.exit_country = "de";
  std::optional<std::string> torrc = BuildTorrc(o, nullptr);
  ASSERT_TRUE(torrc);
  EXPECT_TRUE(HasLine(*torrc, "ExitNodes {de}"));
  EXPECT_TRUE(HasLine(*torrc, "StrictNodes 1"));

  for (const char* bad : {"DE", "d", "deu", "d}\nSocksPort 0.0.0.0:9", "1a"}) {
    o.exit_country = bad;
    EXPECT_FALSE(BuildTorrc(o, nullptr)) << bad;
  }
}

TEST(TorrcBuilderTest, RejectsInjectionThroughPaths) {
  const char* kBad[] = {
      "",
      "/tmp/x\nControlPort 0.0.0.0:9051",
      "/tmp/x\rSocksPort 0.0.0.0:9050",
      "\"/tmp/quoted\"",
      "/tmp/x # comment",
      "/tmp/x\\",
      " /tmp/leading-space",
      "/tmp/trailing-space ",
  };
  for (const char* bad : kBad) {
    TorrcOptions o = BaseOptions();
    o.data_dir = bad;
    std::string error;
    EXPECT_FALSE(BuildTorrc(o, &error)) << bad;
    EXPECT_FALSE(error.empty());
  }
  TorrcOptions o = BaseOptions();
  o.geoip_file = "/x\nUseBridges 0";
  EXPECT_FALSE(BuildTorrc(o, nullptr));
  std::string nul_path = "/tmp/x";
  nul_path.push_back('\0');
  nul_path += "y";
  o = BaseOptions();
  o.log_file = nul_path;
  EXPECT_FALSE(BuildTorrc(o, nullptr));
}

TEST(TorrcBuilderTest, NormalizesObfs4Bridge) {
  std::string error;
  std::optional<std::string> n = NormalizeBridgeLine(
      std::string("  Bridge   obfs4  192.95.36.142:443\t"
                  "cdf2e852bf539b82bd10e27e9115a31734e378c2 "
                  "cert=qUVQ0srL1JI/vO6V6m/24anYXiJD3QP2HgzUKQtQ7GRqqUvs7P+"
                  "tG43RtAqdhLOALP7DJQ iat-mode=1  "),
      &error);
  ASSERT_TRUE(n) << error;
  EXPECT_EQ(kObfs4Line, *n);
  EXPECT_EQ(BridgeTransport::kObfs4, GetBridgeTransport(*n));
}

TEST(TorrcBuilderTest, AcceptsSnowflakeAndIPv6Bridges) {
  std::string error;
  std::optional<std::string> n = NormalizeBridgeLine(kSnowflakeLine, &error);
  ASSERT_TRUE(n) << error;
  EXPECT_EQ(kSnowflakeLine, *n);
  EXPECT_EQ(BridgeTransport::kSnowflake, GetBridgeTransport(*n));
  EXPECT_TRUE(NormalizeBridgeLine(
      "obfs4 [2001:db8::1]:443 CDF2E852BF539B82BD10E27E9115A31734E378C2 "
      "cert=qUVQ0srL1JI/vO6V6m/24anYXiJD3QP2HgzUKQtQ7GRqqUvs7P+tG43RtAqdhLOALP"
      "7DJQ iat-mode=0",
      &error))
      << error;
}

TEST(TorrcBuilderTest, RejectsMalformedOrHostileBridgeLines) {
  const std::string fp = "CDF2E852BF539B82BD10E27E9115A31734E378C2";
  const std::string cert =
      "cert=qUVQ0srL1JI/vO6V6m/24anYXiJD3QP2HgzUKQtQ7GRqqUvs7P+tG43RtAqdhLOALP"
      "7DJQ";
  const std::vector<std::string> kBad = {
      "",
      "obfs4",
      // Injection attempts.
      "obfs4 1.2.3.4:443 " + fp + " " + cert + " iat-mode=1\nSocksPort 9999",
      "obfs4 1.2.3.4:443 " + fp + " " + cert + " iat-mode=1\rControlPort 1",
      "obfs4 1.2.3.4:443 " + fp + " " + cert + " iat-mode=1 #comment",
      "snowflake 1.2.3.4:80 " + fp + " url=\"https://x/\"",
      "snowflake 1.2.3.4:80 " + fp + " url=https://x/\\",
      "snowflake 1.2.3.4:80 " + fp + " url=https://x/{y}",
      "snowflake 1.2.3.4:80 " + fp + " url=https://x/;rm",
      // Unsupported transports / vanilla bridges.
      "meek_lite 1.2.3.4:80 " + fp + " url=https://x/",
      "webtunnel 1.2.3.4:443 " + fp + " url=https://x/",
      "1.2.3.4:443 " + fp,
      // Address / port / fingerprint problems.
      "obfs4 1.2.3:443 " + fp + " " + cert + " iat-mode=1",
      "obfs4 1.2.3.256:443 " + fp + " " + cert + " iat-mode=1",
      "obfs4 1.2.3.4:0 " + fp + " " + cert + " iat-mode=1",
      "obfs4 1.2.3.4:65536 " + fp + " " + cert + " iat-mode=1",
      "obfs4 1.2.3.4 " + fp + " " + cert + " iat-mode=1",
      "obfs4 example.com:443 " + fp + " " + cert + " iat-mode=1",
      "obfs4 [::1:443 " + fp + " " + cert + " iat-mode=1",
      "obfs4 1.2.3.4:443 " + fp.substr(0, 39) + " " + cert + " iat-mode=1",
      "obfs4 1.2.3.4:443 " + fp.substr(0, 39) + "G " + cert + " iat-mode=1",
      // obfs4 parameter rules.
      "obfs4 1.2.3.4:443 " + fp + " " + cert,
      "obfs4 1.2.3.4:443 " + fp + " iat-mode=1",
      "obfs4 1.2.3.4:443 " + fp + " " + cert + " iat-mode=3",
      "obfs4 1.2.3.4:443 " + fp + " cert=short iat-mode=1",
      "obfs4 1.2.3.4:443 " + fp + " " + cert + " iat-mode=1 iat-mode=1",
      "obfs4 1.2.3.4:443 " + fp + " " + cert + " iat-mode=1 extra=1",
      "obfs4 1.2.3.4:443 " + fp + " " + cert + " iat-mode=1 novalue",
      // Non-ASCII.
      "snowflake 1.2.3.4:80 " + fp + " front=\xC3\xA9xample.com",
  };
  for (const std::string& bad : kBad) {
    std::string error;
    EXPECT_FALSE(NormalizeBridgeLine(bad, &error)) << bad;
    EXPECT_FALSE(error.empty()) << bad;
  }
}

TEST(TorrcBuilderTest, ParseBridgeLinesIsAllOrNothing) {
  std::string error;
  std::optional<std::vector<std::string>> lines = ParseBridgeLines(
      std::string("# my bridges\r\n\r\n") + kObfs4Line + "\r\n" + kObfs4Line +
          "\n" + kSnowflakeLine,
      &error);
  ASSERT_TRUE(lines) << error;
  ASSERT_EQ(2u, lines->size());  // Duplicate dropped.
  EXPECT_EQ(kObfs4Line, (*lines)[0]);
  EXPECT_EQ(kSnowflakeLine, (*lines)[1]);

  lines = ParseBridgeLines(
      std::string(kObfs4Line) + "\nobfs4 1.2.3.4:1 nope", &error);
  EXPECT_FALSE(lines);
  EXPECT_EQ(0u, error.find("Line 2:")) << error;

  std::string many;
  for (int i = 1; i <= 40; ++i) {
    many += "obfs4 10.0.0." + std::to_string(i) +
            ":443 CDF2E852BF539B82BD10E27E9115A31734E378C2 "
            "cert=qUVQ0srL1JI/vO6V6m/24anYXiJD3QP2HgzUKQtQ7GRqqUvs7P+tG43RtAq"
            "dhLOALP7DJQ iat-mode=0\n";
  }
  EXPECT_FALSE(ParseBridgeLines(many, &error));

  lines = ParseBridgeLines("", &error);
  ASSERT_TRUE(lines);
  EXPECT_TRUE(lines->empty());
}

TEST(TorrcBuilderTest, BridgesEmitTransportPluginsOnlyWhenNeeded) {
  TorrcOptions o = BaseOptions();
  o.bridge_lines = {kObfs4Line};
  std::string error;
  // No obfs4 transport -> refuse rather than write a torrc Tor can't use.
  EXPECT_FALSE(BuildTorrc(o, &error));

  o.obfs4_exec = "./pluggable_transports/lyrebird";
  o.snowflake_exec = "./pluggable_transports/snowflake-client";
  std::optional<std::string> torrc = BuildTorrc(o, &error);
  ASSERT_TRUE(torrc) << error;
  EXPECT_TRUE(HasLine(*torrc, "UseBridges 1"));
  EXPECT_TRUE(HasLine(
      *torrc, "ClientTransportPlugin obfs4 exec ./pluggable_transports/lyrebird"));
  EXPECT_EQ(std::string::npos, torrc->find("ClientTransportPlugin snowflake"));
  EXPECT_TRUE(HasLine(*torrc, std::string("Bridge ") + kObfs4Line));

  o.bridge_lines = {kSnowflakeLine};
  torrc = BuildTorrc(o, &error);
  ASSERT_TRUE(torrc) << error;
  EXPECT_TRUE(HasLine(*torrc,
                      "ClientTransportPlugin snowflake exec "
                      "./pluggable_transports/snowflake-client"));
  EXPECT_EQ(std::string::npos, torrc->find("ClientTransportPlugin obfs4"));

  // Exec paths with spaces would be split by Tor: refuse.
  o.snowflake_exec = "/Applications/My Apps/snowflake-client";
  EXPECT_FALSE(BuildTorrc(o, nullptr));

  // Non-canonical (unvalidated) lines are rejected even if harmless-looking.
  o = BaseOptions();
  o.obfs4_exec = "./pluggable_transports/lyrebird";
  o.bridge_lines = {std::string("Bridge ") + kObfs4Line};
  EXPECT_FALSE(BuildTorrc(o, nullptr));
}

// Linux packages don't bundle the transports: the distribution's root-owned
// lyrebird / obfs4proxy / snowflake-client are run by absolute path.
TEST(TorrcBuilderTest, SystemTransportsUseAbsolutePaths) {
  TorrcOptions o = BaseOptions();
  o.bridge_lines = {kObfs4Line, kSnowflakeLine};
  o.obfs4_exec = "/usr/bin/obfs4proxy";
  o.snowflake_exec = "/usr/bin/snowflake-client";
  std::string error;
  std::optional<std::string> torrc = BuildTorrc(o, &error);
  ASSERT_TRUE(torrc) << error;
  EXPECT_TRUE(HasLine(*torrc, "UseBridges 1"));
  EXPECT_TRUE(
      HasLine(*torrc, "ClientTransportPlugin obfs4 exec /usr/bin/obfs4proxy"));
  EXPECT_TRUE(HasLine(
      *torrc, "ClientTransportPlugin snowflake exec /usr/bin/snowflake-client"));
  EXPECT_TRUE(HasLine(*torrc, std::string("Bridge ") + kObfs4Line));
  EXPECT_TRUE(HasLine(*torrc, std::string("Bridge ") + kSnowflakeLine));

  // Debian's lyrebird package.
  o.bridge_lines = {kObfs4Line};
  o.obfs4_exec = "/usr/bin/lyrebird";
  o.snowflake_exec.clear();
  torrc = BuildTorrc(o, &error);
  ASSERT_TRUE(torrc) << error;
  EXPECT_TRUE(
      HasLine(*torrc, "ClientTransportPlugin obfs4 exec /usr/bin/lyrebird"));
  EXPECT_EQ(std::string::npos, torrc->find("ClientTransportPlugin snowflake"));

  // A snowflake bridge with only obfs4 available is refused.
  o.bridge_lines = {kSnowflakeLine};
  EXPECT_FALSE(BuildTorrc(o, &error));
  EXPECT_FALSE(error.empty());
}

TEST(TorrcBuilderTest, TransportExecValidation) {
  for (const char* good : {
           "./pluggable_transports/lyrebird",
           "./pluggable_transports/snowflake-client",
           "./lyrebird",
           "pluggable_transports\\lyrebird.exe",
           "pluggable_transports\\snowflake-client.exe",
           "/usr/bin/lyrebird",
           "/usr/bin/obfs4proxy",
           "/usr/bin/snowflake-client",
       }) {
    EXPECT_TRUE(IsValidTransportExec(good)) << good;
  }
  std::string nul = "/usr/bin/lyrebird";
  nul.push_back('\0');
  for (const std::string& bad : std::vector<std::string>{
           "",
           nul,
           // Bare names could be looked up on PATH.
           "lyrebird",
           "obfs4proxy",
           ".",
           "./",
           // Not canonical, or escaping the tor dir.
           "../lyrebird",
           "./pluggable_transports/../../lyrebird",
           "pluggable_transports\\..\\lyrebird.exe",
           "./pluggable_transports//lyrebird",
           "./pluggable_transports/./lyrebird",
           "/usr/bin/../bin/lyrebird",
           "/usr/bin//lyrebird",
           "/usr/bin/./lyrebird",
           "/usr/bin/lyrebird/",
           "/",
           "/usr\\bin\\lyrebird",
           // Windows absolute / root-relative paths.
           "C:\\tor\\pluggable_transports\\lyrebird.exe",
           "C:/tor/lyrebird.exe",
           "\\tor\\lyrebird.exe",
           "\\\\server\\share\\lyrebird.exe",
           // Tor splits the line on blanks; torrc injection.
           "/opt/My Apps/lyrebird",
           "/usr/bin/lyrebird\tmanaged",
           "/usr/bin/lyrebird\nSocksPort 0.0.0.0:9050",
           "/usr/bin/lyrebird#comment",
           "\"/usr/bin/lyrebird\"",
           "./pluggable_transports/lyrebird\\",
       }) {
    EXPECT_FALSE(IsValidTransportExec(bad)) << bad;
    TorrcOptions o = BaseOptions();
    o.bridge_lines = {kObfs4Line};
    o.obfs4_exec = bad;
    EXPECT_FALSE(BuildTorrc(o, nullptr)) << bad;
  }
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
