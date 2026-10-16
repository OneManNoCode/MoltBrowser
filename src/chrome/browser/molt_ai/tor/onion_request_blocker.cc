// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_request_blocker.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/onion_location.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace molt_ai {
namespace tor {

namespace {

using FactoryType = content::ContentBrowserClient::URLLoaderFactoryType;

// Owns one profile's proxies so they die with it.
class OnionProxySet : public base::SupportsUserData::Data {
 public:
  static OnionProxySet* Get(content::BrowserContext* context) {
    auto* self = static_cast<OnionProxySet*>(context->GetUserData(&kKey));
    if (!self) {
      auto owned = std::make_unique<OnionProxySet>();
      self = owned.get();
      context->SetUserData(&kKey, std::move(owned));
    }
    return self;
  }

  void Add(std::unique_ptr<OnionBlockingURLLoaderFactory> proxy) {
    proxies_.insert(std::move(proxy));
  }

  void Remove(OnionBlockingURLLoaderFactory* proxy) {
    auto it = proxies_.find(proxy);
    if (it != proxies_.end()) {
      proxies_.erase(it);
    }
  }

  base::WeakPtr<OnionProxySet> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  static const int kKey = 0;
  std::set<std::unique_ptr<OnionBlockingURLLoaderFactory>,
           base::UniquePtrComparator>
      proxies_;
  base::WeakPtrFactory<OnionProxySet> weak_factory_{this};
};

const int OnionProxySet::kKey;

// Everything but navigations, which OnionNavigationThrottle stops before
// their request starts (and on every redirect). Downloads and DevTools
// fetches are covered too: nothing outside MoltNet may reach an onion name.
bool ShouldProxy(FactoryType type) {
  switch (type) {
    case FactoryType::kNavigation:
      return false;
    case FactoryType::kDownload:
    case FactoryType::kDocumentSubResource:
    case FactoryType::kWorkerMainResource:
    case FactoryType::kWorkerSubResource:
    case FactoryType::kServiceWorkerScript:
    case FactoryType::kServiceWorkerSubResource:
    case FactoryType::kPrefetch:
    case FactoryType::kDevTools:
    case FactoryType::kEarlyHints:
      return true;
  }
  return true;  // A new factory type is blocked until reviewed.
}

// Unknown or missing profiles are treated as non-MoltNet: blocking is the
// safe default.
bool IsMoltNetContext(content::BrowserContext* browser_context) {
  return browser_context &&
         IsMoltNetProfile(Profile::FromBrowserContext(browser_context));
}

}  // namespace

// ---------------------------------------------------------------------------
// InProgressRequest: proxies one request's URLLoader (renderer -> network)
// and URLLoaderClient (network -> renderer), so a redirect to an onion host
// is seen before the renderer can follow it.

class OnionBlockingURLLoaderFactory::InProgressRequest
    : public network::mojom::URLLoader,
      public network::mojom::URLLoaderClient {
 public:
  InProgressRequest(
      OnionBlockingURLLoaderFactory* factory,
      mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client)
      : factory_(factory),
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

  void Start(int32_t request_id,
             uint32_t options,
             const network::ResourceRequest& request,
             const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
    if (IsOnionRequestURL(request.url)) {
      // Never reaches the network service, so never reaches a proxy.
      CompleteWithError(net::ERR_BLOCKED_BY_CLIENT);
      return;
    }
    if (!factory_->target_factory_.is_bound()) {
      CompleteWithError(net::ERR_FAILED);
      return;
    }
    factory_->target_factory_->CreateLoaderAndStart(
        target_loader_.BindNewPipeAndPassReceiver(), request_id, options,
        request, client_receiver_.BindNewPipeAndPassRemote(),
        traffic_annotation);
    client_receiver_.set_disconnect_handler(base::BindOnce(
        &InProgressRequest::OnNetworkDisconnect, base::Unretained(this)));
  }

  // network::mojom::URLLoader (from the renderer):
  void FollowRedirect(
      const std::vector<std::string>& removed_headers,
      const net::HttpRequestHeaders& modified_headers,
      const net::HttpRequestHeaders& modified_cors_exempt_headers,
      const std::optional<GURL>& new_url) override {
    // A throttle may substitute the redirect target.
    if (new_url && IsOnionRequestURL(*new_url)) {
      Block();
      return;
    }
    if (target_loader_.is_bound()) {
      target_loader_->FollowRedirect(removed_headers, modified_headers,
                                     modified_cors_exempt_headers, new_url);
    }
  }

  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {
    if (target_loader_.is_bound()) {
      target_loader_->SetPriority(priority, intra_priority_value);
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
    // The network service only follows a redirect once the client says so,
    // so cancelling here means the onion name is never requested.
    if (IsOnionRequestURL(redirect_info.new_url)) {
      Block();
      return;
    }
    target_client_->OnReceiveRedirect(redirect_info, std::move(head));
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
  // Cancels the network request (if any) and fails the renderer's.
  void Block() {
    target_loader_.reset();
    client_receiver_.reset();
    CompleteWithError(net::ERR_BLOCKED_BY_CLIENT);
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
    // Destroys |this| (and with it the network request).
    factory_->RemoveRequest(this);
  }

  const raw_ptr<OnionBlockingURLLoaderFactory> factory_;
  bool completed_ = false;

  // Renderer side.
  mojo::Receiver<network::mojom::URLLoader> loader_receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> target_client_;
  // Network side.
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};
  mojo::Remote<network::mojom::URLLoader> target_loader_;
};

// ---------------------------------------------------------------------------
// OnionBlockingURLLoaderFactory

// static
void OnionBlockingURLLoaderFactory::MaybeProxy(
    content::BrowserContext* browser_context,
    content::ContentBrowserClient::URLLoaderFactoryType type,
    network::URLLoaderFactoryBuilder& factory_builder) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // MoltNet reaches onion services through Tor. (Content always passes the
  // context; without one there is nothing to own the proxy.)
  if (!browser_context || IsMoltNetContext(browser_context) ||
      !ShouldProxy(type)) {
    return;
  }
  OnionProxySet* set = OnionProxySet::Get(browser_context);
  set->Add(std::make_unique<OnionBlockingURLLoaderFactory>(
      factory_builder,
      base::BindOnce(&OnionProxySet::Remove, set->GetWeakPtr())));
}

OnionBlockingURLLoaderFactory::OnionBlockingURLLoaderFactory(
    network::URLLoaderFactoryBuilder& factory_builder,
    DisconnectCallback on_disconnect)
    : on_disconnect_(std::move(on_disconnect)) {
  auto [loader_receiver, target_factory] = factory_builder.Append();
  target_factory_.Bind(std::move(target_factory));
  target_factory_.set_disconnect_handler(
      base::BindOnce(&OnionBlockingURLLoaderFactory::OnTargetFactoryError,
                     base::Unretained(this)));
  proxy_receivers_.Add(this, std::move(loader_receiver));
  proxy_receivers_.set_disconnect_handler(
      base::BindRepeating(&OnionBlockingURLLoaderFactory::OnProxyBindingError,
                          base::Unretained(this)));
}

OnionBlockingURLLoaderFactory::~OnionBlockingURLLoaderFactory() = default;

void OnionBlockingURLLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto in_progress = std::make_unique<InProgressRequest>(
      this, std::move(loader_receiver), std::move(client));
  InProgressRequest* raw = in_progress.get();
  requests_.insert(std::move(in_progress));
  raw->Start(request_id, options, request, traffic_annotation);
}

