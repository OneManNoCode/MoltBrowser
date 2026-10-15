// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Plain data types shared by MoltShield's engine sequence, network hooks,
// tab helper and UI. No Chromium/Rust dependencies beyond //base so they can
// cross sequences freely.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_TYPES_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/time/time.h"

namespace molt_ai::shield {

// One network request, normalized for adblock-rust.
struct CheckInput {
  CheckInput();
  CheckInput(const CheckInput&);
  CheckInput(CheckInput&&);
  CheckInput& operator=(const CheckInput&);
  CheckInput& operator=(CheckInput&&);
  ~CheckInput();

  std::string url;          // Canonical GURL spec.
  std::string host;         // Request host.
  std::string source_host;  // Initiator (document) host; may be empty.
  std::string type;         // adblock-rust type ("script", "image", ...).
  bool third_party = true;  // Per net::registry_controlled_domains.
  std::string method = "GET";
};

// Engine verdict for one request.
struct CheckResult {
  CheckResult();
  CheckResult(const CheckResult&);
  CheckResult(CheckResult&&);
  CheckResult& operator=(const CheckResult&);
  CheckResult& operator=(CheckResult&&);
  ~CheckResult();

  bool should_block = false;
  bool important = false;
  bool has_exception = false;
  // `data:` URL of a surrogate to serve instead ($redirect), or empty.
  std::string redirect_data_url;
  // URL with $removeparam parameters stripped, or empty if unchanged.
  std::string rewritten_url;
};

// Cosmetic resources for one document.
struct CosmeticData {
  CosmeticData();
  CosmeticData(const CosmeticData&);
  CosmeticData(CosmeticData&&);
  CosmeticData& operator=(const CosmeticData&);
  CosmeticData& operator=(CosmeticData&&);
  ~CosmeticData();

  std::string hide_css;         // Vetted `sel{display:none!important}` rules.
  std::string scriptlet_js;     // Main-world IIFE; args JSON-escaped.
  bool generichide = false;     // Skip the generic class/id pass.
  // Procedural/action filters for the renderer's procedural engine (vetted
  // JSON data, never script; empty if none) and the sanitized declaration
  // blocks their :style() actions refer to by index.
  std::string procedural_json;
  std::vector<std::string> procedural_styles;
};

// Session counters surfaced in the MoltShield settings section. Field names
// kept from the pre-adblock-rust ShieldStats the UI was designed around.
struct ShieldStats {
  int64_t total_requests_checked = 0;
  int64_t trackers_blocked = 0;  // Requests cancelled or served a surrogate.
  int64_t ads_blocked = 0;       // Subset: sub_frame/image/media/script ads.
  int64_t redirects_served = 0;  // Blocked with a surrogate resource.
  int64_t websockets_blocked = 0;
  int64_t cosmetic_rules_applied = 0;  // Documents given hide CSS.
  int64_t scriptlets_injected = 0;     // Documents given scriptlets.
  int64_t params_stripped = 0;  // Requests/navigations with tracking
                                // parameters removed.
};

// Status of one filter list, for the UI and breakage reports.
struct ListStatus {
  ListStatus();
  ListStatus(const ListStatus&);
  ListStatus& operator=(const ListStatus&);
  ~ListStatus();

  std::string id;
  std::string title;
  bool enabled = false;
  bool cached = false;      // A downloaded copy exists on disk.
  uint32_t rule_lines = 0;  // Filter lines after preprocessing.
  std::string version;      // "! Version:" / "! Last modified:" header.
  std::string sha256_hex;   // Of the cached copy.
  base::Time fetched_at;    // Last time the content changed on disk.
  base::Time checked_at;    // Last successful (200/304) check.
  std::string last_error;   // Empty if the last attempt succeeded.
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_TYPES_H_
