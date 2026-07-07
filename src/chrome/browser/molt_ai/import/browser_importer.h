// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// BrowserImporter — generalized read/parse/decrypt CORE for importing a user's
// own local browser profile (bookmarks + passwords) across the Chromium family
// (Chrome, Chromium, Edge, Brave, Opera, Vivaldi) plus Firefox and Safari, on
// macOS / Windows / Linux.
//
// This is a pure DATA layer for the "Import from another browser" migration
// feature. It ONLY reads the user's own local profiles; it never writes to any
// store and performs no UI. The one user-consent point on macOS is the Keychain
// prompt that a Safe Storage key read triggers (Chromium family) — that is
// expected and intentional. On Safari, Full Disk Access (TCC) is required.
//
// Firefox and Safari bookmark reading is implemented by import_internal:: hooks
// (firefox_safari_reader.cc); this file dispatches to them.

#ifndef CHROME_BROWSER_MOLT_AI_IMPORT_BROWSER_IMPORTER_H_
#define CHROME_BROWSER_MOLT_AI_IMPORT_BROWSER_IMPORTER_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "url/gurl.h"

namespace molt_ai {

// Identifies a source browser. Chromium-family entries share the same profile
// layout + os_crypt decryption scheme (parameterized per-browser); Firefox and
// Safari use their own readers.
enum class BrowserId {
  kChrome,
  kChromium,
  kEdge,
  kBrave,
  kOpera,
  kVivaldi,
  kFirefox,
  kSafari,
};

// A single bookmark harvested from a source browser.
struct BrowserBookmark {
  std::u16string title;
  GURL url;
  // Folder path from the tree root, folder names joined by '/'
  // (e.g. "Bookmarks Bar/Tech"). "" == the bar root itself.
  std::string folder_path;
};

// A single saved credential harvested from a source browser's login store.
struct BrowserCredential {
  std::string signon_realm;
  GURL url;
  std::u16string username;
  std::u16string password;
};

// Result of a single ReadProfile() call. All failure modes are reported through
// these fields — the importer never crashes on bad input.
struct BrowserImportData {
  BrowserId source_id = BrowserId::kChrome;
  std::string display_name;

  // True iff a profile with a bookmarks store was located.
  bool profile_found = false;

  std::vector<BrowserBookmark> bookmarks;
  std::vector<BrowserCredential> credentials;

  int password_total = 0;             // rows seen in the logins table
  int password_decrypt_failures = 0;  // rows we could not decrypt

  // True iff the Safe Storage keychain read was denied / failed (mac). When
  // set, bookmarks are still returned but no passwords.
  bool keychain_denied = false;

  // True iff password import is supported for this browser on this platform
  // AND include_passwords was requested.
  bool passwords_supported = false;

  // Safari: reading its bookmarks requires Full Disk Access (TCC). Set when the
  // read was blocked by the sandbox and the user must grant access.
  bool needs_full_disk_access = false;

  // Fatal error (e.g. profile missing / unsupported). Empty == ok.
  std::string error;
};

// A browser detected as installed on this machine (stat-only discovery).
struct DetectedBrowser {
  BrowserId id;
  std::string display_name;
  base::FilePath profile_path;
  bool has_bookmarks = false;
  bool has_passwords_store = false;
};

class BrowserImporter {
 public:
  // Stat-only discovery of installed, importable browsers. Performs NO keychain
  // access and no decryption — safe to call on the UI thread cheaply, though
  // it does touch the filesystem so prefer a worker. Cross-platform.
  static std::vector<DetectedBrowser> DetectInstalled();

  // BLOCKING — the caller MUST run this off the UI thread. Reads the given
  // browser's default profile (bookmarks, and passwords when supported).
  //
  // `include_passwords` gates the login-store read and Keychain access
  // entirely; when false, only bookmarks are returned and no Keychain prompt
  // is shown.
  static BrowserImportData ReadProfile(BrowserId id, bool include_passwords);
};

// Hooks implemented in firefox_safari_reader.cc (Agent B), dispatched from
// BrowserImporter::ReadProfile().
namespace import_internal {

std::vector<BrowserBookmark> ReadFirefoxBookmarks(
    const base::FilePath& profile_dir);

struct SafariResult {
  std::vector<BrowserBookmark> bookmarks;
  bool needs_full_disk_access = false;
  bool ok = false;
};

SafariResult ReadSafariBookmarks();

}  // namespace import_internal

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_IMPORT_BROWSER_IMPORTER_H_
