// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltProfileStore — encrypted on-device profile for the Form Filler
// Agent. Stores a single JSON blob (name, email, phone, address fields)
// at ~/.moltbrowser/profile.enc encrypted via OSCrypt (uses the OS
// keychain on macOS / DPAPI on Windows / libsecret on Linux). The blob
// is small so we read/write the whole thing on every change; no
// streaming or partial-update path is needed.
//
// A profile.enc that exists but can't be read (the OS keyring is locked
// or was reset, the file came from another machine, or it is corrupt) is
// never overwritten by a plain Save(): the save is refused and the file is
// left as it is, so the profile comes back once the keyring is unlocked.
// Replacing it takes an explicit user choice
// (SaveKeepingUnreadableAsBackup), and even then the old file is renamed
// to a timestamped backup next to it, never deleted.
//
// All methods are UI-thread-safe but perform synchronous file I/O —
// wrap calls in ScopedAllowBlockingForMolt or post them to a worker
// sequence if the caller is hot on the UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_PROFILE_MOLT_PROFILE_STORE_H_
#define CHROME_BROWSER_MOLT_AI_PROFILE_MOLT_PROFILE_STORE_H_

#include <string>

#include "base/files/file_path.h"
#include "base/values.h"

namespace molt_ai {
namespace profile {

// Field keys used inside the JSON blob. Centralized so the WebUI
// editor, the form-fill matcher, and the store all agree.
inline constexpr char kFieldFullName[]    = "full_name";
inline constexpr char kFieldFirstName[]   = "first_name";
inline constexpr char kFieldLastName[]    = "last_name";
inline constexpr char kFieldEmail[]       = "email";
inline constexpr char kFieldPhone[]       = "phone";
inline constexpr char kFieldAddressLine1[] = "address_line1";
inline constexpr char kFieldAddressLine2[] = "address_line2";
inline constexpr char kFieldCity[]        = "city";
inline constexpr char kFieldState[]       = "state";
inline constexpr char kFieldZip[]         = "zip";
inline constexpr char kFieldCountry[]     = "country";
inline constexpr char kFieldCompany[]     = "company";
inline constexpr char kFieldJobTitle[]    = "job_title";
inline constexpr char kFieldWebsite[]     = "website";

// What reading profile.enc found.
enum class ProfileReadStatus {
  kOk,      // Read, decrypted and parsed.
  kAbsent,  // No profile yet: no file, or an empty one.
  // A profile exists but couldn't be read: the OS keyring is locked or
  // unavailable, the key changed, or the file is corrupt.
  kUnreadable,
};

struct ProfileLoadResult {
  ProfileLoadResult();
  ProfileLoadResult(ProfileLoadResult&&);
  ProfileLoadResult& operator=(ProfileLoadResult&&);
  ~ProfileLoadResult();

  ProfileReadStatus status = ProfileReadStatus::kAbsent;
  base::DictValue profile;  // Empty unless status is kOk.
};

enum class ProfileSaveResult {
  kSaved,
  // No real OS key store behind OSCrypt (molt_ai::IsSecureStorageAvailable()
  // is false); nothing written.
  kNoSecureStorage,
  // profile.enc exists but couldn't be read; it was left untouched.
  kExistingUnreadable,
  // Directory, encryption or write failure; profile.enc left untouched.
  kFailed,
};

class MoltProfileStore {
 public:
  MoltProfileStore();
  ~MoltProfileStore();

  MoltProfileStore(const MoltProfileStore&) = delete;
  MoltProfileStore& operator=(const MoltProfileStore&) = delete;

  // Returns the current profile dict. Empty dict if the file doesn't
  // exist or fails to decrypt (corrupt file, keychain unavailable). Use
  // LoadWithStatus() to tell those apart.
  base::DictValue Load() const;

  // The profile, and whether it was absent or unreadable.
  ProfileLoadResult LoadWithStatus() const;

  // Writes |dict| as the new profile, atomically (.tmp + rename). Refuses
  // (kNoSecureStorage) when there is no real OS key store behind OSCrypt —
  // see molt_ai::IsSecureStorageAvailable() — and refuses
  // (kExistingUnreadable) to replace a profile.enc it can't read.
  ProfileSaveResult Save(const base::DictValue& dict);

  // Like Save(), but when profile.enc exists and can't be read, it is
  // first renamed to profile.enc.unreadable-<UTC time> in the same
  // directory and |*backup_path| is set to that file. Only for an explicit
  // user choice to start over. |*backup_path| is empty when nothing was
  // moved.
  ProfileSaveResult SaveKeepingUnreadableAsBackup(const base::DictValue& dict,
                                                  base::FilePath* backup_path);

  // Wipes the profile file. Returns true if the file is gone after
  // the call (whether or not it existed before).
  bool Clear();

  // Test seam: override the root directory (defaults to ~/.moltbrowser).
  void SetRootForTesting(const base::FilePath& root);

 private:
  base::FilePath GetProfilePath() const;
  ProfileSaveResult SaveImpl(const base::DictValue& dict,
                             bool backup_unreadable,
                             base::FilePath* backup_path);

  base::FilePath root_override_;
};

}  // namespace profile
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_PROFILE_MOLT_PROFILE_STORE_H_
