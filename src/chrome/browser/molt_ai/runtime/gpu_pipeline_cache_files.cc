// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/gpu_pipeline_cache_files.h"

#include <algorithm>
#include <utility>

namespace molt_ai {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr size_t kDigestChars = 32;
constexpr char kNamePrefix[] = "vk-";
constexpr char kNameSuffix[] = ".bin";
constexpr size_t kNameBytes = 3 + kDigestChars + 4;

bool IsLowerHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool IsShaderSetChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' ||
         c == '/' || c == '_' || c == '-';
}

}  // namespace

std::string PipelineCacheKeyText(
    const GpuDriverKey& key,
    const std::array<uint8_t, 16>& pipeline_cache_uuid,
    std::string_view shader_set) {
  if (!key.identified) return "";
  std::string text = "molt-vk-pipeline-cache:1|";
  text += GpuDriverKeyToString(key);
  text += '|';
  for (uint8_t byte : pipeline_cache_uuid) {
    text.push_back(kHexDigits[byte >> 4]);
    text.push_back(kHexDigits[byte & 0xf]);
  }
  text += '|';
  for (char c : shader_set) {
    if (IsShaderSetChar(c)) text.push_back(c);
  }
  return text;
}

std::string PipelineCacheFileName(std::string_view digest_hex) {
  if (digest_hex.size() < kDigestChars) return "";
  for (size_t i = 0; i < kDigestChars; ++i) {
    if (!IsLowerHex(digest_hex[i])) return "";
  }
  return std::string(kNamePrefix) +
         std::string(digest_hex.substr(0, kDigestChars)) + kNameSuffix;
}

bool IsPipelineCacheFileName(std::string_view name) {
  if (name.size() != kNameBytes || name.substr(0, 3) != kNamePrefix ||
      name.substr(3 + kDigestChars) != kNameSuffix) {
    return false;
  }
  for (size_t i = 3; i < 3 + kDigestChars; ++i) {
    if (!IsLowerHex(name[i])) return false;
  }
  return true;
}

std::vector<std::string> PipelineCacheFilesToDelete(
    std::vector<PipelineCacheFileInfo> files,
    std::string_view keep) {
  std::erase_if(files, [](const PipelineCacheFileInfo& f) {
    return !IsPipelineCacheFileName(f.name);
  });
  // Newest first; |keep| before everything.
  std::stable_sort(files.begin(), files.end(),
                   [&](const PipelineCacheFileInfo& a,
                       const PipelineCacheFileInfo& b) {
                     const bool a_keep = a.name == keep;
                     const bool b_keep = b.name == keep;
                     if (a_keep != b_keep) return a_keep;
                     return a.modified > b.modified;
                   });
  std::vector<std::string> doomed;
  for (size_t i = kMaxPipelineCacheFiles; i < files.size(); ++i) {
    doomed.push_back(std::move(files[i].name));
  }
  return doomed;
}

}  // namespace molt_ai
