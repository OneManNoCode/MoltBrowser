// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/version.h, for
// tools/fuzz/molt_fuzz.py only, with upstream's parsing rules: dot-separated
// unsigned 32-bit numbers, no '+' sign, no leading zero in the first one;
// comparison treats missing trailing components as 0.

#ifndef MOLT_FUZZ_STAND_INS_BASE_VERSION_H_
#define MOLT_FUZZ_STAND_INS_BASE_VERSION_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/string_number_conversions.h"

namespace base {

class Version {
 public:
  Version() = default;
  explicit Version(std::string_view version) {
    std::vector<uint32_t> parsed;
    size_t start = 0;
    while (true) {
      const size_t dot = version.find('.', start);
      const std::string_view part = version.substr(start, dot - start);
      unsigned num = 0;
      if (part.empty() || part[0] == '+' || !StringToUint(part, &num)) return;
      if (parsed.empty() && NumberToString(num) != part) return;
      parsed.push_back(num);
      if (dot == std::string_view::npos) break;
      start = dot + 1;
    }
    components_ = std::move(parsed);
  }

  bool IsValid() const { return !components_.empty(); }
  const std::vector<uint32_t>& components() const { return components_; }

  int CompareTo(const Version& other) const {
    const size_t n = std::max(components_.size(), other.components_.size());
    for (size_t i = 0; i < n; ++i) {
      const uint32_t a = i < components_.size() ? components_[i] : 0;
      const uint32_t b = i < other.components_.size() ? other.components_[i] : 0;
      if (a != b) return a < b ? -1 : 1;
    }
    return 0;
  }

  std::string GetString() const {
    std::string out;
    for (size_t i = 0; i < components_.size(); ++i) {
      if (i) out += '.';
      out += NumberToString(components_[i]);
    }
    return out;
  }

  friend bool operator==(const Version& a, const Version& b) {
    return a.CompareTo(b) == 0;
  }
  friend bool operator<(const Version& a, const Version& b) {
    return a.CompareTo(b) < 0;
  }
  friend bool operator<=(const Version& a, const Version& b) {
    return a.CompareTo(b) <= 0;
  }
  friend bool operator>(const Version& a, const Version& b) {
    return a.CompareTo(b) > 0;
  }
  friend bool operator>=(const Version& a, const Version& b) {
    return a.CompareTo(b) >= 0;
  }

 private:
  std::vector<uint32_t> components_;
};

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_VERSION_H_
