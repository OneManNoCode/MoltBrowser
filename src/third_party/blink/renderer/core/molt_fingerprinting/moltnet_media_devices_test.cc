// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_devices.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/mediastream/media_devices.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"
#include "third_party/blink/renderer/core/testing/page_test_base.h"

namespace blink::molt_fingerprinting {
namespace {

using mojom::MediaStreamRequestResult;

// The browser's permission service: answers RequestPermissions() with one
// configured status, and keeps what was asked.
class FakePermissionService final : public mojom::blink::PermissionService {
 public:
  void Bind(mojo::ScopedMessagePipeHandle handle) {
    receivers_.Add(this, mojo::PendingReceiver<mojom::blink::PermissionService>(
                             std::move(handle)));
  }

  void HasPermission(mojom::blink::PermissionDescriptorPtr permission,
                     HasPermissionCallback callback) override {
    std::move(callback).Run(Status());
  }
  void RegisterPageEmbeddedPermissionControl(
      Vector<mojom::blink::PermissionDescriptorPtr> permissions,
      mojom::blink::EmbeddedPermissionRequestDescriptorPtr descriptor,
      mojo::PendingRemote<mojom::blink::EmbeddedPermissionControlClient>
          pending_client) override {}
  void RequestPageEmbeddedPermission(
      Vector<mojom::blink::PermissionDescriptorPtr> permissions,
      mojom::blink::EmbeddedPermissionRequestDescriptorPtr descriptors,
      RequestPageEmbeddedPermissionCallback callback) override {}
  void RequestPermission(mojom::blink::PermissionDescriptorPtr permission,
                         RequestPermissionCallback callback) override {}
  void RequestPermissions(
      Vector<mojom::blink::PermissionDescriptorPtr> permissions,
      RequestPermissionsCallback callback) override {
    Vector<mojom::blink::PermissionStatusWithDetailsPtr> statuses;
    for (const auto& permission : permissions) {
      asked.push_back(permission->name);
      statuses.push_back(Status());
    }
    std::move(callback).Run(std::move(statuses));
  }
  void RevokePermission(mojom::blink::PermissionDescriptorPtr permission,
                        RevokePermissionCallback callback) override {}
  void AddPermissionObserver(
      mojom::blink::PermissionDescriptorPtr permission,
      mojom::blink::PermissionStatusWithDetailsPtr last_known_status,
      mojo::PendingRemote<mojom::blink::PermissionObserver> observer)
      override {}
  void AddPageEmbeddedPermissionObserver(
      mojom::blink::PermissionDescriptorPtr permission,
      mojom::blink::PermissionStatus last_known_status,
      mojo::PendingRemote<mojom::blink::PermissionObserver> observer)
      override {}
  void NotifyEventListener(mojom::blink::PermissionDescriptorPtr permission,
                           const String& event_type,
                           bool is_added) override {}

  mojom::blink::PermissionStatusWithDetailsPtr Status() const {
    auto status = mojom::blink::PermissionStatusWithDetails::New();
    status->status = answer;
    return status;
  }

  mojom::blink::PermissionStatus answer =
      mojom::blink::PermissionStatus::ASK;
  Vector<mojom::blink::PermissionName> asked;

 private:
  mojo::ReceiverSet<mojom::blink::PermissionService> receivers_;
};

class MoltNetMediaDevicesTest : public PageTestBase {
 protected:
  void SetUp() override {
    PageTestBase::SetUp();
    ResetForTesting();
    SetMoltNetMediaForTesting(true);
    Window().GetBrowserInterfaceBroker().SetBinderForTesting(
        mojom::blink::PermissionService::Name_,
        base::BindRepeating(&FakePermissionService::Bind,
                            base::Unretained(&service_)));
  }
  void TearDown() override {
    Window().GetBrowserInterfaceBroker().SetBinderForTesting(
        mojom::blink::PermissionService::Name_, {});
    SetMoltNetMediaForTesting(false);
    ResetForTesting();
    PageTestBase::TearDown();
  }

  LocalDOMWindow& Window() { return *GetFrame().DomWindow(); }

  // getUserMedia() failing with `failure`: what it reports instead, or
  // nullopt when it reports `failure` at once.
  std::optional<MediaStreamRequestResult> Fail(bool audio,
                                               bool video,
                                               MediaStreamRequestResult failure) {
    std::optional<MediaStreamRequestResult> reported;
    base::RunLoop run_loop;
    if (!ConfirmUserMediaFailure(
            &Window(), audio, video, failure,
            base::BindOnce(
                [](std::optional<MediaStreamRequestResult>* out,
                   base::OnceClosure quit, MediaStreamRequestResult result) {
                  *out = result;
                  std::move(quit).Run();
                },
                &reported, run_loop.QuitClosure()))) {
      return std::nullopt;
    }
    run_loop.Run();
    EXPECT_TRUE(reported.has_value());
    return reported;
  }

