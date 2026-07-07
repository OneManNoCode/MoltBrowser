// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/import/chrome_importer.h"

#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include <Security/Security.h>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#include "crypto/aes_cbc.h"
#include "crypto/kdf.h"
#include "crypto/subtle_passkey.h"
#include "sql/database.h"
#include "sql/statement.h"
#endif  // BUILDFLAG(IS_MAC)

namespace molt_ai {

namespace {

// ---------------------------------------------------------------------------
// Profile location
// ---------------------------------------------------------------------------

// Returns the path to the user's Chrome "Default" profile directory, or an
// empty FilePath if it (or its Bookmarks file) cannot be located.
// This phase targets macOS; the path is guarded per platform.
base::FilePath GetChromeDefaultProfileDir() {
#if BUILDFLAG(IS_MAC)
  base::FilePath home = base::GetHomeDir();
  if (home.empty()) {
    return base::FilePath();
  }
  return home.Append("Library")
      .Append("Application Support")
      .Append("Google")
      .Append("Chrome")
      .Append("Default");
#else
  // Other platforms are not supported in this phase. The tree structure of
  // the Bookmarks JSON is identical cross-platform, so only the base path
  // needs to change when this is extended.
  return base::FilePath();
#endif
}

// ---------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------

// Recursively walks a Chrome bookmark tree node. `folder_path` is the '/'-
// joined path of ancestor folder names ("" at a root). URL nodes are appended
// to `out`; malformed nodes are skipped, never fatal.
void WalkBookmarkNode(const base::DictValue& node,
                      const std::string& folder_path,
                      std::vector<ChromeBookmark>& out) {
  const std::string* type = node.FindString("type");
  if (!type) {
    return;
  }

  if (*type == "url") {
    const std::string* url = node.FindString("url");
    if (!url) {
      return;
    }
    GURL gurl(*url);
    if (!gurl.is_valid()) {
      return;
    }
    const std::string* title = node.FindString("name");
    ChromeBookmark bookmark;
    bookmark.title = title ? base::UTF8ToUTF16(*title) : std::u16string();
    bookmark.url = std::move(gurl);
    bookmark.folder_path = folder_path;
    out.push_back(std::move(bookmark));
    return;
  }

  if (*type == "folder") {
    // Extend the folder path with this folder's own name, unless this is the
    // top-level container itself (handled by the caller passing "").
    std::string child_path = folder_path;
    const std::string* name = node.FindString("name");
    if (name && !name->empty()) {
      if (!child_path.empty()) {
        child_path.append("/");
      }
      child_path.append(*name);
    }

    const base::ListValue* children = node.FindList("children");
    if (!children) {
      return;
    }
    for (const base::Value& child : *children) {
      if (child.is_dict()) {
        WalkBookmarkNode(child.GetDict(), child_path, out);
      }
    }
  }
}

// Walks one named top-level root (bookmark_bar / other / synced). The root's
// own display name is used as the base folder path (e.g. "Bookmarks Bar"), so
// URLs sitting directly under it get that as their folder_path.
void WalkBookmarkRoot(const base::DictValue& roots,
                      std::string_view root_key,
                      std::vector<ChromeBookmark>& out) {
  const base::DictValue* root = roots.FindDict(root_key);
  if (!root) {
    return;
  }
  const std::string* name = root->FindString("name");
  std::string base_path = name ? *name : std::string();

  const base::ListValue* children = root->FindList("children");
  if (!children) {
    return;
  }
  for (const base::Value& child : *children) {
    if (child.is_dict()) {
      WalkBookmarkNode(child.GetDict(), base_path, out);
    }
  }
}

void ReadBookmarks(const base::FilePath& profile_dir,
                   std::vector<ChromeBookmark>& out) {
  base::FilePath bookmarks_path = profile_dir.Append("Bookmarks");
  std::string json;
  if (!base::ReadFileToString(bookmarks_path, &json)) {
    return;
  }

  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;  // Malformed JSON — skip, never crash.
  }

  const base::DictValue* roots = parsed->GetDict().FindDict("roots");
  if (!roots) {
    return;
  }

