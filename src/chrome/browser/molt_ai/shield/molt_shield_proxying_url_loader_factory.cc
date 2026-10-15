// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_proxying_url_loader_factory.h"

#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/data_url.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_version.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/url_constants.h"

namespace molt_ai::shield {

namespace {

using FactoryType = content::ContentBrowserClient::URLLoaderFactoryType;

// Surrogate bodies are small (uBO's largest is ~40 KB).
constexpr size_t kMaxSurrogateBytes = 256 * 1024;

// Owns the proxies of one profile so they die with it.
class ProxySet : public base::SupportsUserData::Data {
 public:
  static ProxySet* Get(content::BrowserContext* context) {
    auto* self = static_cast<ProxySet*>(context->GetUserData(&kKey));
    if (!self) {
      auto owned = std::make_unique<ProxySet>();
      self = owned.get();
      context->SetUserData(&kKey, std::move(owned));
    }
    return self;
  }

  void Add(std::unique_ptr<MoltShieldProxyingURLLoaderFactory> proxy) {
    proxies_.insert(std::move(proxy));
  }

  void Remove(MoltShieldProxyingURLLoaderFactory* proxy) {
    auto it = proxies_.find(proxy);
    if (it != proxies_.end()) {
      proxies_.erase(it);
    }
  }