  FakePermissionService service_;
};

Vector<Vector<WebMediaDeviceInfo>> Enumeration() {
  return Vector<Vector<WebMediaDeviceInfo>>(
      static_cast<wtf_size_t>(mojom::MediaDeviceType::kNumMediaDeviceTypes));
}

TEST_F(MoltNetMediaDevicesTest, OneOfEachKind) {
  const mojom::MediaDeviceType kinds[] = {
      mojom::MediaDeviceType::kMediaAudioInput,
      mojom::MediaDeviceType::kMediaVideoInput,
      mojom::MediaDeviceType::kMediaAudioOutput};
  for (const mojom::MediaDeviceType kind : kinds) {
    Vector<Vector<WebMediaDeviceInfo>> enumeration = Enumeration();
    // None of that kind listed: one is.
    EXPECT_TRUE(ShouldListUniformDevice(kind, enumeration));
    // One listed (no permission, so no ID): it stays alone.
    enumeration[static_cast<wtf_size_t>(kind)].push_back(WebMediaDeviceInfo());
    EXPECT_FALSE(ShouldListUniformDevice(kind, enumeration));
    // Devices the page may see: they are what they are.
    WebMediaDeviceInfo device;
    device.device_id = "some-device";
    enumeration[static_cast<wtf_size_t>(kind)][0] = device;
    EXPECT_FALSE(ShouldListUniformDevice(kind, enumeration));
  }
  // Microphone permission shows both audio kinds: with a microphone's ID
  // listed, no output means none (as in Chromium); a camera still gets one.
  {
    Vector<Vector<WebMediaDeviceInfo>> enumeration = Enumeration();
    WebMediaDeviceInfo mic;
    mic.device_id = "some-microphone";
    enumeration[static_cast<wtf_size_t>(
                    mojom::MediaDeviceType::kMediaAudioInput)]
        .push_back(mic);
    EXPECT_FALSE(ShouldListUniformDevice(
        mojom::MediaDeviceType::kMediaAudioOutput, enumeration));
    EXPECT_TRUE(ShouldListUniformDevice(
        mojom::MediaDeviceType::kMediaVideoInput, enumeration));
  }
  EXPECT_FALSE(ShouldListUniformDevice(
      mojom::MediaDeviceType::kNumMediaDeviceTypes, Enumeration()));
  EXPECT_FALSE(ShouldListUniformDevice(mojom::MediaDeviceType::kMediaAudioInput,
                                       {}));
  SetMoltNetMediaForTesting(false);
  EXPECT_FALSE(ShouldListUniformDevice(
      mojom::MediaDeviceType::kMediaAudioOutput, Enumeration()));
}

TEST_F(MoltNetMediaDevicesTest, DeviceChangeNeedsPermission) {
  EXPECT_TRUE(DeviceChangeNeedsPermission());
  SetMoltNetMediaForTesting(false);
  EXPECT_FALSE(DeviceChangeNeedsPermission());
}

// No camera or microphone: getUserMedia() asks first, then answers as a
// request a device could meet would be answered.
TEST_F(MoltNetMediaDevicesTest, UserMediaAsksFirst) {
  service_.answer = mojom::blink::PermissionStatus::ASK;
  EXPECT_EQ(Fail(true, false, MediaStreamRequestResult::NO_HARDWARE),
            MediaStreamRequestResult::PERMISSION_DISMISSED);
  ASSERT_EQ(service_.asked.size(), 1u);
  EXPECT_EQ(service_.asked[0], mojom::blink::PermissionName::AUDIO_CAPTURE);

  service_.asked.clear();
  service_.answer = mojom::blink::PermissionStatus::DENIED;
  EXPECT_EQ(Fail(true, true, MediaStreamRequestResult::NO_HARDWARE),
            MediaStreamRequestResult::PERMISSION_DENIED);
  ASSERT_EQ(service_.asked.size(), 2u);
  EXPECT_EQ(service_.asked[0], mojom::blink::PermissionName::AUDIO_CAPTURE);
  EXPECT_EQ(service_.asked[1], mojom::blink::PermissionName::VIDEO_CAPTURE);

  // Granted: the real failure.
  service_.answer = mojom::blink::PermissionStatus::GRANTED;
  EXPECT_EQ(Fail(false, true, MediaStreamRequestResult::NO_HARDWARE),
            MediaStreamRequestResult::NO_HARDWARE);
  EXPECT_EQ(
      Fail(false, true, MediaStreamRequestResult::CONSTRAINT_NOT_SATISFIED),
      MediaStreamRequestResult::CONSTRAINT_NOT_SATISFIED);
}

TEST_F(MoltNetMediaDevicesTest, OtherFailuresStayAsTheyAre) {
  EXPECT_EQ(Fail(true, false, MediaStreamRequestResult::PERMISSION_DENIED),
            std::nullopt);
  EXPECT_EQ(Fail(true, false, MediaStreamRequestResult::TRACK_START_FAILURE_AUDIO),
            std::nullopt);
  // Not a camera or microphone request (a screen capture).
  EXPECT_EQ(Fail(false, false, MediaStreamRequestResult::NO_HARDWARE),
            std::nullopt);
  EXPECT_FALSE(ConfirmUserMediaFailure(nullptr, true, false,
                                       MediaStreamRequestResult::NO_HARDWARE,
                                       base::DoNothing()));
  SetMoltNetMediaForTesting(false);
  EXPECT_EQ(Fail(true, false, MediaStreamRequestResult::NO_HARDWARE),
            std::nullopt);
  EXPECT_TRUE(service_.asked.empty());
}

}  // namespace
}  // namespace blink::molt_fingerprinting
