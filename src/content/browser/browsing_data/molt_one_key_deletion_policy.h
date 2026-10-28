// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what a deletion of one storage key's data reads, where
// storage is kept per site (MoltNet's partition).
//
// StoragePartitionImpl's DataDeletionHelper finds what to delete by listing
// it first, at Chromium 156: localStorage, unless the deletion names its
// storage key itself rather than through a filter (ClearLocalStorage());
// sessionStorage always (ClearSessionStorage()); and quota-managed data
// (IndexedDB, Cache Storage, the file systems, service workers) by listing
// every bucket modified in the deletion's time range
// (ClearQuotaManagedData()). A filter then picks what goes. Clear-Site-Data
// ("storage") deletes one storage key's data through such a filter
// (BrowsingDataFilterBuilder::SetStorageKey(), which makes its storage key
// filter match that key and nothing else), and a site sends it at will, on
// any response of its own: each one listed every site's areas, in the
// storage service and over Mojo, and every site's buckets, so its cost
// grew with what all sites store, and a site could spend that on every
// other site's turn.
//
// Here, where storage is kept per site, a deletion that names one storage
// key (the key itself, or a deleting filter with its storage key set) reads
// that key's usage only: its DOM storage areas, in the turn of its site in
// the storage service (DOMStorageContextWrapper::
// MoltGetLocalStorageUsageForStorageKey(),
// MoltGetSessionStorageUsageForStorageKey()), and its buckets
// (QuotaManagerImpl::GetBucketsForStorageKey(), for a deletion over all
// time, as Clear-Site-Data's is; a deletion over a time range keeps
// Chromium's listing, which reads the modification times). What is deleted
// doesn't change: the same filter decides, on a list that holds everything
// the filter can match. The callers that want everything (the browser's
// site settings and site data dialogs, clearing browsing data, counting it)
// are the browser's own and read the whole listing as before.
//
// Std-only, so molt_ai_unittests tests it.

#ifndef CONTENT_BROWSER_BROWSING_DATA_MOLT_ONE_KEY_DELETION_POLICY_H_
#define CONTENT_BROWSER_BROWSING_DATA_MOLT_ONE_KEY_DELETION_POLICY_H_

namespace content {

// What a deletion reads to find what it deletes.
enum class MoltDeletionReads {
  // Every storage key's usage (Chromium's listing).
  kWholePartition,
  // The usage of the one storage key the deletion names.
  kOneStorageKey,
};

// Whether a deletion names one storage key: the key itself
// (`storage_key_given`: a non-opaque key passed to the deletion), or a
// filter in delete mode with its storage key set (`deleting_filter`,
// `filter_has_storage_key`), whose storage key filter matches that key only.
constexpr bool MoltDeletionNamesOneStorageKey(bool storage_key_given,
                                              bool deleting_filter,
                                              bool filter_has_storage_key) {
  return storage_key_given || (deleting_filter && filter_has_storage_key);
}

// What a deletion of DOM storage (localStorage, sessionStorage) reads, in a
// partition that `keeps_per_site` or not.
constexpr MoltDeletionReads MoltDomStorageDeletionReads(bool keeps_per_site,
                                                        bool names_one_key) {
  return keeps_per_site && names_one_key ? MoltDeletionReads::kOneStorageKey
                                         : MoltDeletionReads::kWholePartition;
}

// What a deletion of quota-managed data reads; one key's buckets only over
// all time (`all_time`: no begin, no end), since the bucket list for one key
// carries no modification times to select by.
constexpr MoltDeletionReads MoltQuotaDeletionReads(bool keeps_per_site,
                                                   bool names_one_key,
                                                   bool all_time) {
  return keeps_per_site && names_one_key && all_time
             ? MoltDeletionReads::kOneStorageKey
             : MoltDeletionReads::kWholePartition;
}

}  // namespace content

#endif  // CONTENT_BROWSER_BROWSING_DATA_MOLT_ONE_KEY_DELETION_POLICY_H_
