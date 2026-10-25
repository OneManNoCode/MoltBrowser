// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A Background Fetch request of a regular profile is made with the fetch's
// credentials mode (components/download/internal/common/
// molt_fetch_credentials.h): one that asked for `credentials: 'omit'` sends
// no cookies. Through Chromium's real download request
// (download::CreateResourceRequest()) and a regular profile's real network
// context, to a test server that records what it receives.

#include "components/download/internal/common/molt_fetch_credentials.h"

#include <memory>
#include <string>
#include <vector>

#include "base/synchronization/lock.h"
#include "base/test/task_environment.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/download/public/common/download_utils.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/cookies/site_for_cookies.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_url_loader_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace download {
namespace {

using network::mojom::CredentialsMode;
using network::mojom::RequestMode;

// What a Background Fetch request's parameters carry in a regular profile
// (DownloadDriverImpl::Start()).
std::unique_ptr<DownloadUrlParameters> FetchParams(const GURL& url,
                                                   CredentialsMode mode) {
  auto params =
      std::make_unique<DownloadUrlParameters>(url, TRAFFIC_ANNOTATION_FOR_TESTS);
  const url::Origin origin = url::Origin::Create(url);
  params->set_initiator(origin);
  params->set_isolation_info(net::IsolationInfo::Create(
      net::IsolationInfo::RequestType::kOther, origin, origin,
      net::SiteForCookies::FromOrigin(origin)));
  params->set_credentials_mode(mode);
  return params;
}

TEST(MoltNetBackgroundFetchCredentialsTest, TheFetchsModeIsApplied) {
  const GURL url("https://a.test/movie.mp4");
  std::unique_ptr<network::ResourceRequest> omit =
      CreateResourceRequest(FetchParams(url, CredentialsMode::kOmit).get());
  EXPECT_EQ(omit->credentials_mode, CredentialsMode::kOmit);
  EXPECT_EQ(omit->mode, RequestMode::kNoCors);

  std::unique_ptr<network::ResourceRequest> same_origin = CreateResourceRequest(
      FetchParams(url, CredentialsMode::kSameOrigin).get());
  EXPECT_EQ(same_origin->credentials_mode, CredentialsMode::kSameOrigin);
  EXPECT_EQ(same_origin->mode, RequestMode::kNoCors);

  // Every other download: a navigation with credentials, as in Chromium.
  std::unique_ptr<network::ResourceRequest> include = CreateResourceRequest(
      FetchParams(url, CredentialsMode::kInclude).get());
  EXPECT_EQ(include->credentials_mode, CredentialsMode::kInclude);
  EXPECT_EQ(include->mode, RequestMode::kNavigate);
  DownloadUrlParameters link(url, TRAFFIC_ANNOTATION_FOR_TESTS);
  std::unique_ptr<network::ResourceRequest> plain =
      CreateResourceRequest(&link);
  EXPECT_EQ(plain->credentials_mode, CredentialsMode::kInclude);
  EXPECT_EQ(plain->mode, RequestMode::kNavigate);
}

TEST(MoltNetBackgroundFetchCredentialsTest, SameOriginNeedsAnOrigin) {
  static_assert(MoltDownloadCredentialsMode(CredentialsMode::kSameOrigin,
                                            /*has_initiator=*/false) ==
                CredentialsMode::kOmit);
  static_assert(MoltDownloadCredentialsMode(CredentialsMode::kSameOrigin,
                                            true) ==
                CredentialsMode::kSameOrigin);
  static_assert(MoltDownloadCredentialsMode(CredentialsMode::kInclude,
                                            false) ==
                CredentialsMode::kInclude);
  static_assert(MoltDownloadIsNavigation(CredentialsMode::kInclude));
  static_assert(!MoltDownloadIsNavigation(CredentialsMode::kOmit));
  network::ResourceRequest request;
  request.mode = RequestMode::kNavigate;
  MoltApplyDownloadCredentialsMode(CredentialsMode::kSameOrigin, request);
  EXPECT_EQ(request.credentials_mode, CredentialsMode::kOmit);
  EXPECT_EQ(request.mode, RequestMode::kNoCors);
}

// Through a regular profile's network context: the site has a cookie; the
// fetch that omits credentials doesn't send it, one that includes them does.
class MoltNetBackgroundFetchCookiesTest : public testing::Test {
 protected:
  void SetUp() override {
    server_.RegisterRequestMonitor(base::BindRepeating(
        &MoltNetBackgroundFetchCookiesTest::Record, base::Unretained(this)));
    server_.AddDefaultHandlers();
    ASSERT_TRUE(server_.Start());

    auto params = network::mojom::NetworkContextParams::New();
    params->cert_verifier_params =
        network::FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
    context_ = std::make_unique<network::NetworkContext>(
        service_.get(), context_remote_.BindNewPipeAndPassReceiver(),
        std::move(params));
    auto factory_params = network::mojom::URLLoaderFactoryParams::New();
    factory_params->process_id = network::OriginatingProcessId::browser();
    factory_params->is_trusted = true;
    factory_params->is_orb_enabled = false;
    context_->CreateURLLoaderFactory(factory_.BindNewPipeAndPassReceiver(),
                                     std::move(factory_params));
  }

  void Record(const net::test_server::HttpRequest& request) {
    base::AutoLock lock(lock_);
    auto it = request.headers.find("Cookie");
    cookies_.push_back(it == request.headers.end() ? "" : it->second);
  }

  // The Cookie header the server got with the request built from `params`.
  std::string Load(DownloadUrlParameters& params) {
    std::unique_ptr<network::ResourceRequest> request =
        CreateResourceRequest(&params);
    network::TestURLLoaderClient client;
    mojo::PendingRemote<network::mojom::URLLoader> loader;
    factory_->CreateLoaderAndStart(
        loader.InitWithNewPipeAndPassReceiver(), /*request_id=*/0,
        network::mojom::kURLLoadOptionNone, *request, client.CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
    client.RunUntilComplete();
    EXPECT_EQ(client.completion_status().error_code, net::OK);
    base::AutoLock lock(lock_);
    return cookies_.empty() ? "(no request)" : cookies_.back();
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  net::test_server::EmbeddedTestServer server_;
  std::unique_ptr<network::NetworkService> service_ =
      network::NetworkService::CreateForTesting();
  mojo::Remote<network::mojom::NetworkContext> context_remote_;
  std::unique_ptr<network::NetworkContext> context_;
  mojo::Remote<network::mojom::URLLoaderFactory> factory_;
  base::Lock lock_;
  std::vector<std::string> cookies_;
};

TEST_F(MoltNetBackgroundFetchCookiesTest, AnOmittingFetchSendsNoCookies) {
  // The site sets a cookie (a download that sends credentials, as the
  // site's page would).
  DownloadUrlParameters set(server_.GetURL("/set-cookie?molt=1"),
                            TRAFFIC_ANNOTATION_FOR_TESTS);
  Load(set);

  std::unique_ptr<DownloadUrlParameters> include =
      FetchParams(server_.GetURL("/echo"), CredentialsMode::kInclude);
  EXPECT_EQ(Load(*include), "molt=1");

  std::unique_ptr<DownloadUrlParameters> omit =
      FetchParams(server_.GetURL("/echo"), CredentialsMode::kOmit);
  EXPECT_EQ(Load(*omit), "");
}

}  // namespace
}  // namespace download
