// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/gpu_pipeline_cache.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

PipelineCacheDevice Device() {
  PipelineCacheDevice device;
  device.vendor_id = 0x1002;
  device.device_id = 0x73ff;
  for (size_t i = 0; i < kPipelineCacheUuidBytes; ++i) {
    device.uuid[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return device;
}

void PutLe32(std::string* s, size_t offset, uint32_t v) {
  (*s)[offset] = static_cast<char>(v & 0xff);
  (*s)[offset + 1] = static_cast<char>((v >> 8) & 0xff);
  (*s)[offset + 2] = static_cast<char>((v >> 16) & 0xff);
  (*s)[offset + 3] = static_cast<char>((v >> 24) & 0xff);
}

// A version-one cache for |device| with |body| bytes of entries.
std::string Cache(const PipelineCacheDevice& device, size_t body = 100) {
  std::string s(kPipelineCacheHeaderBytes + body, '\x5a');
  PutLe32(&s, 0, kPipelineCacheHeaderBytes);
  PutLe32(&s, 4, kPipelineCacheHeaderVersionOne);
  PutLe32(&s, 8, device.vendor_id);
  PutLe32(&s, 12, device.device_id);
  for (size_t i = 0; i < kPipelineCacheUuidBytes; ++i) {
    s[16 + i] = static_cast<char>(device.uuid[i]);
  }
  return s;
}

TEST(GpuPipelineCacheTest, AcceptsTheDevicesOwnCache) {
  EXPECT_EQ(PipelineCacheCheck::kOk, CheckPipelineCacheHeader(Cache(Device()),
                                                              Device()));
  // A header alone (a driver's empty cache) is a cache too.
  EXPECT_EQ(PipelineCacheCheck::kOk,
            CheckPipelineCacheHeader(Cache(Device(), 0), Device()));
  // Drivers may write a longer header; its end must be in the data.
  std::string longer = Cache(Device(), 16);
  PutLe32(&longer, 0, kPipelineCacheHeaderBytes + 16);
  EXPECT_EQ(PipelineCacheCheck::kOk, CheckPipelineCacheHeader(longer, Device()));
}

TEST(GpuPipelineCacheTest, EmptyAndTruncated) {
  EXPECT_EQ(PipelineCacheCheck::kEmpty,
            CheckPipelineCacheHeader(std::string(), Device()));
  const std::string cache = Cache(Device(), 0);
  EXPECT_EQ(PipelineCacheCheck::kTruncated,
            CheckPipelineCacheHeader(cache.substr(0, 31), Device()));
  EXPECT_EQ(PipelineCacheCheck::kTruncated,
            CheckPipelineCacheHeader(cache.substr(0, 1), Device()));
}

TEST(GpuPipelineCacheTest, SizeCap) {
  const std::string cache = Cache(Device(), 68);  // 100 bytes
  EXPECT_EQ(PipelineCacheCheck::kOk,
            CheckPipelineCacheHeader(cache, Device(), 100));
  EXPECT_EQ(PipelineCacheCheck::kTooLarge,
            CheckPipelineCacheHeader(cache, Device(), 99));
  EXPECT_EQ(32u * 1024 * 1024, kMaxPipelineCacheBytes);
}

TEST(GpuPipelineCacheTest, HeaderSize) {
  std::string cache = Cache(Device(), 8);
  PutLe32(&cache, 0, 31);
  EXPECT_EQ(PipelineCacheCheck::kBadHeaderSize,
            CheckPipelineCacheHeader(cache, Device()));
  PutLe32(&cache, 0, 0);
  EXPECT_EQ(PipelineCacheCheck::kBadHeaderSize,
            CheckPipelineCacheHeader(cache, Device()));
  // Exactly the data's length is fine, one more isn't.
  PutLe32(&cache, 0, 40);
  EXPECT_EQ(PipelineCacheCheck::kOk, CheckPipelineCacheHeader(cache, Device()));
  PutLe32(&cache, 0, 41);
  EXPECT_EQ(PipelineCacheCheck::kBadHeaderSize,
            CheckPipelineCacheHeader(cache, Device()));
  PutLe32(&cache, 0, 0xffffffffu);
  EXPECT_EQ(PipelineCacheCheck::kBadHeaderSize,
            CheckPipelineCacheHeader(cache, Device()));
}

TEST(GpuPipelineCacheTest, HeaderVersion) {
  std::string cache = Cache(Device());
  // VK_PIPELINE_CACHE_HEADER_VERSION_SAFETY_CRITICAL_ONE.
  PutLe32(&cache, 4, 1000298001);
  EXPECT_EQ(PipelineCacheCheck::kBadHeaderVersion,
            CheckPipelineCacheHeader(cache, Device()));
  PutLe32(&cache, 4, 0);
  EXPECT_EQ(PipelineCacheCheck::kBadHeaderVersion,
            CheckPipelineCacheHeader(cache, Device()));
  PutLe32(&cache, 4, 2);
  EXPECT_EQ(PipelineCacheCheck::kBadHeaderVersion,
            CheckPipelineCacheHeader(cache, Device()));
}

TEST(GpuPipelineCacheTest, WrongDevice) {
  PipelineCacheDevice other = Device();
  other.vendor_id = 0x10de;
  EXPECT_EQ(PipelineCacheCheck::kWrongVendor,
            CheckPipelineCacheHeader(Cache(other), Device()));
  other = Device();
  other.device_id = 0x73fe;
  EXPECT_EQ(PipelineCacheCheck::kWrongDevice,
            CheckPipelineCacheHeader(Cache(other), Device()));
  // Every byte of the UUID counts, the last one too.
  for (size_t i : {size_t{0}, size_t{7}, kPipelineCacheUuidBytes - 1}) {
    other = Device();
    other.uuid[i] ^= 1;
    EXPECT_EQ(PipelineCacheCheck::kWrongUuid,
              CheckPipelineCacheHeader(Cache(other), Device()))
        << i;
  }
}

TEST(GpuPipelineCacheTest, LittleEndianFields) {
  // 0x00001002 written big-endian is another vendor.
  std::string cache = Cache(Device());
  cache[8] = 0;
  cache[9] = 0;
  cache[10] = 0x10;
  cache[11] = 0x02;
  EXPECT_EQ(PipelineCacheCheck::kWrongVendor,
            CheckPipelineCacheHeader(cache, Device()));
}

TEST(GpuPipelineCacheTest, DeviceWithoutUuidKeepsNoCache) {
  PipelineCacheDevice no_uuid = Device();
  no_uuid.uuid = {};
  EXPECT_EQ(PipelineCacheCheck::kNoDeviceUuid,
            CheckPipelineCacheHeader(Cache(no_uuid), no_uuid));
  EXPECT_EQ(PipelineCacheCheck::kNoDeviceUuid,
            CheckPipelineCacheHeader(std::string(), no_uuid));
}

TEST(GpuPipelineCacheTest, NamesEveryResult) {
  for (PipelineCacheCheck check :
       {PipelineCacheCheck::kOk, PipelineCacheCheck::kEmpty,
        PipelineCacheCheck::kTruncated, PipelineCacheCheck::kTooLarge,
        PipelineCacheCheck::kBadHeaderSize,
        PipelineCacheCheck::kBadHeaderVersion,
        PipelineCacheCheck::kWrongVendor, PipelineCacheCheck::kWrongDevice,
        PipelineCacheCheck::kWrongUuid, PipelineCacheCheck::kNoDeviceUuid}) {
    EXPECT_NE(std::string("unknown"), PipelineCacheCheckName(check));
  }
}

}  // namespace
}  // namespace molt_ai
