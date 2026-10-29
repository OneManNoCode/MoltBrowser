// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_limits.h"

#include <stdint.h>

#include <limits>
#include <string>

namespace molt_ai {
namespace helper_limits {

namespace {

uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
  return a > std::numeric_limits<uint64_t>::max() - b
             ? std::numeric_limits<uint64_t>::max()
             : a + b;
}

}  // namespace

uint64_t MaxScratchEntries(const ScratchUsage& before) {
  return SaturatingAdd(before.entries, kMaxScratchNewEntries);
}

ScratchVerdict JudgeScratch(const ScratchUsage& before,
                            const ScratchUsage& now) {
  switch (now.gap) {
    case ScratchUsage::Gap::kNone:
      break;
    case ScratchUsage::Gap::kTooManyEntries:
      return ScratchVerdict::kTooManyEntries;
    case ScratchUsage::Gap::kTooDeep:
      return ScratchVerdict::kTooDeep;
    case ScratchUsage::Gap::kUnreadable:
      return ScratchVerdict::kUnmeasurable;
  }
  if (now.entries > MaxScratchEntries(before)) {
    return ScratchVerdict::kTooManyEntries;
  }
  // Less than kMaxHelperWriteBytes more than the browser put there. On
  // Linux a file stops at exactly kMaxHelperWriteBytes (RLIMIT_FSIZE), so
  // a helper that ran into that limit is past this one too.
  if (now.bytes >= SaturatingAdd(before.bytes, kMaxHelperWriteBytes)) {
    return ScratchVerdict::kTooMuchData;
  }
  return ScratchVerdict::kWithinLimits;
}

std::string DescribeScratchVerdict(ScratchVerdict verdict) {
  switch (verdict) {
    case ScratchVerdict::kWithinLimits:
      return std::string();
    case ScratchVerdict::kTooMuchData:
      return "it wrote " + std::to_string(kMaxHelperWriteBytes >> 20) +
             " MiB or more to its scratch directory";
    case ScratchVerdict::kTooManyEntries:
      return "it made more than " + std::to_string(kMaxScratchNewEntries) +
             " files in its scratch directory";
    case ScratchVerdict::kTooDeep:
      return "it nested directories more than " +
             std::to_string(kMaxScratchDepth) +
             " deep in its scratch directory";
    case ScratchVerdict::kUnmeasurable:
      return "what it wrote to its scratch directory couldn't be measured";
  }
  return "its scratch directory is past a limit";
}

std::string DescribeMemoryLimit() {
  return "it used more than " + std::to_string(kMaxHelperMemoryBytes >> 30) +
         " GiB of memory";
}

}  // namespace helper_limits
}  // namespace molt_ai
