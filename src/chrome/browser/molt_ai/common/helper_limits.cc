// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_limits.h"

#include <stdint.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

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

CpuUsageWindow::CpuUsageWindow(double max_cores, int64_t window_ms)
    : max_cores_(max_cores), window_ms_(window_ms > 0 ? window_ms : 1) {}

CpuUsageWindow::~CpuUsageWindow() = default;

bool CpuUsageWindow::Add(int64_t now_ms, int64_t cpu_ms) {
  if (past_) {
    return false;
  }
  // A clock or a counter that goes back starts the window again.
  if (!samples_.empty() &&
      (now_ms < samples_.back().first || cpu_ms < samples_.back().second)) {
    samples_.clear();
  }
  samples_.emplace_back(now_ms, cpu_ms);
  // The oldest sample kept is the last one at least a window old, so the
  // span judged covers the whole window once there is one.
  while (samples_.size() > 2 && now_ms - samples_[1].first >= window_ms_) {
    samples_.pop_front();
  }
  const double used = static_cast<double>(cpu_ms - samples_.front().second);
  const double span = static_cast<double>(
      std::max(now_ms - samples_.front().first, window_ms_));
  past_ = used > max_cores_ * span;
  return !past_;
}

std::string CpuUsageWindow::Describe() const {
  const int64_t hundredths = static_cast<int64_t>(max_cores_ * 100 + 0.5);
  std::string cores = std::to_string(hundredths / 100);
  if (hundredths % 100 != 0) {
    const int64_t fraction = hundredths % 100;
    cores += fraction < 10 ? ".0" : ".";
    cores += std::to_string(fraction);
    while (cores.back() == '0') {
      cores.pop_back();
    }
  }
  return "it used more than " + cores + " of a CPU core for " +
         std::to_string(window_ms_ / 1000) + " s";
}

std::optional<uint64_t> ParseProcStatCpuTicks(std::string_view stat) {
  // "pid (comm) state ppid ...": the command name may hold spaces and
  // parentheses, so the fields start after the last ')'.
  const size_t close = stat.rfind(')');
  if (close == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view rest = stat.substr(close + 1);
  // Fields from the 3rd (state) on; utime is the 14th, stime the 15th.
  uint64_t total = 0;
  for (int field = 3; field <= 15; ++field) {
    while (!rest.empty() && rest.front() == ' ') {
      rest.remove_prefix(1);
    }
    if (rest.empty()) {
      return std::nullopt;
    }
    size_t end = rest.find(' ');
    if (end == std::string_view::npos) {
      end = rest.size();
    }
    const std::string_view token = rest.substr(0, end);
    if (field >= 14) {
      if (token.empty() || token.size() > 19) {
        return std::nullopt;
      }
      uint64_t value = 0;
      for (const char c : token) {
        if (c < '0' || c > '9') {
          return std::nullopt;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
      }
      total = SaturatingAdd(total, value);
    }
    rest.remove_prefix(end);
  }
  return total;
}

}  // namespace helper_limits
}  // namespace molt_ai
