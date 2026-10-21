// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_request_binders.h"

#include <utility>
#include <vector>

#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "chrome/browser/molt_ai/tor/molt_net_requests_policy.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "services/device/public/mojom/geolocation.mojom.h"
#include "services/image_annotation/public/mojom/image_annotation.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/geolocation/geolocation_service.mojom.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai::tor {
namespace {

using FrameBinderMap = mojo::BinderMapWithContext<content::RenderFrameHost*>;

// Stand-ins for Chrome's and content's own binders (BindImageAnnotator,
// RenderFrameHostImpl::GetGeolocationService): they count, and keep the
// pipe.
int g_upstream_annotator_binds = 0;
int g_upstream_geolocation_binds = 0;
std::vector<mojo::ScopedMessagePipeHandle>* g_kept_pipes = nullptr;

void UpstreamAnnotatorBinder(
    content::RenderFrameHost*,
    mojo::PendingReceiver<image_annotation::mojom::Annotator> receiver) {
  ++g_upstream_annotator_binds;
  g_kept_pipes->push_back(receiver.PassPipe());
}

void UpstreamGeolocationBinder(
    content::RenderFrameHost*,
    mojo::PendingReceiver<blink::mojom::GeolocationService> receiver) {
  ++g_upstream_geolocation_binds;
  g_kept_pipes->push_back(receiver.PassPipe());
}

class MoltNetRequestBindersTest : public testing::Test {
 protected:
  MoltNetRequestBindersTest() {
    g_upstream_annotator_binds = 0;
    g_upstream_geolocation_binds = 0;
    g_kept_pipes = &kept_pipes_;
    // What Chrome and content register for an ordinary frame.
    map_.Add<image_annotation::mojom::Annotator>(&UpstreamAnnotatorBinder);
    map_.Add<blink::mojom::GeolocationService>(&UpstreamGeolocationBinder);
  }
  ~MoltNetRequestBindersTest() override { g_kept_pipes = nullptr; }

  template <typename Interface>
  void Bind(mojo::Remote<Interface>& remote) {
    mojo::GenericPendingReceiver receiver(remote.BindNewPipeAndPassReceiver());
    ASSERT_TRUE(map_.TryBind(/*context=*/nullptr, &receiver));
  }

  base::test::TaskEnvironment task_environment_;
  std::vector<mojo::ScopedMessagePipeHandle> kept_pipes_;
  FrameBinderMap map_;
};

TEST_F(MoltNetRequestBindersTest, RegularProfilesKeepTheUpstreamBinders) {
  ApplyMoltNetRequestAccessToFrameBinders(
      MoltNetRequestAccessForProfile(/*is_moltnet_profile=*/false), &map_);
  mojo::Remote<image_annotation::mojom::Annotator> annotator;
  Bind(annotator);
  mojo::Remote<blink::mojom::GeolocationService> geolocation;
  Bind(geolocation);
  EXPECT_EQ(g_upstream_annotator_binds, 1);
  EXPECT_EQ(g_upstream_geolocation_binds, 1);
}

TEST_F(MoltNetRequestBindersTest, MoltNetFrameWithoutUpstreamBindersBinds) {
  FrameBinderMap empty;
  ApplyMoltNetRequestAccessToFrameBinders(
      MoltNetRequestAccessForProfile(/*is_moltnet_profile=*/true), &empty);
  EXPECT_TRUE(empty.Contains<image_annotation::mojom::Annotator>());
  EXPECT_TRUE(empty.Contains<blink::mojom::GeolocationService>());
}

TEST_F(MoltNetRequestBindersTest, NullMapIsIgnored) {
  ApplyMoltNetRequestAccessToFrameBinders(
      MoltNetRequestAccessForProfile(/*is_moltnet_profile=*/true), nullptr);
}

TEST_F(MoltNetRequestBindersTest, MoltNetImageDescriptionsFailWithoutPixels) {
  ApplyMoltNetRequestAccessToFrameBinders(
      MoltNetRequestAccessForProfile(/*is_moltnet_profile=*/true), &map_);
  mojo::Remote<image_annotation::mojom::Annotator> annotator;
  Bind(annotator);

  for (const char* source_id :
       {"https://site.example/private/photo.jpg", "data-uri-hash-1234", ""}) {
    SCOPED_TRACE(source_id);
    // The renderer's image processor: never asked for the pixels.
    mojo::PendingRemote<image_annotation::mojom::ImageProcessor> processor;
    mojo::PendingReceiver<image_annotation::mojom::ImageProcessor>
        processor_end = processor.InitWithNewPipeAndPassReceiver();

    base::test::TestFuture<image_annotation::mojom::AnnotateImageResultPtr>
        result;
    annotator->AnnotateImage(source_id, "en", std::move(processor),
                             result.GetCallback());
    image_annotation::mojom::AnnotateImageResultPtr annotated = result.Take();
    ASSERT_TRUE(annotated);
    ASSERT_TRUE(annotated->is_error_code());
    EXPECT_EQ(image_annotation::mojom::AnnotateImageError::kFailure,
              annotated->get_error_code());

    // The stand-in let go of the processor without calling it.
    base::RunLoop().RunUntilIdle();
    mojo::ScopedMessagePipeHandle processor_pipe = processor_end.PassPipe();
    EXPECT_TRUE(processor_pipe->QuerySignalsState().peer_closed());
    EXPECT_FALSE(processor_pipe->QuerySignalsState().readable());
  }
  EXPECT_TRUE(annotator.is_connected());
  EXPECT_EQ(g_upstream_annotator_binds, 0);
}

TEST_F(MoltNetRequestBindersTest, MoltNetLocationIsDeniedWithoutAProvider) {
  ApplyMoltNetRequestAccessToFrameBinders(
      MoltNetRequestAccessForProfile(/*is_moltnet_profile=*/true), &map_);
  mojo::Remote<blink::mojom::GeolocationService> service;
  Bind(service);

  for (bool user_gesture : {false, true}) {
    for (blink::mojom::GeolocationAccuracy accuracy :
         {blink::mojom::GeolocationAccuracy::kPrecise,
          blink::mojom::GeolocationAccuracy::kApproximate}) {
      SCOPED_TRACE(testing::Message() << "user_gesture=" << user_gesture);
      mojo::Remote<device::mojom::Geolocation> geolocation;
      base::RunLoop dropped;
      mojo::PendingReceiver<device::mojom::Geolocation> receiver =
          geolocation.BindNewPipeAndPassReceiver();
      geolocation.set_disconnect_handler(dropped.QuitClosure());

      base::test::TestFuture<blink::mojom::PermissionStatus> status;
      service->CreateGeolocation(std::move(receiver), user_gesture, accuracy,
                                 status.GetCallback());
      EXPECT_EQ(blink::mojom::PermissionStatus::DENIED, status.Get());
      // No Geolocation was bound: the renderer's end sees it closed, as
      // after content's own denial.
      dropped.Run();
      EXPECT_FALSE(geolocation.is_connected());
    }
  }
  // The service itself stays usable for the next request.
  EXPECT_TRUE(service.is_connected());
  EXPECT_EQ(g_upstream_geolocation_binds, 0);
}

}  // namespace
}  // namespace molt_ai::tor