void OnionBlockingURLLoaderFactory::Clone(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> loader_receiver) {
  proxy_receivers_.Add(this, std::move(loader_receiver));
}

void OnionBlockingURLLoaderFactory::OnTargetFactoryError() {
  target_factory_.reset();
  proxy_receivers_.Clear();
  MaybeDestroySelf();
}

void OnionBlockingURLLoaderFactory::OnProxyBindingError() {
  if (proxy_receivers_.empty()) {
    target_factory_.reset();
  }
  MaybeDestroySelf();
}

void OnionBlockingURLLoaderFactory::RemoveRequest(InProgressRequest* request) {
  auto it = requests_.find(request);
  if (it != requests_.end()) {
    requests_.erase(it);
  }
  MaybeDestroySelf();
}

void OnionBlockingURLLoaderFactory::MaybeDestroySelf() {
  // Stay alive while any request is in flight, even if every factory pipe
  // has closed.
  if (target_factory_.is_bound() || !requests_.empty()) {
    return;
  }
  // Deletes |this|.
  std::move(on_disconnect_).Run(this);
}

// ---------------------------------------------------------------------------
// WebSockets

bool ShouldInterceptWebSocketForOnion(content::RenderFrameHost* frame) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return frame && !IsMoltNetContext(frame->GetBrowserContext());
}

bool IsOnionWebSocketBlocked(content::RenderFrameHost* frame,
                             const GURL& url) {
  return ShouldInterceptWebSocketForOnion(frame) && IsOnionRequestURL(url);
}

}  // namespace tor
}  // namespace molt_ai
