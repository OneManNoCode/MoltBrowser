// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_nonce_storage.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/unguessable_token.h"
#include "chrome/browser/molt_ai/tor/molt_net_nonce_storage_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/dom_storage_context.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page.h"
#include "content/public/browser/page_user_data.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "net/base/schemeful_site.h"
#include "storage/browser/quota/quota_manager.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "url/origin.h"

namespace molt_ai::tor {

namespace {

const char kMoltNetNonceRegistryKey[] = "MoltNetNonceRegistry";

// A MoltNet profile's nonce groups (molt_net_nonce_storage_policy.h): which
// are open for each top-level site, and what each one's page's frames used.
class MoltNetNonceRegistry : public base::SupportsUserData::Data {
 public:
  explicit MoltNetNonceRegistry(content::BrowserContext* browser_context)
      : browser_context_(browser_context) {}
  MoltNetNonceRegistry(const MoltNetNonceRegistry&) = delete;
  MoltNetNonceRegistry& operator=(const MoltNetNonceRegistry&) = delete;
  ~MoltNetNonceRegistry() override = default;

  static MoltNetNonceRegistry* For(content::BrowserContext* browser_context) {
    auto* registry = static_cast<MoltNetNonceRegistry*>(
        browser_context->GetUserData(kMoltNetNonceRegistryKey));
    if (!registry) {
      auto created = std::make_unique<MoltNetNonceRegistry>(browser_context);
      registry = created.get();
      browser_context->SetUserData(kMoltNetNonceRegistryKey,
                                   std::move(created));
    }
    return registry;
  }

  // A frame of a page of top-level site `site` uses `storage_key` (with
  // `nonce`), in the storage partition of `config`.
  void Noted(const std::string& site,
             const base::UnguessableToken& nonce,
             const blink::StorageKey& storage_key,
             const content::StoragePartitionConfig& config) {
    auto [record, inserted] = records_.try_emplace(nonce, config);
    record->second.keys.insert(storage_key);
    if (!inserted) {
      return;
    }
    // Past the site's bound, its oldest open group is closed.
    if (std::optional<base::UnguessableToken> closed =
            groups_.Added(site, nonce)) {
      if (auto it = records_.find(*closed); it != records_.end()) {
        Drop(*closed, it->second, /*closed=*/true);
      }
    }
  }

  // The page that used `nonce` is gone: its group goes.
  void Gone(const base::UnguessableToken& nonce) {
    auto it = records_.find(nonce);
    if (it == records_.end()) {
      return;
    }
    Drop(nonce, it->second, /*closed=*/false);
    records_.erase(it);
    groups_.Gone(nonce);
  }

  base::WeakPtr<MoltNetNonceRegistry> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  struct Record {
    explicit Record(const content::StoragePartitionConfig& config)
        : config(config) {}
    content::StoragePartitionConfig config;
    std::set<blink::StorageKey> keys;
  };

  // What the storage partition holds for `nonce` goes: its DOM storage (in
  // the storage service, by nonce) and its storage keys' buckets.
  void Drop(const base::UnguessableToken& nonce,
            const Record& record,
            bool closed) {
    content::StoragePartition* partition =
        browser_context_->GetStoragePartition(record.config,
                                              /*can_create=*/false);
    if (!partition) {
      return;
    }
    if (content::DOMStorageContext* dom_storage =
            partition->GetDOMStorageContext()) {
      dom_storage->MoltDropNonce(nonce, closed);
    }
    if (storage::QuotaManager* quota_manager = partition->GetQuotaManager()) {
      content::GetIOThreadTaskRunner({})->PostTask(
          FROM_HERE,
          base::BindOnce(&storage::QuotaManagerImpl::MoltDropNonce,
                         base::WrapRefCounted(quota_manager), nonce,
                         std::vector<blink::StorageKey>(record.keys.begin(),
                                                        record.keys.end()),
                         closed));
    }
  }

  const raw_ptr<content::BrowserContext> browser_context_;
  MoltNetNonceGroups<base::UnguessableToken> groups_;
  std::map<base::UnguessableToken, Record> records_;
  base::WeakPtrFactory<MoltNetNonceRegistry> weak_factory_{this};
};

// The nonces a page's frames used; when the page goes, their groups go.
class MoltNetNoncePage : public content::PageUserData<MoltNetNoncePage> {
 public:
  MoltNetNoncePage(const MoltNetNoncePage&) = delete;
  MoltNetNoncePage& operator=(const MoltNetNoncePage&) = delete;
  ~MoltNetNoncePage() override {
    if (!registry_) {
      return;
    }
    for (const base::UnguessableToken& nonce : nonces_) {
      registry_->Gone(nonce);
    }
  }

  void Noted(const base::UnguessableToken& nonce) { nonces_.insert(nonce); }

 private:
  friend class content::PageUserData<MoltNetNoncePage>;
  MoltNetNoncePage(content::Page& page,
                   base::WeakPtr<MoltNetNonceRegistry> registry)
      : content::PageUserData<MoltNetNoncePage>(page),
        registry_(std::move(registry)) {}

  base::WeakPtr<MoltNetNonceRegistry> registry_;
  std::set<base::UnguessableToken> nonces_;

  PAGE_USER_DATA_KEY_DECL();
};

PAGE_USER_DATA_KEY_IMPL(MoltNetNoncePage);

}  // namespace

// static
void MoltNetNonceStorageTabHelper::MaybeCreateForWebContents(
    content::WebContents* web_contents) {
  if (IsMoltNetProfile(
          Profile::FromBrowserContext(web_contents->GetBrowserContext()))) {
    CreateForWebContents(web_contents);
  }
}

MoltNetNonceStorageTabHelper::MoltNetNonceStorageTabHelper(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<MoltNetNonceStorageTabHelper>(
          *web_contents) {}

MoltNetNonceStorageTabHelper::~MoltNetNonceStorageTabHelper() = default;

void MoltNetNonceStorageTabHelper::RenderFrameCreated(
    content::RenderFrameHost* render_frame_host) {
  Note(render_frame_host);
}

void MoltNetNonceStorageTabHelper::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->HasCommitted() ||
      navigation_handle->IsSameDocument()) {
    return;
  }
  Note(navigation_handle->GetRenderFrameHost());
}

void MoltNetNonceStorageTabHelper::Note(
    content::RenderFrameHost* render_frame_host) {
  if (!render_frame_host) {
    return;
  }
  const blink::StorageKey& storage_key = render_frame_host->GetStorageKey();
  if (!storage_key.nonce().has_value()) {
    return;
  }
  MoltNetNonceRegistry* registry =
      MoltNetNonceRegistry::For(render_frame_host->GetBrowserContext());
  content::Page& page = render_frame_host->GetPage();
  MoltNetNoncePage::CreateForPage(page, registry->GetWeakPtr());
  MoltNetNoncePage* noted = MoltNetNoncePage::GetForPage(page);
  if (!noted) {
    return;
  }
  // The top-level site the page is shown under (an outer page's, for a
  // fenced frame's).
  const std::string site =
      net::SchemefulSite(
          render_frame_host->GetOutermostMainFrame()->GetLastCommittedOrigin())
          .Serialize();
  noted->Noted(*storage_key.nonce());
  registry->Noted(site, *storage_key.nonce(), storage_key,
                  render_frame_host->GetStoragePartition()->GetConfig());
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(MoltNetNonceStorageTabHelper);

}  // namespace molt_ai::tor
