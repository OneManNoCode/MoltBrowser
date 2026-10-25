// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_device.h"

#include <memory>
#include <optional>
#include <string>

#include "services/network/public/cpp/client_hints.h"
#include "services/network/public/mojom/web_client_hints_types.mojom-shared.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/device_memory/approximated_device_memory.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/web/web_navigation_params.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/navigator.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/loader/frame_fetch_context.h"
#include "third_party/blink/renderer/core/loader/frame_loader.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_device_values.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/loader/fetch/client_hints_preferences.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_request.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "ui/gfx/geometry/size.h"

namespace blink::molt_fingerprinting {
namespace {

using network::mojom::WebClientHintsType;

// kUniform can't be configured here (it pins the process's time zone and
// locale for every later test), so MoltNet is simulated with
// SetMoltNetDeviceForTesting().
class MoltNetDeviceTest : public testing::Test {
 protected:
  void SetUp() override {
    ResetForTesting();
    SetMoltNetDeviceForTesting(false);
    // 16 GiB: reported as 16 on desktop platforms, 8 on Android.
    ApproximatedDeviceMemory::SetPhysicalMemoryMBForTesting(16384);
  }
  void TearDown() override {
    SetMoltNetDeviceForTesting(false);
    ResetForTesting();
  }

  // A secure document whose page asked for every client hint (Accept-CH),
  // with JavaScript on, as the browser commits it.
  Document& PageAskingForEveryHint() {
    holder_ = std::make_unique<DummyPageHolder>(gfx::Size(500, 500));
    holder_->GetFrame().Loader().CommitNavigation(
        WebNavigationParams::CreateWithEmptyHTMLForTesting(
            KURL("https://www.example.com/")),
        /*extra_data=*/nullptr);
    test::RunPendingTasks();
    Document& document = holder_->GetDocument();
    document.GetSettings()->SetScriptEnabled(true);
    ClientHintsPreferences preferences;
    for (const auto& [type, name] : network::GetClientHintToNameMap()) {
      preferences.SetShouldSend(type);
    }
    document.GetFrame()->GetClientHintsPreferences().UpdateFrom(preferences);
    return document;
  }

  // An image request from `document` to its own origin, 300 CSS pixels wide,
  // with the client hints Blink adds.
  ResourceRequest SubresourceRequest(Document& document) {
    ResourceRequest request(KURL("https://www.example.com/1.png"));
    static_cast<FrameFetchContext&>(document.Fetcher()->Context())
        .AddClientHintsIfNecessary(300.0f, request);
    return request;
  }

  static bool Has(const ResourceRequest& request, const std::string& header) {
    return !request.HttpHeaderField(AtomicString(header.c_str())).IsNull();
  }