  base::WeakPtr<ProxySet> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  static const int kKey = 0;
  std::set<std::unique_ptr<MoltShieldProxyingURLLoaderFactory>,
           base::UniquePtrComparator>
      proxies_;
  base::WeakPtrFactory<ProxySet> weak_factory_{this};
};

const int ProxySet::kKey;

bool ShouldProxy(FactoryType type) {
  switch (type) {
    case FactoryType::kDocumentSubResource:
    case FactoryType::kWorkerMainResource:
    case FactoryType::kWorkerSubResource:
    case FactoryType::kServiceWorkerScript:
    case FactoryType::kServiceWorkerSubResource:
    case FactoryType::kPrefetch:
    case FactoryType::kEarlyHints:
      return true;
    case FactoryType::kNavigation:  // MoltShieldNavigationThrottle.
    case FactoryType::kDownload:    // User-initiated.
    case FactoryType::kDevTools:
      return false;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// InProgressRequest

class MoltShieldProxyingURLLoaderFactory::InProgressRequest
    : public network::mojom::URLLoader,
      public network::mojom::URLLoaderClient {
 public:
  InProgressRequest(
      MoltShieldProxyingURLLoaderFactory* factory,
      mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      : factory_(factory),
        request_id_(request_id),
        options_(options),
        request_(request),
        request_type_(RequestTypeForDestination(request.destination,
                                                request.keepalive)),
        traffic_annotation_(traffic_annotation),
        loader_receiver_(this, std::move(loader_receiver)),
        target_client_(std::move(client)) {
    loader_receiver_.set_disconnect_handler(base::BindOnce(
        &InProgressRequest::OnRendererDisconnect, base::Unretained(this)));
    target_client_.set_disconnect_handler(base::BindOnce(
        &InProgressRequest::OnRendererDisconnect, base::Unretained(this)));
  }

  InProgressRequest(const InProgressRequest&) = delete;
  InProgressRequest& operator=(const InProgressRequest&) = delete;
  ~InProgressRequest() override = default;

  void Start() {
    std::optional<CheckInput> input = MakeInput(request_.url);
    if (!input) {
      StartTarget();
      return;
    }
    MoltShieldService::Get()->Check(
        std::move(*input),
        base::BindOnce(&InProgressRequest::OnStartChecked,
                       weak_factory_.GetWeakPtr()));
  }

  // network::mojom::URLLoader (from the renderer):
  void FollowRedirect(
      const std::vector<std::string>& removed_headers,
      const net::HttpRequestHeaders& modified_headers,
      const net::HttpRequestHeaders& modified_cors_exempt_headers,
      const std::optional<GURL>& new_url) override {
    if (target_loader_.is_bound()) {
      target_loader_->FollowRedirect(removed_headers, modified_headers,
                                     modified_cors_exempt_headers, new_url);
    }
  }

  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {
    if (target_loader_.is_bound()) {
      target_loader_->SetPriority(priority, intra_priority_value);
    } else {
      pending_priority_ = std::make_pair(priority, intra_priority_value);
    }
  }

  // network::mojom::URLLoaderClient (from the network service):
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr early_hints) override {
    target_client_->OnReceiveEarlyHints(std::move(early_hints));
  }

  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override {
    target_client_->OnReceiveResponse(std::move(head), std::move(body),
                                      std::move(cached_metadata));
  }

  void OnReceiveRedirect(const net::RedirectInfo& redirect_info,
                         network::mojom::URLResponseHeadPtr head) override {
    std::optional<CheckInput> input = MakeInput(redirect_info.new_url);
    if (!input) {
      target_client_->OnReceiveRedirect(redirect_info, std::move(head));
      return;
    }
    // Hold any further network messages until the new URL is vetted, so the
    // renderer never sees them out of order.
    client_receiver_.Pause();
    MoltShieldService::Get()->Check(
        std::move(*input),
        base::BindOnce(&InProgressRequest::OnRedirectChecked,
                       weak_factory_.GetWeakPtr(), redirect_info,
                       std::move(head)));
  }

  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback callback) override {
    target_client_->OnUploadProgress(current_position, total_size,
                                     std::move(callback));
  }

  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {
    target_client_->OnTransferSizeUpdated(transfer_size_diff);
  }

  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    completed_ = true;
    target_client_->OnComplete(status);
  }

 private:
  std::optional<CheckInput> MakeInput(const GURL& url) {
    MoltShieldService* service = MoltShieldService::Get();
    // Documents (e.g. speculation-rules prefetches) are never blocked.
    if (!service->IsEnabled() || request_type_ == kTypeDocument ||
        request_type_ == kTypeSubdocument) {
      return std::nullopt;
    }
    std::optional<url::Origin> top = factory_->top_frame_origin_;
    if (!top && request_.trusted_params) {
      top = request_.trusted_params->isolation_info.top_frame_origin();
    }
    if (top && !AreShieldsUpForSite(factory_->browser_context_,
                                    top->GetURL())) {
      return std::nullopt;
    }
    return MakeCheckInput(url, request_.request_initiator, request_type_,
                          request_.method);
  }

  void OnStartChecked(CheckResult result) {
    MoltShieldService::Get()->RecordCheck(result, request_type_);
    if (!result.should_block) {
      StartTarget();
      return;
    }
    RecordBlockedForFrame(
        content::RenderFrameHost::FromID(factory_->frame_id_));
    if (!result.redirect_data_url.empty() &&
        request_.mode == network::mojom::RequestMode::kNoCors &&
        ServeSurrogate(result.redirect_data_url)) {
      return;
    }
    CompleteWithError(net::ERR_BLOCKED_BY_CLIENT);
  }

  void OnRedirectChecked(net::RedirectInfo redirect_info,
                         network::mojom::URLResponseHeadPtr head,
                         CheckResult result) {
    MoltShieldService::Get()->RecordCheck(result, request_type_);
    if (result.should_block) {
      RecordBlockedForFrame(
          content::RenderFrameHost::FromID(factory_->frame_id_));
      // Dropping the target loader cancels the network request.
      target_loader_.reset();
      client_receiver_.reset();
      CompleteWithError(net::ERR_BLOCKED_BY_CLIENT);
      return;
    }
    target_client_->OnReceiveRedirect(redirect_info, std::move(head));
    client_receiver_.Resume();
  }

  void StartTarget() {
    if (!factory_->target_factory_.is_bound()) {
      CompleteWithError(net::ERR_FAILED);
      return;
    }
    factory_->target_factory_->CreateLoaderAndStart(
        target_loader_.BindNewPipeAndPassReceiver(), request_id_, options_,
        request_, client_receiver_.BindNewPipeAndPassRemote(),
        traffic_annotation_);
    client_receiver_.set_disconnect_handler(base::BindOnce(
        &InProgressRequest::OnNetworkDisconnect, base::Unretained(this)));
    if (pending_priority_) {
      target_loader_->SetPriority(pending_priority_->first,
                                  pending_priority_->second);
    }
  }

  // Answers a blocked `$redirect=` request with the surrogate resource
  // (e.g. a no-op google-analytics.js) so pages that call into the tracker
  // don't break. Only for no-cors requests: the response bypasses the
  // network service's CORS checks.
  bool ServeSurrogate(const std::string& data_url) {
    std::string mime_type;
    std::string charset;
    std::string body;
    if (!net::DataURL::Parse(GURL(data_url), &mime_type, &charset, &body) ||
        body.size() > kMaxSurrogateBytes) {
      return false;
    }
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    // body.size() <= kMaxSurrogateBytes, so the cast is lossless.
    const uint32_t capacity =
        body.empty() ? 1u : static_cast<uint32_t>(body.size());
    if (mojo::CreateDataPipe(capacity, producer, consumer) != MOJO_RESULT_OK) {
      return false;
    }
    if (!body.empty() &&
        producer->WriteAllData(base::as_byte_span(body)) != MOJO_RESULT_OK) {
      return false;
    }
    producer.reset();

    auto head = network::mojom::URLResponseHead::New();
    head->mime_type = mime_type;
    head->charset = charset;
    head->content_length = static_cast<int64_t>(body.size());
    head->headers =
        net::HttpResponseHeaders::Builder(net::HttpVersion(1, 1), "200 OK")
            .AddHeader("Content-Type", mime_type)
            .AddHeader("Cache-Control", "no-store")
            .Build();
    target_client_->OnReceiveResponse(std::move(head), std::move(consumer),
                                      std::nullopt);
    network::URLLoaderCompletionStatus status(net::OK);
    status.decoded_body_length = static_cast<int64_t>(body.size());
    status.encoded_data_length = 0;
    completed_ = true;
    target_client_->OnComplete(status);
    return true;
  }

  void CompleteWithError(int net_error) {
    if (!completed_) {
      completed_ = true;
      target_client_->OnComplete(network::URLLoaderCompletionStatus(net_error));
    }
  }

  void OnNetworkDisconnect() {
    // The network service closes its end after OnComplete; a disconnect
    // before that is a failure the renderer must hear about.
    CompleteWithError(net::ERR_ABORTED);
  }

  void OnRendererDisconnect() {
    // Destroys `this` (and with it the network request).
    factory_->RemoveRequest(this);
  }

  const raw_ptr<MoltShieldProxyingURLLoaderFactory> factory_;
  const int32_t request_id_;
  const uint32_t options_;
  const network::ResourceRequest request_;
  const std::string request_type_;
  const net::MutableNetworkTrafficAnnotationTag traffic_annotation_;
  bool completed_ = false;
  std::optional<std::pair<net::RequestPriority, int32_t>> pending_priority_;

  // Renderer side.
  mojo::Receiver<network::mojom::URLLoader> loader_receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> target_client_;
  // Network side.
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};
  mojo::Remote<network::mojom::URLLoader> target_loader_;

