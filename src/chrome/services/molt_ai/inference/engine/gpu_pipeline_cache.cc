// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/gpu_pipeline_cache.h"

namespace molt_ai {

namespace {

// The little-endian uint32 at |offset| (the caller checked the length).
uint32_t ReadLe32(std::string_view data, size_t offset) {
  return static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) |
         static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 2])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(data[offset + 3])) << 24;
}

}  // namespace

const char* PipelineCacheCheckName(PipelineCacheCheck check) {
  switch (check) {
    case PipelineCacheCheck::kOk:
      return "ok";
    case PipelineCacheCheck::kEmpty:
      return "empty";
    case PipelineCacheCheck::kTruncated:
      return "truncated";
    case PipelineCacheCheck::kTooLarge:
      return "too large";
    case PipelineCacheCheck::kBadHeaderSize:
      return "bad header size";
    case PipelineCacheCheck::kBadHeaderVersion:
      return "bad header version";
    case PipelineCacheCheck::kWrongVendor:
      return "wrong vendor";
    case PipelineCacheCheck::kWrongDevice:
      return "wrong device";
    case PipelineCacheCheck::kWrongUuid:
      return "wrong UUID";
    case PipelineCacheCheck::kNoDeviceUuid:
      return "no device UUID";
  }
  return "unknown";
}

PipelineCacheCheck CheckPipelineCacheHeader(std::string_view data,
                                            const PipelineCacheDevice& device,
                                            size_t max_bytes) {
  bool has_uuid = false;
  for (uint8_t byte : device.uuid) {
    has_uuid = has_uuid || byte != 0;
  }
  if (!has_uuid) {
    return PipelineCacheCheck::kNoDeviceUuid;
  }
  if (data.empty()) {
    return PipelineCacheCheck::kEmpty;
  }
  if (data.size() > max_bytes) {
    return PipelineCacheCheck::kTooLarge;
  }
  if (data.size() < kPipelineCacheHeaderBytes) {
    return PipelineCacheCheck::kTruncated;
  }
  const uint32_t header_size = ReadLe32(data, 0);
  if (header_size < kPipelineCacheHeaderBytes || header_size > data.size()) {
    return PipelineCacheCheck::kBadHeaderSize;
  }
  if (ReadLe32(data, 4) != kPipelineCacheHeaderVersionOne) {
    return PipelineCacheCheck::kBadHeaderVersion;
  }
  if (ReadLe32(data, 8) != device.vendor_id) {
    return PipelineCacheCheck::kWrongVendor;
  }
  if (ReadLe32(data, 12) != device.device_id) {
    return PipelineCacheCheck::kWrongDevice;
  }
  for (size_t i = 0; i < kPipelineCacheUuidBytes; ++i) {
    if (static_cast<uint8_t>(data[16 + i]) != device.uuid[i]) {
      return PipelineCacheCheck::kWrongUuid;
    }
  }
  return PipelineCacheCheck::kOk;
}

}  // namespace molt_ai
