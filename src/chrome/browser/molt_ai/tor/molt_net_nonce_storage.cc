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

#include "base/containers/flat_set.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
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
#include "media/media_buildflags.h"
#include "net/base/isolation_info.h"
#include "net/base/schemeful_site.h"
#include "net/cookies/cookie_partition_key.h"
#include "net/cookies/cookie_partition_key_collection.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "storage/browser/quota/quota_manager.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"

#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
#include "content/public/browser/cdm_storage_data_model.h"
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)

namespace molt_ai::tor {

namespace {

const char kMoltNetNonceRegistryKey[] = "MoltNetNonceRegistry";

using CookiePartitionKeys = std::set<net::CookiePartitionKey>;

// Deletes the cookies of `partitions` in `partition`'s cookie store: only
// partitioned cookies, and only in those partitions (a filter that names
// partitions still matches every unpartitioned cookie without
// `partitioned_state_only`: net::CookieDeletionInfo::Matches()).
void DeleteNonceCookies(content::StoragePartition* partition,
                        const CookiePartitionKeys& partitions) {
  if (partitions.empty()) {
    return;
  }
  auto filter = network::mojom::CookieDeletionFilter::New();
  filter->partitioned_state_only = true;
  filter->cookie_partition_key_collection = net::CookiePartitionKeyCollection(
      base::flat_set<net::CookiePartitionKey>(partitions.begin(),
                                              partitions.end()));
  partition->GetCookieManagerForBrowserProcess()->DeleteCookies(
      std::move(filter), base::DoNothing());
}

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
  // `nonce`), in the storage partition of `config`; its cookies are in the
  // key's own partition (blink::StorageKey::ToCookiePartitionKey(): the
  // frame's site, as the frame's network key gives them).
  void NotedStorageKey(const std::string& site,
                       const base::UnguessableToken& nonce,
                       const blink::StorageKey& storage_key,
                       const content::StoragePartitionConfig& config) {
    Record& record = RecordFor(site, nonce, config);
    record.keys.insert(storage_key);
    if (std::optional<net::CookiePartitionKey> partition =
            storage_key.ToCookiePartitionKey()) {
      record.cookie_partitions.insert(*partition);
    }
  }

  // A navigation of a frame of such a page, under `nonce`, requests a URL of
  // `cookie_partition`'s site: its response may set cookies there.
  void NotedCookiePartition(const std::string& site,
                            const base::UnguessableToken& nonce,
                            const net::CookiePartitionKey& cookie_partition,
                            const content::StoragePartitionConfig& config) {
    RecordFor(site, nonce, config).cookie_partitions.insert(cookie_partition);
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
    // The cookie partitions of the nonce: its storage keys', and those of
    // the sites its frames' navigations requested.
    CookiePartitionKeys cookie_partitions;
  };

  // `nonce`'s record, made (and counted to `site`'s bound) on first use.
  Record& RecordFor(const std::string& site,
                    const base::UnguessableToken& nonce,
                    const content::StoragePartitionConfig& config) {
    auto [record, inserted] = records_.try_emplace(nonce, config);
    if (inserted) {
      // Past the site's bound, its oldest open group is closed.
      if (std::optional<base::UnguessableToken> closed =
              groups_.Added(site, nonce)) {
        if (auto it = records_.find(*closed); it != records_.end()) {
          Drop(*closed, it->second, /*closed=*/true);
        }
      }
    }
    return record->second;
  }

