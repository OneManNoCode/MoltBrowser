// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's url/gurl.h, for
// tools/fuzz/molt_fuzz.py only. A deliberately small parser: enough for
// MoltBrowser's scheme / host / port decisions to be exercised. The real
// canonicalizer is stricter and does more (IDN, IPv4 forms, escapes,
// backslashes); the GN fuzzer_test targets use it. Like upstream, host()
// and scheme() return std::string_view and a standard URL's default port
// is dropped.

#ifndef MOLT_FUZZ_STAND_INS_URL_GURL_H_
#define MOLT_FUZZ_STAND_INS_URL_GURL_H_

#include <string>
#include <string_view>

class GURL {
 public:
  GURL() = default;
  explicit GURL(std::string_view in) { Parse(in); }
  explicit GURL(const char* in) : GURL(std::string_view(in)) {}
  explicit GURL(const std::string& in) : GURL(std::string_view(in)) {}

  bool is_valid() const { return valid_; }
  bool is_empty() const { return spec_.empty(); }
  const std::string& spec() const { return spec_; }
  const std::string& possibly_invalid_spec() const { return spec_; }
  std::string_view scheme() const { return scheme_; }
  std::string_view host() const { return host_; }
  std::string_view host_piece() const { return host_; }
  std::string_view path() const { return path_; }
  std::string_view path_piece() const { return path_; }
  bool has_host() const { return !host_.empty(); }
  bool has_port() const { return port_ >= 0; }
  // The explicit port, or -1 (url::PORT_UNSPECIFIED).
  int IntPort() const { return port_; }
  int EffectiveIntPort() const {
    if (port_ >= 0) return port_;
    if (scheme_ == "http" || scheme_ == "ws") return 80;
    if (scheme_ == "https" || scheme_ == "wss") return 443;
    return -1;
  }
  bool SchemeIs(std::string_view s) const { return valid_ && scheme_ == s; }
  bool SchemeIsHTTPOrHTTPS() const {
    return SchemeIs("http") || SchemeIs("https");
  }
  bool SchemeIsCryptographic() const {
    return SchemeIs("https") || SchemeIs("wss");
  }
  bool DomainIs(std::string_view domain) const {
    if (!valid_ || host_.empty()) return false;
    if (host_ == domain) return true;
    return host_.size() > domain.size() &&
           host_.compare(host_.size() - domain.size(), domain.size(),
                         domain) == 0 &&
           host_[host_.size() - domain.size() - 1] == '.';
  }

  friend bool operator==(const GURL& a, const GURL& b) {
    return a.spec_ == b.spec_ && a.valid_ == b.valid_;
  }

 private:
  static bool IsStandard(const std::string& s) {
    return s == "http" || s == "https" || s == "ftp" || s == "ws" ||
           s == "wss";
  }
  void Parse(std::string_view in) {
    size_t b = 0;
    size_t e = in.size();
    while (b < e && static_cast<unsigned char>(in[b]) <= 0x20) ++b;
    while (e > b && static_cast<unsigned char>(in[e - 1]) <= 0x20) --e;
    std::string s(in.substr(b, e - b));
    spec_ = s;
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0) return;
    std::string scheme;
    for (size_t i = 0; i < colon; ++i) {
      char c = s[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      const bool ok = (c >= 'a' && c <= 'z') ||
                      (i > 0 && ((c >= '0' && c <= '9') || c == '+' ||
                                 c == '-' || c == '.'));
      if (!ok) return;
      scheme.push_back(c);
    }
    const std::string rest = s.substr(colon + 1);
    if (!IsStandard(scheme)) {
      scheme_ = scheme;
      spec_ = scheme + ":" + rest;
      path_ = rest;
      valid_ = true;
      return;
    }
    if (rest.compare(0, 2, "//") != 0) return;
    const std::string auth = rest.substr(2);
    const size_t end = auth.find_first_of("/?#");
    std::string hostport = auth.substr(0, end);
    const size_t at = hostport.rfind('@');
    if (at != std::string::npos) hostport = hostport.substr(at + 1);
    std::string host;
    std::string port_text;
    if (!hostport.empty() && hostport[0] == '[') {
      const size_t close = hostport.find(']');
      if (close == std::string::npos) return;
      host = hostport.substr(0, close + 1);
      if (close + 1 < hostport.size()) {
        if (hostport[close + 1] != ':') return;
        port_text = hostport.substr(close + 2);
      }
    } else {
      const size_t pc = hostport.find(':');
      host = hostport.substr(0, pc);
      if (pc != std::string::npos) port_text = hostport.substr(pc + 1);
    }
    for (char& c : host) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (host.empty()) return;
    for (char c : host) {
      const auto u = static_cast<unsigned char>(c);
      if (u <= 0x20 || u == 0x7F || c == '<' || c == '>' || c == '"' ||
          c == '\\' || c == '%' || c == '^' || c == '|' || c == '#' ||
          c == '?' || c == '/' || c == '@') {
        return;
      }
    }
    int port = -1;
    if (!port_text.empty()) {
      if (port_text.size() > 5) return;
      port = 0;
      for (char c : port_text) {
        if (c < '0' || c > '9') return;
        port = port * 10 + (c - '0');
      }
      if (port > 65535) return;
    }
    if ((scheme == "http" && port == 80) || (scheme == "https" && port == 443)) {
      port = -1;
    }
    std::string path = end == std::string::npos ? "/" : auth.substr(end);
    if (path.empty() || path[0] != '/') path = "/" + path;
    scheme_ = scheme;
    host_ = host;
    port_ = port;
    path_ = path;
    spec_ = scheme + "://" + host_ +
            (port_ >= 0 ? ":" + std::to_string(port_) : std::string()) + path;
    valid_ = true;
  }

  bool valid_ = false;
  std::string spec_;
  std::string scheme_;
  std::string host_;
  std::string path_;
  int port_ = -1;
};

#endif  // MOLT_FUZZ_STAND_INS_URL_GURL_H_
