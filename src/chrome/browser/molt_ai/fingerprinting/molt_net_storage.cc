// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage.h"

#include <cstddef>

#include "base/time/time.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"

namespace molt_ai::fingerprinting {

std::optional<storage::QuotaSettings> MoltNetQuotaSettings(RendererMode mode) {
  if (mode != RendererMode::kUniform) {
    return std::nullopt;
  }
  storage::QuotaSettings settings;
  settings.pool_size = kMoltNetQuotaPoolBytes;
  settings.per_storage_key_quota = kMoltNetQuotaPerStorageKeyBytes;
  settings.session_only_per_storage_key_quota =
      kMoltNetQuotaPerStorageKeyBytes;
  // Each site's own share, whatever the others store; all of them together
  // the pool, beyond it what the machine can spare above the reserve; and
  // no eviction (storage/browser/quota/molt_storage_budget_policy.h).
  settings.molt_per_site_quota = kMoltNetQuotaPerSiteBytes;
  settings.molt_memory_reserve = kMoltNetStorageMemoryReserveBytes;
  // MoltNet's partitions are in memory: no disk space to keep free.
  settings.should_remain_available = 0;
  settings.must_remain_available = 0;
  // Never asked again: the settings don't change.
  settings.refresh_interval = base::TimeDelta::Max();
  return settings;
}

std::optional<storage::BlobStorageLimits> MoltNetBlobStorageLimits(
    RendererMode mode) {
  if (mode != RendererMode::kUniform) {
    return std::nullopt;
  }
  // Chromium's transport sizes (the same on every machine) ...
  storage::BlobStorageLimits limits;
  // ... with a fixed memory limit and no disk.
  limits.max_blob_in_memory_space =
      static_cast<size_t>(kMoltNetBlobMemoryBytes);
  limits.desired_max_disk_space = 0;
  limits.effective_max_disk_space = 0;
  return limits;
}

}  // namespace molt_ai::fingerprinting
