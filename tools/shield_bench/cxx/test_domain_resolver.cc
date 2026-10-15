// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// shield_bench stand-in for src/.../shield/adblock/domain_resolver.cc, which
// uses Chromium's net::registry_controlled_domains. This one knows a small
// set of multi-label public suffixes — enough for the benchmark corpus and
// tests; the browser uses the full PSL.

#include "chrome/browser/molt_ai/shield/adblock/domain_resolver.h"

#include <string_view>

#include "shield_bench/cxx/cxx_smoke.h"

namespace molt_ai::shield::ffi {

namespace {

constexpr std::string_view kMultiLabelSuffixes[] = {
    "co.uk",  "org.uk", "ac.uk",  "gov.uk", "co.jp",  "ne.jp",  "or.jp",
    "com.au", "net.au", "org.au", "com.br", "com.cn", "com.tw", "com.mx",
    "co.in",  "co.kr",  "co.nz",  "co.za",  "com.ar", "com.tr", "com.sg",
    "github.io", "blogspot.com", "herokuapp.com", "appspot.com",
    "cloudfront.net", "azurewebsites.net", "s3.amazonaws.com",
};

bool IsIpLiteral(std::string_view host) {
  if (!host.empty() && host.front() == '[') {
    return true;  // IPv6
  }
  for (char c : host) {
    if (!((c >= '0' && c <= '9') || c == '.')) {
      return false;
    }
  }
  return !host.empty();
}

// Byte offset of the `n`-th label counted from the end (1 = last label), or
// 0 if the host has fewer than `n` labels.
size_t NthLabelFromEnd(std::string_view host, int n) {
  size_t end = host.size();
  for (int i = 1; i <= n; ++i) {
    size_t dot = end == 0 ? std::string_view::npos : host.rfind('.', end - 1);
    if (dot == std::string_view::npos) {
      return 0;  // Ran out of labels: the whole host is the domain.
    }
    if (i == n) {
      return dot + 1;
    }
    end = dot;
  }
  return 0;
}

}  // namespace

size_t registrable_domain_offset(rust::Str host_str) {
  std::string_view host(host_str.data(), host_str.size());
  if (host.empty() || IsIpLiteral(host)) {
    return 0;
  }
  int suffix_labels = 1;
  for (std::string_view suffix : kMultiLabelSuffixes) {
    if (host.size() > suffix.size() &&
        host.substr(host.size() - suffix.size()) == suffix &&
        host[host.size() - suffix.size() - 1] == '.') {
      suffix_labels = 1;
      for (char c : suffix) {
        suffix_labels += c == '.';
      }
      break;
    }
  }
  return NthLabelFromEnd(host, suffix_labels + 1);
}

}  // namespace molt_ai::shield::ffi

namespace shield_bench {

size_t bench_domain_offset(rust::Str host) {
  return molt_ai::shield::ffi::registrable_domain_offset(host);
}

}  // namespace shield_bench