  base::WeakPtrFactory<InProgressRequest> weak_factory_{this};
};

// ---------------------------------------------------------------------------
// MoltShieldProxyingURLLoaderFactory

// static
void MoltShieldProxyingURLLoaderFactory::MaybeProxy(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    content::ContentBrowserClient::URLLoaderFactoryType type,
    const url::Origin& request_initiator,
    const net::IsolationInfo& isolation_info,
    network::URLLoaderFactoryBuilder& factory_builder) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!browser_context || !ShouldProxy(type)) {
    return;
  }
  MoltShieldService* service = MoltShieldService::Get();
  service->RegisterProfile(Profile::FromBrowserContext(browser_context));
  if (!service->IsEnabled()) {
    return;
  }
  // chrome://, extension and other non-web initiators: nothing to filter
  // (and WebUI must never be affected).
  if (!request_initiator.opaque() &&
      request_initiator.scheme() != url::kHttpScheme &&
      request_initiator.scheme() != url::kHttpsScheme) {
    return;
  }
  std::optional<url::Origin> top_frame_origin =
      isolation_info.top_frame_origin();
  if (top_frame_origin &&
      !AreShieldsUpForSite(browser_context, top_frame_origin->GetURL())) {
    return;
  }

  ProxySet* set = ProxySet::Get(browser_context);
  set->Add(std::make_unique<MoltShieldProxyingURLLoaderFactory>(
      browser_context,
      frame ? frame->GetGlobalId() : content::GlobalRenderFrameHostId(),
      std::move(top_frame_origin), factory_builder,
      base::BindOnce(&ProxySet::Remove, set->GetWeakPtr())));
}

