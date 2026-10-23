// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_network.h"

#include <cmath>
#include <memory>
#include <optional>
#include <set>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_connection_type.h"
#include "third_party/blink/public/platform/web_effective_connection_type.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_network_values.h"
#include "third_party/blink/renderer/platform/network/network_state_notifier.h"
#include "third_party/blink/renderer/platform/scheduler/test/fake_task_runner.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink::molt_fingerprinting {
namespace {

using scheduler::FakeTaskRunner;

class Observer final : public NetworkStateNotifier::NetworkStateObserver {
 public:
  void ConnectionChange(WebConnectionType type,
                        double max_bandwidth_mbps,
                        WebEffectiveConnectionType effective_type,
                        const std::optional<base::TimeDelta>& http_rtt,
                        const std::optional<base::TimeDelta>& transport_rtt,
                        const std::optional<double>& downlink_mbps,
                        bool save_data) override {
    ++connection_changes;
    last_effective_type = effective_type;
    last_http_rtt = http_rtt;
  }
  void OnLineStateChange(bool on_line) override {
    ++on_line_changes;
    last_on_line = on_line;
  }

  int connection_changes = 0;
  int on_line_changes = 0;
  bool last_on_line = true;
  WebEffectiveConnectionType last_effective_type =
      WebEffectiveConnectionType::kTypeUnknown;
  std::optional<base::TimeDelta> last_http_rtt;
};

// Each test uses a notifier of its own, as the browser fills it when a
// renderer starts: a 3g estimate over Wi-Fi with data saver on. Pinning the
// process's notifier here (Configure(kUniform)) would pin it for every
// later test.
class MoltNetNetworkTest : public testing::Test {
 protected:
  MoltNetNetworkTest()
      : task_runner_(base::MakeRefCounted<FakeTaskRunner>()),
        notifier_(std::make_unique<NetworkStateNotifier>()) {
    SetRealState(*notifier_);
  }

  void TearDown() override {
    notifier_->ClearOverride();
    RunPendingTasks();
  }

  static void SetRealState(NetworkStateNotifier& notifier) {
    notifier.SetOnLine(true);
    notifier.SetWebConnection(kWebConnectionTypeWifi, 54.0);
    notifier.SetNetworkQuality(WebEffectiveConnectionType::kType3G,
                               base::Milliseconds(450),
                               base::Milliseconds(400),
                               /*downlink_throughput_kbps=*/1300);
    notifier.SetSaveDataEnabled(true);
  }

  void Pin() { notifier_->SetUniformNetworkState(UniformNetworkState()); }

  void RunPendingTasks() { task_runner_->RunUntilIdle(); }

  void ExpectUniform(const NetworkStateNotifier& notifier) {
    EXPECT_EQ(notifier.ConnectionType(), kWebConnectionTypeUnknown);
    EXPECT_FALSE(notifier.IsCellularConnectionType());
    EXPECT_TRUE(std::isinf(notifier.MaxBandwidth()));
    EXPECT_EQ(notifier.EffectiveType(), WebEffectiveConnectionType::kType4G);
    EXPECT_EQ(notifier.HttpRtt(), base::Milliseconds(200));
    EXPECT_EQ(notifier.TransportRtt(), base::Milliseconds(125));
    EXPECT_EQ(notifier.DownlinkThroughputMbps(), 1.6);
    EXPECT_FALSE(notifier.SaveDataEnabled());
    EXPECT_EQ(notifier.GetWebHoldbackEffectiveType(), std::nullopt);
    EXPECT_EQ(notifier.GetWebHoldbackHttpRtt(), std::nullopt);
    EXPECT_EQ(notifier.GetWebHoldbackDownlinkThroughputMbps(), std::nullopt);
  }

