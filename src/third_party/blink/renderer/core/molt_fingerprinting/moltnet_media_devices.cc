// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_devices.h"

#include <array>
#include <span>
#include <utility>

#include "third_party/blink/public/common/mediastream/media_devices.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_media_values.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_remote.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink::molt_fingerprinting {

namespace {

bool HasDeviceId(const Vector<WebMediaDeviceInfo>& devices) {
  for (const WebMediaDeviceInfo& device : devices) {
    if (!device.device_id.empty()) {
      return true;
    }
  }
  return false;
}

mojom::blink::PermissionDescriptorPtr Descriptor(
    mojom::blink::PermissionName name) {
  auto descriptor = mojom::blink::PermissionDescriptor::New();
  descriptor->name = name;
  return descriptor;
}

moltnet::PermissionAnswer AnswerFor(
    const mojom::blink::PermissionStatusWithDetailsPtr& status) {
  if (!status) {
    return moltnet::PermissionAnswer::kDenied;
  }
  switch (status->status) {
    case mojom::blink::PermissionStatus::GRANTED:
      return moltnet::PermissionAnswer::kGranted;
    case mojom::blink::PermissionStatus::ASK:
      return moltnet::PermissionAnswer::kAsk;
    case mojom::blink::PermissionStatus::DENIED:
      return moltnet::PermissionAnswer::kDenied;
  }
  return moltnet::PermissionAnswer::kDenied;
}

// Asks for the camera and microphone permissions a failing getUserMedia()
// request needs, then reports its result. Kept alive by the pending reply;
// the window going away drops it.
class UserMediaFailureConfirmation final
    : public GarbageCollected<UserMediaFailureConfirmation> {
 public:
  UserMediaFailureConfirmation(
      LocalDOMWindow& window,
      mojom::MediaStreamRequestResult failure,
      base::OnceCallback<void(mojom::MediaStreamRequestResult)> done)
      : service_(&window), failure_(failure), done_(std::move(done)) {}

  void Start(LocalDOMWindow& window, bool audio, bool video) {
    window.GetBrowserInterfaceBroker().GetInterface(
        service_.BindNewPipeAndPassReceiver(
            window.GetTaskRunner(TaskType::kMiscPlatformAPI)));
    Vector<mojom::blink::PermissionDescriptorPtr> permissions;
    if (audio) {
      permissions.push_back(
          Descriptor(mojom::blink::PermissionName::AUDIO_CAPTURE));
    }
    if (video) {
      permissions.push_back(
          Descriptor(mojom::blink::PermissionName::VIDEO_CAPTURE));
    }
    service_->RequestPermissions(
        std::move(permissions),
        BindOnce(&UserMediaFailureConfirmation::OnAnswered,
                 WrapPersistent(this)));
  }

  void Trace(Visitor* visitor) const { visitor->Trace(service_); }

 private:
  void OnAnswered(
      Vector<mojom::blink::PermissionStatusWithDetailsPtr> statuses) {
    std::array<moltnet::PermissionAnswer, 2> answers = {};
    size_t count = 0;
    for (const auto& status : statuses) {
      if (count == answers.size()) {
        break;
      }
      answers[count++] = AnswerFor(status);
    }
    mojom::MediaStreamRequestResult result =
        mojom::MediaStreamRequestResult::PERMISSION_DENIED;
    switch (moltnet::UserMediaFailureVerdict(
        std::span<const moltnet::PermissionAnswer>(answers).first(count))) {
      case moltnet::UserMediaVerdict::kRealFailure:
        result = failure_;
        break;
      case moltnet::UserMediaVerdict::kPermissionDenied:
        result = mojom::MediaStreamRequestResult::PERMISSION_DENIED;
        break;
      case moltnet::UserMediaVerdict::kPermissionDismissed:
        result = mojom::MediaStreamRequestResult::PERMISSION_DISMISSED;
        break;
    }
    service_.reset();
    std::move(done_).Run(result);
  }

  HeapMojoRemote<mojom::blink::PermissionService> service_;
  const mojom::MediaStreamRequestResult failure_;
  base::OnceCallback<void(mojom::MediaStreamRequestResult)> done_;
};

}  // namespace

bool ShouldListUniformDevice(
    mojom::MediaDeviceType kind,
    const Vector<Vector<WebMediaDeviceInfo>>& enumeration) {
  if (!UsesUniformMediaDevices()) {
    return false;
  }
  const auto index = static_cast<wtf_size_t>(kind);
  if (kind == mojom::MediaDeviceType::kNumMediaDeviceTypes ||
      index >= enumeration.size()) {
    return false;
  }
  bool ids_exposed = HasDeviceId(enumeration[index]);
  if (kind != mojom::MediaDeviceType::kMediaVideoInput) {
    // Microphone permission shows both audio kinds.
    for (const mojom::MediaDeviceType audio :
         {mojom::MediaDeviceType::kMediaAudioInput,
          mojom::MediaDeviceType::kMediaAudioOutput}) {
      const auto other = static_cast<wtf_size_t>(audio);
      ids_exposed |= other < enumeration.size() &&
                     HasDeviceId(enumeration[other]);
    }
  }
  return moltnet::ListsUniformDevice(enumeration[index].size(), ids_exposed);
}

bool DeviceChangeNeedsPermission() {
  return UsesUniformMediaDevices();
}

bool ConfirmUserMediaFailure(
    LocalDOMWindow* window,
    bool audio,
    bool video,
    mojom::MediaStreamRequestResult result,
    base::OnceCallback<void(mojom::MediaStreamRequestResult)> done) {
  if (!UsesUniformMediaDevices() || !window || (!audio && !video)) {
    return false;
  }
  if (result != mojom::MediaStreamRequestResult::NO_HARDWARE &&
      result != mojom::MediaStreamRequestResult::CONSTRAINT_NOT_SATISFIED) {
    return false;
  }
  MakeGarbageCollected<UserMediaFailureConfirmation>(*window, result,
                                                     std::move(done))
      ->Start(*window, audio, video);
  return true;
}

}  // namespace blink::molt_fingerprinting