  WalkBookmarkRoot(*roots, "bookmark_bar", out);
  WalkBookmarkRoot(*roots, "other", out);
  WalkBookmarkRoot(*roots, "synced", out);
}

// ---------------------------------------------------------------------------
// Passwords (macOS only)
// ---------------------------------------------------------------------------

#if BUILDFLAG(IS_MAC)

// Chrome's mac Safe Storage keychain item coordinates. These are the Google
// Chrome (branded) values — deliberately NOT MoltBrowser's own item.
constexpr char kChromeKeychainService[] = "Chrome Safe Storage";
constexpr char kChromeKeychainAccount[] = "Chrome";

// Chrome mac key-derivation constants (must match Chrome exactly).
constexpr std::array<uint8_t, 9> kSalt = {'s', 'a', 'l', 't', 'y',
                                          's', 'a', 'l', 't'};
constexpr size_t kIterations = 1003;
constexpr size_t kDerivedKeySize = 16;  // AES-128

// AES-CBC IV Chrome uses for password_value: 16 spaces.
constexpr std::array<uint8_t, crypto::aes_cbc::kBlockSize> kIv = {
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
};

constexpr char kObfuscationPrefixV10[] = "v10";

// Reads Chrome's Safe Storage password from the login Keychain via the
// Security-framework C API. This triggers the user's Keychain prompt —
// expected and intentional (the consent point). Returns the raw keychain
// password bytes, or nullopt if not found / access denied. Never logs the
// key material.
std::optional<std::string> ReadChromeKeychainPassword() {
  base::apple::ScopedCFTypeRef<CFStringRef> service(CFStringCreateWithCString(
      nullptr, kChromeKeychainService, kCFStringEncodingUTF8));
  base::apple::ScopedCFTypeRef<CFStringRef> account(CFStringCreateWithCString(
      nullptr, kChromeKeychainAccount, kCFStringEncodingUTF8));
  if (!service || !account) {
    return std::nullopt;
  }

  const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount,
                        kSecReturnData, kSecMatchLimit};
  const void* values[] = {kSecClassGenericPassword, service.get(),
                          account.get(), kCFBooleanTrue, kSecMatchLimitOne};
  base::apple::ScopedCFTypeRef<CFDictionaryRef> query(CFDictionaryCreate(
      nullptr, keys, values, std::size(keys), &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks));
  if (!query) {
    return std::nullopt;
  }

  CFTypeRef result = nullptr;
  OSStatus status = SecItemCopyMatching(query.get(), &result);
  base::apple::ScopedCFTypeRef<CFTypeRef> scoped_result(result);
  if (status != errSecSuccess || !scoped_result) {
    // errSecItemNotFound (Chrome not installed / never set a key), user
    // denial, or any other failure all land here.
    return std::nullopt;
  }

  CFDataRef data = base::apple::CFCast<CFDataRef>(scoped_result.get());
  if (!data) {
    return std::nullopt;
  }
  base::span<const uint8_t> bytes(CFDataGetBytePtr(data),
                                  static_cast<size_t>(CFDataGetLength(data)));
  return std::string(base::as_string_view(bytes));
}

// Decrypts one Chrome password_value blob using the derived key. Returns the
// UTF-8 plaintext, or nullopt on failure. Never logs plaintext.
std::optional<std::string> DecryptPassword(
    base::span<const uint8_t> derived_key,
    const std::string& encrypted) {
  // Legacy plaintext (no v10 prefix): rare, treat bytes as the password.
  std::string_view prefix(kObfuscationPrefixV10);
  if (encrypted.size() < prefix.size() ||
      std::string_view(encrypted).substr(0, prefix.size()) != prefix) {
    if (encrypted.empty()) {
      return std::nullopt;
    }
    return encrypted;
  }

  base::span<const uint8_t> ciphertext =
      base::as_byte_span(encrypted).subspan(prefix.size());
  if (ciphertext.empty()) {
    return std::nullopt;
  }

  std::optional<std::vector<uint8_t>> plain =
      crypto::aes_cbc::Decrypt(derived_key, kIv, ciphertext);
  if (!plain) {
    return std::nullopt;
  }
  return std::string(base::as_string_view(*plain));
}

