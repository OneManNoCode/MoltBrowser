// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ChromeImporter — read/parse/decrypt CORE for importing a user's own
// Google Chrome profile (bookmarks + passwords) on macOS.
//
// This is a pure DATA layer for the "Import from Chrome" browser-migration
// feature (analogous to Edge/Brave/Arc). It ONLY reads the user's own local
// Chrome profile; it never writes to any store and performs no UI. The one
// user-consent point is the macOS Keychain prompt that Chrome's Safe Storage
// key read triggers — that is expected and intentional.

#ifndef CHROME_BROWSER_MOLT_AI_IMPORT_CHROME_IMPORTER_H_
#define CHROME_BROWSER_MOLT_AI_IMPORT_CHROME_IMPORTER_H_

#include <string>
#include <vector>

#include "url/gurl.h"

namespace molt_ai {

// A single bookmark harvested from Chrome's "Bookmarks" JSON.
struct ChromeBookmark {
  std::u16string title;
  GURL url;
  // Folder path from the tree root, folder names joined by '/'
  // (e.g. "Bookmarks Bar/Tech"). "" == the bar root itself.
  std::string folder_path;
};

// A single saved credential harvested from Chrome's "Login Data" DB.
struct ChromeCredential {
  std::string signon_realm;
  GURL url;
  std::u16string username;
  std::u16string password;
};

// Result of a single ReadDefaultProfile() call. All failure modes are
// reported through these fields — the importer never crashes on bad input.
struct ChromeImportData {
  // True iff a Chrome Default profile with a "Bookmarks" file was located.
  bool chrome_profile_found = false;

  std::vector<ChromeBookmark> bookmarks;
  std::vector<ChromeCredential> credentials;

  int password_total = 0;             // rows seen in the logins table
  int password_decrypt_failures = 0;  // rows we could not decrypt

  // True iff the user denied / we failed the Chrome Safe Storage keychain
  // read. Bookmarks are still returned in this case.
  bool keychain_denied = false;

  // False on non-mac platforms in this phase (password import is mac-only).
  bool passwords_supported = false;

  // Fatal error (e.g. profile missing). Empty == ok.
  std::string error;
};

class ChromeImporter {
 public:
  // BLOCKING — the caller MUST run this off the UI thread. Reads
  // ~/Library/Application Support/Google/Chrome/Default/.
  //
  // `include_passwords` gates the "Login Data" read and Keychain access
  // entirely; when false, only bookmarks are returned and no Keychain
  // prompt is shown.
  //
  // Only the "Default" profile is read in this phase (multi-profile support
  // is a follow-up).
  static ChromeImportData ReadDefaultProfile(bool include_passwords);
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_IMPORT_CHROME_IMPORTER_H_
