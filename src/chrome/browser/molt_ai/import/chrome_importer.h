// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// BACK-COMPAT SHIM. The Chrome-only importer has been generalized into
// molt_ai::BrowserImporter (browser_importer.h). This header preserves the
// original ChromeImporter / ChromeImportData / ChromeBookmark / ChromeCredential
// names so callers that predate the migration keep compiling. New code should
// use browser_importer.h directly.

#ifndef CHROME_BROWSER_MOLT_AI_IMPORT_CHROME_IMPORTER_H_
#define CHROME_BROWSER_MOLT_AI_IMPORT_CHROME_IMPORTER_H_

#include <string>
#include <vector>

#include "chrome/browser/molt_ai/import/browser_importer.h"

namespace molt_ai {

// Old struct names now alias the generalized ones.
using ChromeBookmark = BrowserBookmark;
using ChromeCredential = BrowserCredential;

// The original ChromeImportData exposed `chrome_profile_found` (renamed to
// `profile_found` in BrowserImportData). Keep a distinct struct that mirrors the
// generalized data with the historical field name so old readers still compile.
struct ChromeImportData {
  bool chrome_profile_found = false;

  std::vector<ChromeBookmark> bookmarks;
  std::vector<ChromeCredential> credentials;

  int password_total = 0;
  int password_decrypt_failures = 0;

  bool keychain_denied = false;
  bool passwords_supported = false;

  std::string error;
};

// Thin wrapper preserving the original entry point. Delegates to
// BrowserImporter::ReadProfile(kChrome, ...) and adapts the result struct.
class ChromeImporter {
 public:
  // BLOCKING — caller MUST run off the UI thread. Reads the user's default
  // Google Chrome profile (bookmarks + passwords when supported).
  static ChromeImportData ReadDefaultProfile(bool include_passwords) {
    BrowserImportData d =
        BrowserImporter::ReadProfile(BrowserId::kChrome, include_passwords);
    ChromeImportData out;
    out.chrome_profile_found = d.profile_found;
    out.bookmarks = std::move(d.bookmarks);
    out.credentials = std::move(d.credentials);
    out.password_total = d.password_total;
    out.password_decrypt_failures = d.password_decrypt_failures;
    out.keychain_denied = d.keychain_denied;
    out.passwords_supported = d.passwords_supported;
    out.error = std::move(d.error);
    return out;
  }
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_IMPORT_CHROME_IMPORTER_H_