  // What the storage partition holds for `nonce` goes: its DOM storage (in
  // the storage service, by nonce), its storage keys' buckets and media-
  // license data, and its cookies; those again later if its page is gone.
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
#if BUILDFLAG(ENABLE_LIBRARY_CDMS)
    if (content::CdmStorageDataModel* cdm_storage =
            partition->GetCdmStorageDataModel()) {
      for (const blink::StorageKey& key : record.keys) {
        cdm_storage->DeleteDataForStorageKey(key, base::DoNothing());
      }
    }
#endif  // BUILDFLAG(ENABLE_LIBRARY_CDMS)
    DeleteNonceCookies(partition, record.cookie_partitions);
    if (MoltNetNonceSweepsCookiesLater(closed) &&
        !record.cookie_partitions.empty()) {
      content::GetUIThreadTaskRunner({})->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&MoltNetNonceRegistry::SweepCookies,
                         weak_factory_.GetWeakPtr(), record.config,
                         record.cookie_partitions),
          base::Seconds(kMoltNetNonceCookieSweepSeconds));
    }
  }

  // A gone page's cookie partitions, again: what one of its requests still
  // on its way set after the page went.
  void SweepCookies(const content::StoragePartitionConfig& config,
                    const CookiePartitionKeys& cookie_partitions) {
    if (content::StoragePartition* partition =
            browser_context_->GetStoragePartition(config,
                                                  /*can_create=*/false)) {
      DeleteNonceCookies(partition, cookie_partitions);
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

// The site whose bound the page of `render_frame_host` counts to: the
// top-level site it is shown under (an outer page's, for a fenced frame's);
// for a page served sandboxed, the site its origin came from
// (MoltNetNonceGroupSite()).
std::string GroupSiteOf(content::RenderFrameHost* render_frame_host,
                        const base::UnguessableToken& nonce) {
  const url::Origin& top =
      render_frame_host->GetOutermostMainFrame()->GetLastCommittedOrigin();
  const url::SchemeHostPort& precursor =
      top.GetTupleOrPrecursorTupleIfOpaque();
  return MoltNetNonceGroupSite(
      top.opaque(), net::SchemefulSite(top).Serialize(),
      top.opaque() && precursor.IsValid()
          ? net::SchemefulSite(precursor.GetURL()).Serialize()
          : std::string(),
      nonce.ToString());
}

// `page`'s record of its nonces, made on first use.
MoltNetNoncePage* NoncePageOf(content::Page& page,
                              MoltNetNonceRegistry* registry) {
  MoltNetNoncePage::CreateForPage(page, registry->GetWeakPtr());
  return MoltNetNoncePage::GetForPage(page);
}

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

void MoltNetNonceStorageTabHelper::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  NoteRequest(navigation_handle);
}

void MoltNetNonceStorageTabHelper::DidRedirectNavigation(
    content::NavigationHandle* navigation_handle) {
  NoteRequest(navigation_handle);
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
  MoltNetNoncePage* noted =
      NoncePageOf(render_frame_host->GetPage(), registry);
  if (!noted) {
    return;
  }
  noted->Noted(*storage_key.nonce());
  registry->NotedStorageKey(
      GroupSiteOf(render_frame_host, *storage_key.nonce()),
      *storage_key.nonce(), storage_key,
      render_frame_host->GetStoragePartition()->GetConfig());
}

void MoltNetNonceStorageTabHelper::NoteRequest(
    content::NavigationHandle* navigation_handle) {
  // A frame's navigation with a nonce (a credentialless iframe's, a fenced
  // frame's) requests its URL with the frame's network key for that URL:
  // the cookies its response sets are in the partition of the URL's site
  // with the nonce (net::CookiePartitionKey::FromNetworkIsolationKey()),
  // committed or not (a download, a 204, a redirect's own response).
  if (navigation_handle->IsSameDocument() ||
      !navigation_handle->GetURL().SchemeIsHTTPOrHTTPS()) {
    return;
  }
  content::RenderFrameHost* parent =
      navigation_handle->GetParentFrameOrOuterDocument();
  if (!parent) {
    return;
  }
  const std::optional<base::UnguessableToken> nonce =
      navigation_handle->GetIsolationInfo().nonce();
  if (!nonce.has_value()) {
    return;
  }
  // Noted on the page the frame belongs to, whose nonce it is.
  MoltNetNonceRegistry* registry =
      MoltNetNonceRegistry::For(parent->GetBrowserContext());
  MoltNetNoncePage* noted = NoncePageOf(parent->GetPage(), registry);
  if (!noted) {
    return;
  }
  noted->Noted(*nonce);
  registry->NotedCookiePartition(
      GroupSiteOf(parent, *nonce), *nonce,
      net::CookiePartitionKey::FromWire(
          net::SchemefulSite(navigation_handle->GetURL()),
          net::CookiePartitionKey::AncestorChainBit::kCrossSite, *nonce),
      parent->GetStoragePartition()->GetConfig());
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(MoltNetNonceStorageTabHelper);

}  // namespace molt_ai::tor
