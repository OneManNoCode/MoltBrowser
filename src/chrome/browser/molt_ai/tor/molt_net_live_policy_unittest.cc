// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_live_policy.h"

#include <cstdint>
#include <optional>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

// What a MoltNet profile's settings look like right after MoltNet wrote
// them, with no policy.
MoltNetProfileSettings MoltNetsOwnSettings() {
  MoltNetProfileSettings settings;
  settings.proxy = {.proxy_is_default = false,
                    .proxy_is_user_controlled = true,
                    .override_rule_count = 0};
  settings.proxy_is_moltnets_own = true;
  settings.leak_controls_in_place = true;
  settings.blob_urls_partitioned = true;
  return settings;
}

TEST(MoltNetLivePolicyTest, WritesWhatItAccepts) {
  for (uint16_t port : {1, 9050, 9150, 45213, 65535}) {
    EXPECT_EQ(MoltNetSocksPort(true, MoltNetSocksProxyUri(port),
                               kMoltNetProxyBypassList),
              std::optional<uint16_t>(port))
        << port;
  }
  EXPECT_EQ(MoltNetSocksProxyUri(9050), "socks5://127.0.0.1:9050");
}

TEST(MoltNetLivePolicyTest, AcceptsOnlyMoltNetsOwnProxySetting) {
  const std::string own = MoltNetSocksProxyUri(9150);
  // Not fixed servers (direct, system, PAC, auto-detect).
  EXPECT_FALSE(MoltNetSocksPort(false, own, kMoltNetProxyBypassList));
  // Another bypass list: none (Chromium's implicit loopback bypass stays),
  // or one that sends sites direct.
  for (const char* bypass :
       {"", "<local>", "*", "<-loopback>;*", "<-loopback>;example.com",
        " <-loopback>", "<-LOOPBACK>"}) {
    EXPECT_FALSE(MoltNetSocksPort(true, own, bypass)) << bypass;
  }
  // Another scheme, host, chain or list.
  for (const char* server :
       {"socks4://127.0.0.1:9150", "socks://127.0.0.1:9150",
        "http://127.0.0.1:9150", "https://127.0.0.1:9150",
        "socks5://localhost:9150", "socks5://127.0.0.2:9150",
        "socks5://[::1]:9150", "socks5://0.0.0.0:9150",
        "socks5://127.0.0.1:9150;direct://",
        "socks5://127.0.0.1:9150,direct://",
        "socks5://127.0.0.1:9150 socks5://127.0.0.1:9151",
        "https=socks5://127.0.0.1:9150", "socks5://127.0.0.1",
        "SOCKS5://127.0.0.1:9150", " socks5://127.0.0.1:9150", "direct://",
        ""}) {
    EXPECT_FALSE(MoltNetSocksPort(true, server, kMoltNetProxyBypassList))
        << server;
  }
}

TEST(MoltNetLivePolicyTest, RefusesMalformedPorts) {
  for (const char* port :
       {"", "0", "09150", "65536", "99999", "100000", "-1", "+9150", "9150 ",
        "9150/", "9150a", "0x23", "1e3"}) {
    EXPECT_FALSE(MoltNetSocksPort(
        true, std::string("socks5://127.0.0.1:") + port,
        kMoltNetProxyBypassList))
        << port;
  }
}

TEST(MoltNetLivePolicyTest, MoltNetsOwnSettingsPass) {
  EXPECT_EQ(CheckMoltNetSettings(MoltNetsOwnSettings()),
            MoltNetSettingsCheck::kOk);
  EXPECT_EQ(MoltNetSettingsError(MoltNetSettingsCheck::kOk), "");
}

TEST(MoltNetLivePolicyTest, UnfilledSettingsRefuse) {
  EXPECT_EQ(CheckMoltNetSettings(MoltNetProfileSettings()),
            MoltNetSettingsCheck::kProxyNotMoltNets);
}

TEST(MoltNetLivePolicyTest, PolicyProxyRefuses) {
  // A policy (or command-line flag) outranks MoltNet's own value; the
  // effective value is then whatever it says.
  MoltNetProfileSettings settings = MoltNetsOwnSettings();
  settings.proxy.proxy_is_user_controlled = false;
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kProxyNotMoltNets);
  // Even one that happens to name the same SOCKS5 proxy.
  settings.proxy_is_moltnets_own = true;
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kProxyNotMoltNets);
}

TEST(MoltNetLivePolicyTest, OverrideRulesRefuse) {
  for (size_t rules : {1u, 3u}) {
    MoltNetProfileSettings settings = MoltNetsOwnSettings();
    settings.proxy.override_rule_count = rules;
    EXPECT_EQ(CheckMoltNetSettings(settings),
              MoltNetSettingsCheck::kProxyNotMoltNets)
        << rules;
  }
}

