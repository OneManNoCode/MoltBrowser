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

class MoltProfileStore {
 public:
  MoltProfileStore();
  ~MoltProfileStore();

  MoltProfileStore(const MoltProfileStore&) = delete;
  MoltProfileStore& operator=(const MoltProfileStore&) = delete;

  // Returns the current profile dict. Empty dict if the file doesn't
  // exist or fails to decrypt (corrupt file, keychain unavailable).
  base::DictValue Load() const;

  // Writes |dict| as the new profile, atomically (.tmp + rename).
  // Returns true on success.
  bool Save(const base::DictValue& dict);

  // Wipes the profile file. Returns true if the file is gone after
  // the call (whether or not it existed before).
  bool Clear();

  // Test seam: override the root directory (defaults to ~/.moltbrowser).
  void SetRootForTesting(const base::FilePath& root);

 private:
  base::FilePath GetProfilePath() const;

  base::FilePath root_override_;
};

}  // namespace profile
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_PROFILE_MOLT_PROFILE_STORE_H_
