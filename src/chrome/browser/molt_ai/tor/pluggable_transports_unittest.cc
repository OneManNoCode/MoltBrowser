// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/pluggable_transports.h"

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/tor/torrc_builder.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace tor {
namespace {

constexpr char kObfs4Line[] =
    "obfs4 192.95.36.142:443 CDF2E852BF539B82BD10E27E9115A31734E378C2 "
    "cert=qUVQ0srL1JI/vO6V6m/24anYXiJD3QP2HgzUKQtQ7GRqqUvs7P+tG43RtAqdhLOALP7DJQ "
    "iat-mode=1";
constexpr char kSnowflakeLine[] =
    "snowflake 192.0.2.3:80 2B280B23E1107BB62ABFC40DDCC8824814F80A72 "
    "url=https://snowflake-broker.torproject.net/";

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

TEST(PluggableTransportsTest, Names) {
  EXPECT_EQ("obfs4", TransportName(BridgeTransport::kObfs4));
  EXPECT_EQ("snowflake", TransportName(BridgeTransport::kSnowflake));
  EXPECT_EQ("lyrebird", BundledTransportFileName(BridgeTransport::kObfs4));
  EXPECT_EQ("snowflake-client",
            BundledTransportFileName(BridgeTransport::kSnowflake));
}

// Only fixed, root-owned-by-convention locations: never /usr/local, /opt,
// the home directory or a bare name (PATH).
TEST(PluggableTransportsTest, LinuxSystemPathsAreFixedUsrBinPaths) {
  EXPECT_EQ((std::vector<std::string>{"/usr/bin/lyrebird",
                                      "/usr/bin/obfs4proxy"}),
            LinuxSystemTransportPaths(BridgeTransport::kObfs4));
  EXPECT_EQ((std::vector<std::string>{"/usr/bin/snowflake-client"}),
            LinuxSystemTransportPaths(BridgeTransport::kSnowflake));
  for (BridgeTransport t :
       {BridgeTransport::kObfs4, BridgeTransport::kSnowflake}) {
    for (const std::string& path : LinuxSystemTransportPaths(t)) {
      EXPECT_EQ(0u, path.find("/usr/bin/")) << path;
      // Valid as an absolute ClientTransportPlugin exec path.
      EXPECT_TRUE(IsValidTransportExec(path)) << path;
    }
  }
}

TEST(PluggableTransportsTest, RequiredTransports) {
  EXPECT_TRUE(RequiredTransports({}).empty());
  EXPECT_EQ((std::vector<BridgeTransport>{BridgeTransport::kObfs4}),
            RequiredTransports({kObfs4Line, kObfs4Line}));
  EXPECT_EQ((std::vector<BridgeTransport>{BridgeTransport::kSnowflake}),
            RequiredTransports({kSnowflakeLine}));
  // Each once, obfs4 first, whatever the order of the lines.
  EXPECT_EQ((std::vector<BridgeTransport>{BridgeTransport::kObfs4,
                                          BridgeTransport::kSnowflake}),
            RequiredTransports({kSnowflakeLine, kObfs4Line, kSnowflakeLine}));
  // Unknown transports are not "required" (BuildTorrc refuses them).
  EXPECT_TRUE(RequiredTransports({"meek_lite 1.2.3.4:80 x"}).empty());
}

TEST(PluggableTransportsTest, LinuxMessagesNamePackages) {
  const std::string obfs4 = TransportUnavailableMessage(
      BridgeTransport::kObfs4, TransportSource::kSystemPackages,
      /*rejected_existing=*/false, "/usr/bin/obfs4proxy not found");
  EXPECT_TRUE(Contains(obfs4, "obfs4proxy")) << obfs4;
  EXPECT_TRUE(Contains(obfs4, "lyrebird")) << obfs4;
  EXPECT_TRUE(Contains(obfs4, "sudo apt install obfs4proxy")) << obfs4;
  // "not found" is noise for a missing package.
  EXPECT_FALSE(Contains(obfs4, "not found")) << obfs4;

  const std::string snowflake = TransportUnavailableMessage(
      BridgeTransport::kSnowflake, TransportSource::kSystemPackages, false,
      "");
  EXPECT_TRUE(Contains(snowflake, "sudo apt install snowflake-client"))
      << snowflake;

  // A refused binary names the reason.
  const std::string refused = TransportUnavailableMessage(
      BridgeTransport::kObfs4, TransportSource::kSystemPackages,
      /*rejected_existing=*/true, "/usr/bin/lyrebird is owned by uid 1000");
  EXPECT_TRUE(Contains(refused, "(/usr/bin/lyrebird is owned by uid 1000)"))
      << refused;
  EXPECT_TRUE(Contains(refused, "Reinstall")) << refused;

  const std::string none = NoTransportsMessage(TransportSource::kSystemPackages);
  EXPECT_EQ(0u, none.find("Install obfs4proxy/snowflake-client to use "
                          "bridges"))
      << none;

  EXPECT_TRUE(Contains(
      BuiltInBridgesUnavailableMessage(TransportSource::kSystemPackages),
      "Custom bridges"));
}

TEST(PluggableTransportsTest, BundledMessagesSayReinstall) {
  for (BridgeTransport t :
       {BridgeTransport::kObfs4, BridgeTransport::kSnowflake}) {
    const std::string message = TransportUnavailableMessage(
        t, TransportSource::kBundled, false, "lyrebird.exe not found");
    EXPECT_TRUE(Contains(message, "Reinstall MoltBrowser")) << message;
    EXPECT_TRUE(Contains(message, std::string(BundledTransportFileName(t))))
        << message;
    // Bundled builds have nothing to "apt install".
    EXPECT_FALSE(Contains(message, "apt")) << message;
  }
  EXPECT_FALSE(Contains(NoTransportsMessage(TransportSource::kBundled), "apt"));
  for (TransportSource source :
       {TransportSource::kBundled, TransportSource::kSystemPackages}) {
    const std::string message = BuiltInBridgesUnavailableMessage(source);
    EXPECT_TRUE(Contains(message, "bridges.torproject.org")) << message;
    EXPECT_EQ('.', message.back());
  }
}

}  // namespace
}  // namespace tor
}  // namespace molt_ai
