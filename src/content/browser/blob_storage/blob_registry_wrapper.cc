// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/blob_storage/blob_registry_wrapper.h"

#include "base/functional/bind.h"
#include "content/browser/blob_storage/chrome_blob_storage_context.h"
#include "content/browser/security/cpsp/child_process_security_policy_impl.h"
#include "content/public/browser/browser_thread.h"
#include "storage/browser/blob/blob_registry_impl.h"
#include "storage/browser/blob/blob_storage_context.h"
#include "storage/browser/blob/molt_blob_budget_policy.h"

namespace content {

namespace {

class BindingDelegate : public storage::BlobRegistryImpl::Delegate {
 public:
  BindingDelegate(ChildProcessSecurityPolicyImpl::Handle security_policy_handle,
                  ChildProcessId process_id)
      : security_policy_handle_(std::move(security_policy_handle)),
        molt_budget_key_(process_id.is_null()
                             ? storage::kMoltBlobBrowserKey
                             : static_cast<uint64_t>(
                                   process_id.GetUnsafeValue())) {}
  ~BindingDelegate() override = default;

  // MoltBrowser: the renderer process's own budget key: process ids start
  // at 1 and are never reused (storage/browser/blob/
  // molt_blob_budget_policy.h).
  uint64_t MoltBudgetKey() override { return molt_budget_key_; }

  bool CanReadFile(const base::FilePath& file) override {
    return security_policy_handle_.CanReadFile(file);
  }
  bool CanAccessDataForOrigin(const url::Origin& origin) override {
    return security_policy_handle_.CanAccessDataForOrigin(origin);
  }

 private:
  ChildProcessSecurityPolicyImpl::Handle security_policy_handle_;
  const uint64_t molt_budget_key_;
};

}  // namespace

// static
scoped_refptr<BlobRegistryWrapper> BlobRegistryWrapper::Create(
    scoped_refptr<ChromeBlobStorageContext> blob_storage_context) {
  scoped_refptr<BlobRegistryWrapper> result(new BlobRegistryWrapper());
  GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&BlobRegistryWrapper::InitializeOnIOThread,
                                result, std::move(blob_storage_context)));
  return result;
}

BlobRegistryWrapper::BlobRegistryWrapper() = default;

void BlobRegistryWrapper::Bind(
    ChildProcessId process_id,
    mojo::PendingReceiver<blink::mojom::BlobRegistry> receiver) {
  CHECK_CURRENTLY_ON(BrowserThread::IO, base::NotFatalUntil::M159);
  blob_registry_->Bind(
      std::move(receiver),
      std::make_unique<BindingDelegate>(
          ChildProcessSecurityPolicyImpl::GetInstance()->CreateHandle(
              process_id),
          process_id));
}

BlobRegistryWrapper::~BlobRegistryWrapper() = default;

void BlobRegistryWrapper::InitializeOnIOThread(
    scoped_refptr<ChromeBlobStorageContext> blob_storage_context) {
  CHECK_CURRENTLY_ON(BrowserThread::IO, base::NotFatalUntil::M159);
  blob_registry_ = std::make_unique<storage::BlobRegistryImpl>(
      blob_storage_context->context()->AsWeakPtr());
}

}  // namespace content
