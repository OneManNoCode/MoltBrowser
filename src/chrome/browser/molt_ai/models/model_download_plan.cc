// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/models/model_download_plan.h"

#include <algorithm>

#include "chrome/browser/molt_ai/models/download_policy.h"

namespace molt_ai {
namespace models {

namespace {

bool IsLowerHex(std::string_view s, size_t length) {
  if (s.size() != length) {
    return false;
  }
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

bool IsPinned(const ModelFileSpec& file) {
  return !file.sha256.empty();
}

// The bytes a file counts for: exact when pinned, else the estimate.
int64_t SizeOf(const ModelFileSpec& file) {
  return std::max<int64_t>(0, file.size);
}

}  // namespace

ModelFileSpec::ModelFileSpec() = default;
ModelFileSpec::~ModelFileSpec() = default;
ModelFileSpec::ModelFileSpec(const ModelFileSpec&) = default;
ModelFileSpec& ModelFileSpec::operator=(const ModelFileSpec&) = default;
ModelFileSpec::ModelFileSpec(ModelFileSpec&&) = default;
ModelFileSpec& ModelFileSpec::operator=(ModelFileSpec&&) = default;

DownloadPlan::DownloadPlan() = default;
DownloadPlan::~DownloadPlan() = default;
DownloadPlan::DownloadPlan(const DownloadPlan&) = default;
DownloadPlan& DownloadPlan::operator=(const DownloadPlan&) = default;
DownloadPlan::DownloadPlan(DownloadPlan&&) = default;
DownloadPlan& DownloadPlan::operator=(DownloadPlan&&) = default;

std::string ModelFileUrl(const ModelFileSpec& file) {
  return ModelDownloadUrl(file.repo, file.revision, file.filename);
}

bool IsFullyPinned(const ModelFileSpec& file) {
  return IsLowerHex(file.revision, 40) && IsValidSha256Hex(file.sha256) &&
         file.size > 0;
}

int64_t ExpectedSizeForJob(const ModelFileSpec& file) {
  return IsPinned(file) ? SizeOf(file) : 0;
}

bool IsComplete(const ModelFileSpec& file, const ModelFileOnDisk& on_disk) {
  return IsCompleteModelFile(on_disk.final_exists, on_disk.final_size,
                             ExpectedSizeForJob(file));
}

std::string_view PlanErrorText(PlanError error) {
  switch (error) {
    case PlanError::kNone:
      return "";
    case PlanError::kBadUnit:
      return "This model's files aren't listed correctly, so it can't be "
             "downloaded.";
    case PlanError::kNotPinned:
    case PlanError::kBadPin:
      return "This model isn't pinned to verified files yet, so it can't be "
             "downloaded.";
  }
  return "";
}

PlanError CheckUnit(const std::vector<ModelFileSpec>& files) {
  if (files.empty() || files[0].role != ModelFileRole::kWeights) {
    return PlanError::kBadUnit;
  }
  size_t projectors = 0;
  for (size_t i = 1; i < files.size(); ++i) {
    if (files[i].role != ModelFileRole::kVisionProjector) {
      return PlanError::kBadUnit;
    }
    ++projectors;
  }
  if (projectors > 1) {
    return PlanError::kBadUnit;
  }
  if (projectors == 1) {
    // Fail closed: the projector is untrusted model input, and a model
    // that loads two files is only ever downloaded fully pinned.
    for (const ModelFileSpec& file : files) {
      if (!IsFullyPinned(file)) {
        return PlanError::kNotPinned;
      }
    }
    return PlanError::kNone;
  }
  // A text-only model: unpinned is allowed (downloaded as unverified);
  // a pin, if there is one, must be well formed.
  const ModelFileSpec& weights = files[0];
  if (IsPinned(weights) &&
      (!IsValidSha256Hex(weights.sha256) || weights.size <= 0)) {
    return PlanError::kBadPin;
  }
  return PlanError::kNone;
}

DownloadPlan PlanModelDownload(const std::vector<ModelFileSpec>& files,
                               const std::vector<ModelFileOnDisk>& on_disk) {
  DownloadPlan plan;
  plan.error = CheckUnit(files);
  if (plan.error == PlanError::kNone && on_disk.size() != files.size()) {
    plan.error = PlanError::kBadUnit;
  }
  if (plan.error != PlanError::kNone) {
    return plan;
  }
  for (size_t i = 0; i < files.size(); ++i) {
    const int64_t size = SizeOf(files[i]);
    plan.total_bytes += size;
    if (IsComplete(files[i], on_disk[i])) {
      plan.done_bytes += size;
      continue;
    }
    plan.to_fetch.push_back(i);
    const int64_t partial = std::max<int64_t>(0, on_disk[i].partial_size);
    if (partial < size) {
      plan.bytes_to_write += size - partial;
    } else if (IsPinned(files[i])) {
      // A .partial as large as a pinned file is a leftover the job
      // truncates and writes again.
      plan.bytes_to_write += size;
    }
  }
  return plan;
}

bool IsUnitDownloaded(const std::vector<ModelFileSpec>& files,
                      const std::vector<ModelFileOnDisk>& on_disk) {
  const DownloadPlan plan = PlanModelDownload(files, on_disk);
  return plan.error == PlanError::kNone && plan.to_fetch.empty();
}

}  // namespace models
}  // namespace molt_ai
