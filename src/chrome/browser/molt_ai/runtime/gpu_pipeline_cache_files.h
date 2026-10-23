// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser's pipeline cache files (docs/design/SANDBOXED_INFERENCE.md
// section 11.12): one per GPU, driver and ggml shader set, in
// <user data>/MoltAI/GpuCache/. A file's name is a digest of what it is for
// (the driver's key, the device's pipelineCacheUUID and MoltBrowser's
// version, which names the shader set): nothing the inference service sent
// is ever part of a path. The directory keeps at most kMaxPipelineCacheFiles
// files.
//
// Std-only: the digest is computed by the caller (crypto::hash), so the
// naming and the directory's cap are unit-tested standalone.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_PIPELINE_CACHE_FILES_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_PIPELINE_CACHE_FILES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/runtime/gpu_driver_record.h"

namespace molt_ai {

// The directory, below the user data directory.
inline constexpr char kGpuCacheDirName[] = "GpuCache";
inline constexpr char kMoltAiDirName[] = "MoltAI";

// Files kept in it (one per GPU and driver in use; old ones go first).
inline constexpr size_t kMaxPipelineCacheFiles = 4;

// What a cache file's name is a digest of: "molt-vk-pipeline-cache:1|" +
// the driver's GpuDriverKeyToString() + "|" + the pipelineCacheUUID in hex +
// "|" + |shader_set| with only [0-9a-z./_-] kept. "" for an unidentified
// key (no file for it).
std::string PipelineCacheKeyText(
    const GpuDriverKey& key,
    const std::array<uint8_t, 16>& pipeline_cache_uuid,
    std::string_view shader_set);

// "vk-" + the first 32 characters of |digest_hex| + ".bin", or "" unless
// |digest_hex| starts with 32 lowercase hex digits.
std::string PipelineCacheFileName(std::string_view digest_hex);

// Whether |name| is one PipelineCacheFileName() makes; the directory's cap
// and its deletion touch only those.
bool IsPipelineCacheFileName(std::string_view name);

struct PipelineCacheFileInfo {
  std::string name;
  // Last modified, any monotonic unit.
  int64_t modified = 0;
};

// The files to delete so that at most kMaxPipelineCacheFiles remain, never
// |keep|: the least recently modified first. Files whose name
// IsPipelineCacheFileName() rejects are ignored.
std::vector<std::string> PipelineCacheFilesToDelete(
    std::vector<PipelineCacheFileInfo> files,
    std::string_view keep);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_GPU_PIPELINE_CACHE_FILES_H_