  scoped_refptr<FakeTaskRunner> task_runner_;
  std::unique_ptr<NetworkStateNotifier> notifier_;
};

TEST_F(MoltNetNetworkTest, UniformStateHasTheFixedValues) {
  const NetworkStateNotifier::NetworkState state = UniformNetworkState();
  EXPECT_TRUE(state.connection_initialized);
  EXPECT_EQ(state.type, kWebConnectionTypeUnknown);
  EXPECT_TRUE(std::isinf(state.max_bandwidth_mbps));
  EXPECT_EQ(state.effective_type, WebEffectiveConnectionType::kType4G);
  EXPECT_EQ(state.http_rtt, base::Milliseconds(moltnet::kUniformHttpRttMs));
  EXPECT_EQ(state.transport_rtt,
            base::Milliseconds(moltnet::kUniformTransportRttMs));
  EXPECT_EQ(state.downlink_throughput_mbps, 1.6);
  EXPECT_FALSE(state.save_data);
  EXPECT_EQ(state.network_quality_web_holdback, std::nullopt);
}

TEST_F(MoltNetNetworkTest, PinnedNotifierReportsTheFixedValues) {
  Pin();
  ExpectUniform(*notifier_);
  // The on-line state is the system's.
  EXPECT_TRUE(notifier_->OnLine());
}

// What navigator.connection and the client hints round: the same for every
// site (and every MoltNet user), where Chromium adds a per-site, per-process
// factor between 0.9 and 1.1.
TEST_F(MoltNetNetworkTest, EverySiteReadsTheSameRoundedValues) {
  NetworkStateNotifier regular;
  SetRealState(regular);
  std::set<double> regular_downlinks;
  Pin();
  for (int i = 0; i < 40; ++i) {
    const String host = String::Format("site%d.example", i);
    EXPECT_EQ(notifier_->RoundRtt(host, notifier_->HttpRtt()), 200u) << i;
    EXPECT_EQ(notifier_->RoundMbps(host, notifier_->DownlinkThroughputMbps()),
              1.6)
        << i;
    regular_downlinks.insert(
        regular.RoundMbps(host, regular.DownlinkThroughputMbps()));
  }
  EXPECT_EQ(notifier_->RoundRtt(String(), notifier_->HttpRtt()), 200u);
  EXPECT_EQ(notifier_->RoundMbps(String(), notifier_->DownlinkThroughputMbps()),
            1.6);

  // What a page constructing navigator.connection gets.
  WebConnectionType type;
  double downlink_max_mbps;
  WebEffectiveConnectionType effective_type;
  std::optional<base::TimeDelta> http_rtt;
  std::optional<double> downlink_mbps;
  bool save_data;
  notifier_->GetMetricsWithWebHoldback(&type, &downlink_max_mbps,
                                       &effective_type, &http_rtt,
                                       &downlink_mbps, &save_data);
  EXPECT_EQ(type, kWebConnectionTypeUnknown);
  EXPECT_TRUE(std::isinf(downlink_max_mbps));
  EXPECT_EQ(effective_type, WebEffectiveConnectionType::kType4G);
  EXPECT_EQ(http_rtt, base::Milliseconds(200));
  EXPECT_EQ(downlink_mbps, 1.6);
  EXPECT_FALSE(save_data);

  // Outside MoltNet the noise is still there: a 1.3 Mbps estimate reads
  // differently on different sites.
  EXPECT_GT(regular_downlinks.size(), 1u);
}

// The browser keeps sending its estimate, the system's connection type, data
// saver and the holdback experiment's type: none of it changes anything, and
// no change event fires.
TEST_F(MoltNetNetworkTest, UpdatesFromTheBrowserChangeNothing) {
  Pin();
  Observer observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_->AddConnectionObserver(&observer, task_runner_);

  notifier_->SetNetworkQuality(WebEffectiveConnectionType::kTypeSlow2G,
                               base::Milliseconds(2500),
                               base::Milliseconds(2400),
                               /*downlink_throughput_kbps=*/30);
  notifier_->SetWebConnection(kWebConnectionTypeCellular2G, 0.1);
  notifier_->SetSaveDataEnabled(true);
  notifier_->SetNetworkQualityWebHoldback(WebEffectiveConnectionType::kType2G);
  notifier_->SetNetworkQuality(WebEffectiveConnectionType::kType4G,
                               base::Milliseconds(40), base::Milliseconds(20),
                               /*downlink_throughput_kbps=*/90000);
  RunPendingTasks();

  EXPECT_EQ(observer.connection_changes, 0);
  ExpectUniform(*notifier_);
  EXPECT_EQ(notifier_->RoundRtt(String("a.example"), notifier_->HttpRtt()),
            200u);
}

// navigator.onLine and the online / offline events still follow the system.
TEST_F(MoltNetNetworkTest, OnLineStateStillFollowsTheSystem) {
  Pin();
  Observer observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_->AddOnLineObserver(&observer, task_runner_);
  notifier_->SetOnLine(false);
  RunPendingTasks();
  EXPECT_FALSE(notifier_->OnLine());
  EXPECT_EQ(observer.on_line_changes, 1);
  EXPECT_FALSE(observer.last_on_line);
  notifier_->SetOnLine(true);
  RunPendingTasks();
  EXPECT_TRUE(notifier_->OnLine());
  EXPECT_EQ(observer.on_line_changes, 2);
  ExpectUniform(*notifier_);
}

// DevTools' network emulation (and the tests' overrides) still apply while
// they are on, without noise; afterwards the fixed values are back.
TEST_F(MoltNetNetworkTest, DevToolsEmulationStillApplies) {
  Pin();
  notifier_->SetNetworkConnectionInfoOverride(
      /*on_line=*/true, kWebConnectionTypeCellular3G,
      WebEffectiveConnectionType::kType3G, /*http_rtt_msec=*/450,
      /*max_bandwidth_mbps=*/0.4);
  EXPECT_EQ(notifier_->EffectiveType(), WebEffectiveConnectionType::kType3G);
  EXPECT_EQ(notifier_->RoundRtt(String("a.example"), notifier_->HttpRtt()),
            450u);
  EXPECT_EQ(notifier_->RoundMbps(String("a.example"),
                                 notifier_->DownlinkThroughputMbps()),
            0.4);
  notifier_->ClearOverride();
  ExpectUniform(*notifier_);
}

// Pinning tells observers that already exist about the new values once
// (none exist at Configure() time, before any document).
TEST_F(MoltNetNetworkTest, PinningIsOneChange) {
  Observer observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_->AddConnectionObserver(&observer, task_runner_);
  Pin();
  RunPendingTasks();
  EXPECT_EQ(observer.connection_changes, 1);
  EXPECT_EQ(observer.last_effective_type, WebEffectiveConnectionType::kType4G);
  EXPECT_EQ(observer.last_http_rtt, base::Milliseconds(200));
}

// Regular and Incognito renderers (never pinned) keep Chromium's behaviour.
TEST_F(MoltNetNetworkTest, UnpinnedNotifierFollowsTheBrowser) {
  Observer observer;
  std::unique_ptr<NetworkStateNotifier::NetworkStateObserverHandle> handle =
      notifier_->AddConnectionObserver(&observer, task_runner_);
  EXPECT_EQ(notifier_->EffectiveType(), WebEffectiveConnectionType::kType3G);
  EXPECT_EQ(notifier_->ConnectionType(), kWebConnectionTypeWifi);
  EXPECT_TRUE(notifier_->SaveDataEnabled());
  notifier_->SetNetworkQuality(WebEffectiveConnectionType::kType2G,
                               base::Milliseconds(1500),
                               base::Milliseconds(1400),
                               /*downlink_throughput_kbps=*/60);
  RunPendingTasks();
  EXPECT_EQ(observer.connection_changes, 1);
  EXPECT_EQ(notifier_->EffectiveType(), WebEffectiveConnectionType::kType2G);
  notifier_->SetWebConnection(kWebConnectionTypeEthernet, 100.0);
  notifier_->SetSaveDataEnabled(false);
  notifier_->SetNetworkQualityWebHoldback(WebEffectiveConnectionType::kType3G);
  RunPendingTasks();
  EXPECT_EQ(notifier_->ConnectionType(), kWebConnectionTypeEthernet);
  EXPECT_FALSE(notifier_->SaveDataEnabled());
  EXPECT_EQ(notifier_->GetWebHoldbackEffectiveType(),
            WebEffectiveConnectionType::kType3G);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
