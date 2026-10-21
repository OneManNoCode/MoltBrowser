// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_request_binders.h"

#include <memory>
#include <string>
#include <utility>

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/device/public/mojom/geolocation.mojom.h"
#include "services/image_annotation/public/mojom/image_annotation.mojom.h"
#include "third_party/blink/public/mojom/geolocation/geolocation_service.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom.h"

namespace molt_ai::tor {

namespace {

// What a MoltNet frame gets instead of the AccessibilityLabelsService's
// annotator (services/image_annotation/annotator.cc), which uploads image
// URLs and pixels through the regular profile's network context.
class RefusingImageAnnotator : public image_annotation::mojom::Annotator {
 public:
  RefusingImageAnnotator() = default;
  RefusingImageAnnotator(const RefusingImageAnnotator&) = delete;
  RefusingImageAnnotator& operator=(const RefusingImageAnnotator&) = delete;
  ~RefusingImageAnnotator() override = default;

  // image_annotation::mojom::Annotator:
  void AnnotateImage(
      const std::string& source_id,
      const std::string& description_language_tag,
      mojo::PendingRemote<image_annotation::mojom::ImageProcessor>
          image_processor,
      AnnotateImageCallback callback) override {
    // `image_processor` is dropped unused: the pixels are never read.
    std::move(callback).Run(
        image_annotation::mojom::AnnotateImageResult::NewErrorCode(
            image_annotation::mojom::AnnotateImageError::kFailure));
  }
};

// What a MoltNet frame gets instead of content's GeolocationServiceImpl. It
// answers the way content does when the user denies the permission: the
// status, and the Geolocation receiver dropped unbound, so Blink reports
// PERMISSION_DENIED and the device service never starts a location
// provider.
class RefusingGeolocationService : public blink::mojom::GeolocationService {
 public:
  RefusingGeolocationService() = default;
  RefusingGeolocationService(const RefusingGeolocationService&) = delete;
  RefusingGeolocationService& operator=(const RefusingGeolocationService&) =
      delete;
  ~RefusingGeolocationService() override = default;

  // blink::mojom::GeolocationService:
  void CreateGeolocation(
      mojo::PendingReceiver<device::mojom::Geolocation> receiver,
      bool user_gesture,
      blink::mojom::GeolocationAccuracy accuracy,
      CreateGeolocationCallback callback) override {
    // `receiver` goes out of scope unbound, as in content's denial path.
    std::move(callback).Run(blink::mojom::PermissionStatus::DENIED);
  }
};

void BindRefusingImageAnnotatorForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<image_annotation::mojom::Annotator> receiver) {
  BindRefusingImageAnnotator(std::move(receiver));
}

void BindRefusingGeolocationServiceForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<blink::mojom::GeolocationService> receiver) {
  BindRefusingGeolocationService(std::move(receiver));
}

}  // namespace

void ApplyMoltNetRequestAccessToFrameBinders(
    const MoltNetRequestAccess& access,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  if (!map) {
    return;
  }
  if (!access.image_descriptions) {
    map->Add<image_annotation::mojom::Annotator>(
        &BindRefusingImageAnnotatorForFrame);
  }
  if (!access.geolocation) {
    map->Add<blink::mojom::GeolocationService>(
        &BindRefusingGeolocationServiceForFrame);
  }
}

void BindRefusingImageAnnotator(
    mojo::PendingReceiver<image_annotation::mojom::Annotator> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<RefusingImageAnnotator>(),
                              std::move(receiver));
}

void BindRefusingGeolocationService(
    mojo::PendingReceiver<blink::mojom::GeolocationService> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<RefusingGeolocationService>(),
                              std::move(receiver));
}

}  // namespace molt_ai::tor
