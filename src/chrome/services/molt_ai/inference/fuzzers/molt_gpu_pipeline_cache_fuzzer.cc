// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Vulkan pipeline cache header check (engine/gpu_pipeline_cache.h;
// docs/design/SANDBOXED_INFERENCE.md section 11.12). The service runs it on
// a file the browser hands over (what the service sent earlier, from disk)
// before the GPU driver parses it; the browser runs it on what a possibly
// compromised service sends before storing it.
//
// Checks: it reads only the bytes it is given (ASan), and its verdict is
// exactly an independent reading of VkPipelineCacheHeaderVersionOne: kOk
// only for 32..max_bytes bytes with a header size within the data, version
// one, and the device's vendor id, device id and pipelineCacheUUID (never
// for an all-zero UUID). Half the inputs check against the device their
// own header names, so kOk is reached.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/gpu_pipeline_cache.h"

namespace {

using molt_ai::PipelineCacheCheck;
using molt_ai::PipelineCacheDevice;

uint32_t Le32(std::string_view d, size_t at) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<uint8_t>(d[at + i])) << (8 * i);
  }
  return v;
}

PipelineCacheCheck Expected(std::string_view d,
                            const PipelineCacheDevice& dev,
                            size_t max_bytes) {
  bool any = false;
  for (uint8_t b : dev.uuid) any = any || b;
  if (!any) return PipelineCacheCheck::kNoDeviceUuid;
  if (d.empty()) return PipelineCacheCheck::kEmpty;
  if (d.size() > max_bytes) return PipelineCacheCheck::kTooLarge;
  if (d.size() < 32) return PipelineCacheCheck::kTruncated;
  const uint32_t header = Le32(d, 0);
  if (header < 32 || header > d.size()) {
    return PipelineCacheCheck::kBadHeaderSize;
  }
  if (Le32(d, 4) != 1) return PipelineCacheCheck::kBadHeaderVersion;
  if (Le32(d, 8) != dev.vendor_id) return PipelineCacheCheck::kWrongVendor;
  if (Le32(d, 12) != dev.device_id) return PipelineCacheCheck::kWrongDevice;
  for (size_t i = 0; i < 16; ++i) {
    if (static_cast<uint8_t>(d[16 + i]) != dev.uuid[i]) {
      return PipelineCacheCheck::kWrongUuid;
    }
  }
  return PipelineCacheCheck::kOk;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider fdp(data, size);
  const bool own_header = fdp.ConsumeBool();
  // Small caps too, so kTooLarge is reached.
  const size_t max_bytes = fdp.ConsumeBool()
                               ? molt_ai::kMaxPipelineCacheBytes
                               : fdp.ConsumeIntegralInRange<size_t>(0, 512);
  PipelineCacheDevice device;
  device.vendor_id = fdp.ConsumeIntegral<uint32_t>();
  device.device_id = fdp.ConsumeIntegral<uint32_t>();
  for (uint8_t& b : device.uuid) b = fdp.ConsumeIntegral<uint8_t>();
  const std::string cache = fdp.ConsumeRemainingBytesAsString();
  if (own_header && cache.size() >= 32) {
    device.vendor_id = Le32(cache, 8);
    device.device_id = Le32(cache, 12);
    for (size_t i = 0; i < 16; ++i) {
      device.uuid[i] = static_cast<uint8_t>(cache[16 + i]);
    }
  }
  // An exact-size heap copy, so a read past the end is caught.
  const std::string exact(cache);
  const PipelineCacheCheck got =
      molt_ai::CheckPipelineCacheHeader(exact, device, max_bytes);
  CHECK(got == Expected(exact, device, max_bytes));
  CHECK(molt_ai::PipelineCacheCheckName(got) != std::string("unknown"));
  return 0;
}
