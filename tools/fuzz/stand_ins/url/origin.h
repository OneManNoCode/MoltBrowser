// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's url/origin.h, for
// tools/fuzz/molt_fuzz.py only: tuple origins for http(s), ws(s) and ftp,
// opaque for everything else (and an opaque origin is same-origin with
// nothing but itself). Serialize() writes "scheme://host[:port]" without
// the default port, and "null" for an opaque origin.

#ifndef MOLT_FUZZ_STAND_INS_URL_ORIGIN_H_
#define MOLT_FUZZ_STAND_INS_URL_ORIGIN_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "url/gurl.h"

namespace url {

class Origin {
 public:
  Origin() : nonce_(++NextNonce()) {}
  static Origin Create(const GURL& url) {
    Origin o;
    if (!url.is_valid() || !url.has_host()) return o;
    const std::string_view s = url.scheme();
    if (s != "http" && s != "https" && s != "ws" && s != "wss" && s != "ftp") {
      return o;
    }
    o.opaque_ = false;
    o.nonce_ = 0;
    o.scheme_ = std::string(s);
    o.host_ = std::string(url.host());
    o.port_ = static_cast<uint16_t>(url.EffectiveIntPort());
    return o;
  }
  bool opaque() const { return opaque_; }
  const std::string& scheme() const { return scheme_; }
  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }
  std::string Serialize() const {
    if (opaque_) return "null";
    std::string out = scheme_ + "://" + host_;
    const bool default_port = (scheme_ == "http" && port_ == 80) ||
                              (scheme_ == "https" && port_ == 443);
    if (!default_port) out += ":" + std::to_string(port_);
    return out;
  }
  GURL GetURL() const { return opaque_ ? GURL() : GURL(Serialize() + "/"); }
  bool IsSameOriginWith(const Origin& other) const {
    if (opaque_ || other.opaque_) {
      return opaque_ && other.opaque_ && nonce_ == other.nonce_;
    }
    return scheme_ == other.scheme_ && host_ == other.host_ &&
           port_ == other.port_;
  }
  bool IsSameOriginWith(const GURL& url) const {
    return IsSameOriginWith(Create(url));
  }
  friend bool operator==(const Origin& a, const Origin& b) {
    return a.IsSameOriginWith(b);
  }
  friend bool operator!=(const Origin& a, const Origin& b) { return !(a == b); }

 private:
  static uint64_t& NextNonce() {
    static uint64_t nonce = 0;
    return nonce;
  }

  bool opaque_ = true;
  uint64_t nonce_ = 0;
  std::string scheme_;
  std::string host_;
  uint16_t port_ = 0;
};

}  // namespace url

#endif  // MOLT_FUZZ_STAND_INS_URL_ORIGIN_H_