  test::TaskEnvironment task_environment_;
  std::unique_ptr<DummyPageHolder> holder_;
};

TEST_F(MoltNetDeviceTest, DeviceMemory) {
  EXPECT_EQ(DeviceMemory(16.0f), 16.0f);
  EXPECT_EQ(DeviceMemory(2.0f), 2.0f);
  SetMoltNetDeviceForTesting(true);
  EXPECT_EQ(DeviceMemory(16.0f), moltnet::kDeviceMemory);
  EXPECT_EQ(DeviceMemory(2.0f), moltnet::kDeviceMemory);
}

TEST_F(MoltNetDeviceTest, NavigatorDeviceMemory) {
  Document& document = PageAskingForEveryHint();
  Navigator& navigator = *document.domWindow()->navigator();
  EXPECT_EQ(navigator.deviceMemory(),
            ApproximatedDeviceMemory::GetApproximatedDeviceMemory());
  EXPECT_NE(navigator.deviceMemory(), 4.0f);
  SetMoltNetDeviceForTesting(true);
  EXPECT_EQ(navigator.deviceMemory(), 4.0f);
}

TEST_F(MoltNetDeviceTest, WithholdsClientHints) {
  for (const auto& [type, name] : network::GetClientHintToNameMap()) {
    EXPECT_FALSE(WithholdsClientHint(type)) << name;
  }
  SetMoltNetDeviceForTesting(true);
  for (const auto& [type, name] : network::GetClientHintToNameMap()) {
    EXPECT_EQ(WithholdsClientHint(type),
              !moltnet::SendsClientHint(static_cast<int32_t>(type)))
        << name;
  }
  EXPECT_TRUE(WithholdsClientHint(WebClientHintsType::kDpr));
  EXPECT_TRUE(WithholdsClientHint(WebClientHintsType::kDeviceMemory));
  EXPECT_TRUE(WithholdsClientHint(WebClientHintsType::kUAPlatformVersion));
  EXPECT_FALSE(WithholdsClientHint(WebClientHintsType::kUA));
  EXPECT_FALSE(WithholdsClientHint(WebClientHintsType::kUAPlatform));
}

// What a page that asked for every hint gets on its subresource requests.
// Only hints the page could enable count (a feature may turn off a
// deprecated one); Save-Data is only sent while data saver is on.
TEST_F(MoltNetDeviceTest, SubresourceRequestsCarryOnlyTheAllowedHints) {
  Document& document = PageAskingForEveryHint();
  const ClientHintsPreferences& asked =
      document.GetFrame()->GetClientHintsPreferences();
  auto checked = [&asked](WebClientHintsType type) {
    return asked.ShouldSend(type) && type != WebClientHintsType::kSaveData;
  };

  const ResourceRequest normal = SubresourceRequest(document);
  for (const auto& [type, name] : network::GetClientHintToNameMap()) {
    if (checked(type)) {
      EXPECT_TRUE(Has(normal, name)) << name;
    }
  }

  SetMoltNetDeviceForTesting(true);
  const ResourceRequest moltnet = SubresourceRequest(document);
  for (const auto& [type, name] : network::GetClientHintToNameMap()) {
    if (checked(type)) {
      EXPECT_EQ(Has(moltnet, name),
                moltnet::SendsClientHint(static_cast<int32_t>(type)))
          << name;
    }
  }
  for (const char* header : {"DPR",
                             "Sec-CH-DPR",
                             "Width",
                             "Sec-CH-Width",
                             "Viewport-Width",
                             "Sec-CH-Viewport-Width",
                             "Sec-CH-Viewport-Height",
                             "Device-Memory",
                             "Sec-CH-Device-Memory",
                             "RTT",
                             "Downlink",
                             "ECT",
                             "Sec-CH-UA-Arch",
                             "Sec-CH-UA-Platform-Version",
                             "Sec-CH-UA-Model",
                             "Sec-CH-UA-Full-Version",
                             "Sec-CH-UA-Full-Version-List",
                             "Sec-CH-UA-Bitness",
                             "Sec-CH-UA-WoW64",
                             "Sec-CH-UA-Form-Factors"}) {
    EXPECT_FALSE(Has(moltnet, header)) << header;
  }
  for (const char* header :
       {"Sec-CH-UA", "Sec-CH-UA-Mobile", "Sec-CH-UA-Platform",
        "Sec-CH-Prefers-Color-Scheme", "Sec-CH-Prefers-Reduced-Motion",
        "Sec-CH-Prefers-Reduced-Transparency"}) {
    EXPECT_TRUE(Has(moltnet, header)) << header;
  }
}

UserAgentMetadata RealMetadata() {
  UserAgentMetadata metadata;
  metadata.brand_version_list.emplace_back("Chromium", "152");
  metadata.brand_full_version_list.emplace_back("Chromium", "152.0.7977.169");
  metadata.full_version = "152.0.7977.169";
  metadata.platform = "Windows";
  metadata.platform_version = "19.0.0";
  metadata.architecture = "arm";
  metadata.model = "";
  metadata.mobile = false;
  metadata.bitness = "64";
  metadata.wow64 = true;
  metadata.form_factors = {kDesktopFormFactor};
  return metadata;
}

TEST_F(MoltNetDeviceTest, UserAgentMetadata) {
  UserAgentMetadata metadata = RealMetadata();
  UniformUserAgentMetadata(metadata);
  EXPECT_EQ(metadata, RealMetadata());

  SetMoltNetDeviceForTesting(true);
  UniformUserAgentMetadata(metadata);
  // The low-entropy values stay.
  EXPECT_EQ(metadata.brand_version_list, RealMetadata().brand_version_list);
  EXPECT_EQ(metadata.platform, "Windows");
  EXPECT_FALSE(metadata.mobile);
  // The high-entropy ones are blank.
  EXPECT_TRUE(metadata.brand_full_version_list.empty());
  EXPECT_EQ(metadata.full_version, "");
  EXPECT_EQ(metadata.platform_version, "");
  EXPECT_EQ(metadata.architecture, "");
  EXPECT_EQ(metadata.model, "");
  EXPECT_EQ(metadata.bitness, "");
  EXPECT_FALSE(metadata.wow64);
  EXPECT_TRUE(metadata.form_factors.empty());

  // Every MoltNet user on the same platform and major version reports the
  // same, whatever the machine.
  UserAgentMetadata other = RealMetadata();
  other.platform_version = "10.0.0";
  other.architecture = "x86";
  other.wow64 = false;
  other.brand_full_version_list[0].version = "152.0.7977.100";
  other.full_version = "152.0.7977.100";
  UniformUserAgentMetadata(other);
  EXPECT_EQ(other, metadata);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