MoltShieldProxyingURLLoaderFactory::MoltShieldProxyingURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::GlobalRenderFrameHostId frame_id,
    std::optional<url::Origin> top_frame_origin,
    network::URLLoaderFactoryBuilder& factory_builder,
    DisconnectCallback on_disconnect)
    : browser_context_(browser_context),
      frame_id_(frame_id),
      top_frame_origin_(std::move(top_frame_origin)),
      on_disconnect_(std::move(on_disconnect)) {
  auto [loader_receiver, target_factory] = factory_builder.Append();
  target_factory_.Bind(std::move(target_factory));
  target_factory_.set_disconnect_handler(
      base::BindOnce(&MoltShieldProxyingURLLoaderFactory::OnTargetFactoryError,
                     base::Unretained(this)));
  proxy_receivers_.Add(this, std::move(loader_receiver));
  proxy_receivers_.set_disconnect_handler(base::BindRepeating(
      &MoltShieldProxyingURLLoaderFactory::OnProxyBindingError,
      base::Unretained(this)));
}

MoltShieldProxyingURLLoaderFactory::~MoltShieldProxyingURLLoaderFactory() =
    default;

void MoltShieldProxyingURLLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto in_progress = std::make_unique<InProgressRequest>(
      this, std::move(loader_receiver), request_id, options, request,
      std::move(client), traffic_annotation);
  InProgressRequest* raw = in_progress.get();
  requests_.insert(std::move(in_progress));
  raw->Start();
}

void MoltShieldProxyingURLLoaderFactory::Clone(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> loader_receiver) {
  proxy_receivers_.Add(this, std::move(loader_receiver));
}

void MoltShieldProxyingURLLoaderFactory::OnTargetFactoryError() {
  target_factory_.reset();
  proxy_receivers_.Clear();
  MaybeDestroySelf();
}

void MoltShieldProxyingURLLoaderFactory::OnProxyBindingError() {
  if (proxy_receivers_.empty()) {
    target_factory_.reset();
  }
  MaybeDestroySelf();
}

void MoltShieldProxyingURLLoaderFactory::RemoveRequest(
    InProgressRequest* request) {
  auto it = requests_.find(request);
  if (it != requests_.end()) {
    requests_.erase(it);
  }
  MaybeDestroySelf();
}

void MoltShieldProxyingURLLoaderFactory::MaybeDestroySelf() {
  // Stay alive while any request is in flight, even if every factory pipe
  // has closed.
  if (target_factory_.is_bound() || !requests_.empty()) {
    return;
  }
  // Deletes `this`.
  std::move(on_disconnect_).Run(this);
}

}  // namespace molt_ai::shield