// Reads + decrypts the Login Data DB. Populates credentials and the counters.
// Any per-row failure is counted, not fatal. Sets keychain_denied if the
// Chrome Safe Storage key cannot be read.
void ReadPasswords(const base::FilePath& profile_dir, ChromeImportData& data) {
  data.passwords_supported = true;

  base::FilePath login_data = profile_dir.Append("Login Data");
  if (!base::PathExists(login_data)) {
    return;  // No saved passwords; not an error.
  }

  // Chrome may hold a lock on the live DB — work on a temp copy.
  base::FilePath temp_db;
  if (!base::CreateTemporaryFile(&temp_db)) {
    return;
  }
  if (!base::CopyFile(login_data, temp_db)) {
    base::DeleteFile(temp_db);
    return;
  }

  // Get Chrome's key BEFORE touching the DB so a keychain denial short-
  // circuits cleanly (bookmarks already collected by the caller).
  std::optional<std::string> keychain_password = ReadChromeKeychainPassword();
  if (!keychain_password || keychain_password->empty()) {
    data.keychain_denied = true;
    base::DeleteFile(temp_db);
    return;
  }

  std::array<uint8_t, kDerivedKeySize> derived_key;
  crypto::kdf::DeriveKeyPbkdf2HmacSha1(
      {.iterations = kIterations}, base::as_byte_span(*keychain_password),
      kSalt, derived_key,
      // NOTE: SubtlePassKey's constructor is friend-gated and molt_ai is not
      // on that list; ForTesting() is the only public accessor. The PBKDF2
      // parameters (not the passkey) are what determine correctness. See the
      // deviations note — production should add a molt_ai friend factory.
      crypto::SubtlePassKey::ForTesting());

  {
    sql::Database db(sql::Database::Tag("ChromeImporter"));
    if (!db.Open(temp_db)) {
      base::DeleteFile(temp_db);
      return;
    }

    sql::Statement s(db.GetUniqueStatement(
        "SELECT origin_url, username_value, password_value, signon_realm "
        "FROM logins"));

    while (s.Step()) {
      data.password_total++;

      std::string origin_url = s.ColumnString(0);
      std::u16string username = s.ColumnString16(1);
      std::string encrypted = s.ColumnString(2);
      std::string signon_realm = s.ColumnString(3);

      std::optional<std::string> decrypted =
          DecryptPassword(derived_key, encrypted);
      if (!decrypted) {
        data.password_decrypt_failures++;
        continue;
      }

      ChromeCredential cred;
      cred.signon_realm = std::move(signon_realm);
      cred.url = GURL(origin_url);
      cred.username = std::move(username);
      cred.password = base::UTF8ToUTF16(*decrypted);
      data.credentials.push_back(std::move(cred));
    }
    // db goes out of scope (closed) before we delete the temp file.
  }

  base::DeleteFile(temp_db);
}

#endif  // BUILDFLAG(IS_MAC)

}  // namespace

// static
ChromeImportData ChromeImporter::ReadDefaultProfile(bool include_passwords) {
  ChromeImportData data;

  base::FilePath profile_dir = GetChromeDefaultProfileDir();
  if (profile_dir.empty()) {
    data.error = "Chrome profile location unavailable on this platform.";
    return data;
  }

  // A missing "Bookmarks" file is our signal that no Chrome profile exists.
  base::FilePath bookmarks_path = profile_dir.Append("Bookmarks");
  if (!base::PathExists(bookmarks_path)) {
    data.chrome_profile_found = false;
    return data;
  }
  data.chrome_profile_found = true;

  ReadBookmarks(profile_dir, data.bookmarks);

  if (include_passwords) {
#if BUILDFLAG(IS_MAC)
    ReadPasswords(profile_dir, data);
#else
    data.passwords_supported = false;
#endif
  }

  return data;
}

}  // namespace molt_ai