TEST(MoltNetLivePolicyTest, ProxyThatIsNotMoltNetsRefuses) {
  MoltNetProfileSettings settings = MoltNetsOwnSettings();
  settings.proxy_is_moltnets_own = false;
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kProxyNotMoltNets);
  // The default value (use the system's settings) isn't MoltNet's either.
  settings.proxy = ProfileProxyPrefs();
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kProxyNotMoltNets);
}

TEST(MoltNetLivePolicyTest, LeakControlPolicyRefuses) {
  MoltNetProfileSettings settings = MoltNetsOwnSettings();
  settings.leak_controls_in_place = false;
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kLeakControlsOverridden);
}

TEST(MoltNetLivePolicyTest, UnpartitionedBlobUrlsRefuse) {
  MoltNetProfileSettings settings = MoltNetsOwnSettings();
  settings.blob_urls_partitioned = false;
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kBlobUrlsUnpartitioned);
}

TEST(MoltNetLivePolicyTest, TheProxyIsCheckedFirst) {
  // With several reasons, the message names the proxy, as before the blob
  // URL check was added.
  MoltNetProfileSettings settings = MoltNetsOwnSettings();
  settings.proxy.override_rule_count = 1;
  settings.leak_controls_in_place = false;
  settings.blob_urls_partitioned = false;
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kProxyNotMoltNets);
  settings.proxy.override_rule_count = 0;
  EXPECT_EQ(CheckMoltNetSettings(settings),
            MoltNetSettingsCheck::kLeakControlsOverridden);
}

TEST(MoltNetLivePolicyTest, MessagesNameTheCause) {
  // The open check's messages, unchanged: the panel and the leak kit read
  // them.
  EXPECT_EQ(MoltNetSettingsError(MoltNetSettingsCheck::kProxyNotMoltNets),
            "A policy or command-line flag controls the proxy, so MoltNet "
            "can't guarantee Tor routing");
  EXPECT_EQ(
      MoltNetSettingsError(MoltNetSettingsCheck::kLeakControlsOverridden),
      "A policy overrides MoltNet's WebRTC / prediction / HTTPS-Only / "
      "Safe Browsing settings");
  const std::string blob =
      MoltNetSettingsError(MoltNetSettingsCheck::kBlobUrlsUnpartitioned);
  EXPECT_NE(blob.find("PartitionedBlobUrlUsage"), std::string::npos);
  EXPECT_NE(blob.find("policy"), std::string::npos);
}

TEST(MoltNetLivePolicyTest, EveryRefusalHasAMessage) {
  for (MoltNetSettingsCheck check :
       {MoltNetSettingsCheck::kProxyNotMoltNets,
        MoltNetSettingsCheck::kLeakControlsOverridden,
        MoltNetSettingsCheck::kBlobUrlsUnpartitioned}) {
    const std::string error = MoltNetSettingsError(check);
    EXPECT_FALSE(error.empty());
    // Shown in a dialog: one sentence, plain text.
    EXPECT_LT(error.size(), 120u) << error;
    EXPECT_EQ(error.find('<'), std::string::npos) << error;
  }
  EXPECT_FALSE(std::string(kMoltNetEndedTitle).empty());
}

TEST(MoltNetLivePolicyTest, OtherProfilesGetEveryProxyConfig) {
  for (bool available : {false, true}) {
    for (bool own : {false, true}) {
      EXPECT_TRUE(MoltNetAcceptsProxyConfig(false, available, own));
    }
  }
}

TEST(MoltNetLivePolicyTest, MoltNetGetsOnlyItsOwnProxyConfig) {
  EXPECT_TRUE(MoltNetAcceptsProxyConfig(true, true, true));
  // A policy's proxy, override rules, the system's settings, DIRECT.
  EXPECT_FALSE(MoltNetAcceptsProxyConfig(true, true, false));
  // Unset: the monitor would send DIRECT.
  EXPECT_FALSE(MoltNetAcceptsProxyConfig(true, false, true));
  EXPECT_FALSE(MoltNetAcceptsProxyConfig(true, false, false));
}

TEST(MoltNetLivePolicyTest, IsAConstantExpression) {
  static_assert(MoltNetSocksPort(true, "socks5://127.0.0.1:9050",
                                 "<-loopback>") == 9050);
  static_assert(!MoltNetSocksPort(true, "socks5://127.0.0.1:9050", ""));
  static_assert(CheckMoltNetSettings(MoltNetProfileSettings()) ==
                MoltNetSettingsCheck::kProxyNotMoltNets);
  static_assert(!MoltNetAcceptsProxyConfig(true, true, false));
}

}  // namespace
}  // namespace molt_ai::tor
