// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_util.h"

#include <stddef.h>

namespace molt_ai {
namespace tor {

namespace {

constexpr std::string_view kOnionLabel = "onion";
constexpr std::string_view kOnionSuffix = ".onion";

// A v3 address is base32(32-byte key | 2-byte checksum | version 0x03):
// 35 bytes = 280 bits = exactly 56 base32 characters.
constexpr size_t kV3AddressLength = 56;
constexpr size_t kMaxHostLength = 253;
constexpr size_t kMaxLabelLength = 63;
constexpr size_t kMaxOnionLocationLength = 2048;

char ToLowerASCII(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool EqualsIgnoreASCIICase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerASCII(a[i]) != ToLowerASCII(b[i])) {
      return false;
    }
  }
  return true;
}

bool StartsWithIgnoreASCIICase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         EqualsIgnoreASCIICase(s.substr(0, prefix.size()), prefix);
}

// RFC 4648 base32 alphabet, lowercase (as in onion addresses).
bool IsBase32Char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7');
}

bool IsSubdomainLabel(std::string_view label) {
  if (label.empty() || label.size() > kMaxLabelLength) {
    return false;
  }
  for (char c : label) {
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool IsV3Address(std::string_view address) {
  if (address.size() != kV3AddressLength) {
    return false;
  }
  for (char c : address) {
    if (!IsBase32Char(c)) {
      return false;
    }
  }
  // The last 15 bits are checksum[1] (low 2 bits) + the version byte 0x03,
  // so the 56th character encodes 0b00011 ('d') and the 55th is xx000
  // ('a', 'i', 'q' or 'y').
  const char second_last = address[kV3AddressLength - 2];
  return address.back() == 'd' && (second_last == 'a' || second_last == 'i' ||
                                   second_last == 'q' || second_last == 'y');
}

std::string_view TrimSpacesAndTabs(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

bool IsOnionHost(std::string_view host) {
  while (!host.empty() && host.back() == '.') {
    host.remove_suffix(1);
  }
  if (host.size() < kOnionLabel.size() ||
      !EqualsIgnoreASCIICase(host.substr(host.size() - kOnionLabel.size()),
                             kOnionLabel)) {
    return false;
  }
  return host.size() == kOnionLabel.size() ||
         host[host.size() - kOnionLabel.size() - 1] == '.';
}

bool IsV3OnionServiceHost(std::string_view host) {
  if (host.size() > kMaxHostLength ||
      host.size() < kV3AddressLength + kOnionSuffix.size() ||
      host.substr(host.size() - kOnionSuffix.size()) != kOnionSuffix) {
    return false;
  }
  const std::string_view rest =
      host.substr(0, host.size() - kOnionSuffix.size());
  const size_t dot = rest.rfind('.');
  if (dot == std::string_view::npos) {
    return IsV3Address(rest);
  }
  if (!IsV3Address(rest.substr(dot + 1))) {
    return false;
  }
  std::string_view subdomains = rest.substr(0, dot);
  while (true) {
    const size_t next = subdomains.find('.');
    if (!IsSubdomainLabel(subdomains.substr(0, next))) {
      return false;
    }
    if (next == std::string_view::npos) {
      return true;
    }
    subdomains.remove_prefix(next + 1);
  }
}

std::optional<OnionLocation> ParseOnionLocationHeaderValues(
    const std::vector<std::string>& values) {
  // Several Onion-Location headers are ambiguous: ignore them all.
  if (values.size() != 1) {
    return std::nullopt;
  }
  std::string_view value = TrimSpacesAndTabs(values[0]);
  if (value.empty() || value.size() > kMaxOnionLocationLength) {
    return std::nullopt;
  }
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    // Printable ASCII only. A backslash is a path separator to URL parsers
    // but not to this one, so it is refused rather than interpreted.
    if (u < 0x21 || u > 0x7e || c == '\\') {
      return std::nullopt;
    }
  }

  OnionLocation result;
  if (StartsWithIgnoreASCIICase(value, "https://")) {
    result.scheme = "https";
    value.remove_prefix(8);
  } else if (StartsWithIgnoreASCIICase(value, "http://")) {
    result.scheme = "http";
    value.remove_prefix(7);
  } else {
    return std::nullopt;
  }

  const std::string_view authority =
      value.substr(0, value.find_first_of("/?#"));
  // No user info, IP literals or escaped host characters.
  if (authority.empty() ||
      authority.find_first_of("@[]%") != std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view host = authority;
  const size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    const std::string_view port = authority.substr(colon + 1);
    host = authority.substr(0, colon);
    if (port.empty() || port.size() > 5) {
      return std::nullopt;
    }
    unsigned int number = 0;
    for (char c : port) {
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      number = number * 10 + static_cast<unsigned int>(c - '0');
    }
    if (number < 1 || number > 65535) {
      return std::nullopt;
    }
  }

  result.host.reserve(host.size());
  for (char c : host) {
    result.host.push_back(ToLowerASCII(c));
  }
  if (!IsV3OnionServiceHost(result.host)) {
    return std::nullopt;
  }
  return result;
}

bool ShouldHonorOnionLocation(std::string_view page_scheme,
                              std::string_view page_host) {
  return page_scheme == "https" && !page_host.empty() &&
         !IsOnionHost(page_host);
}

}  // namespace tor
}  // namespace molt_ai
