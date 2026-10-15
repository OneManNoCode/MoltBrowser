// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ConversationStore — the AI chat's saved conversations (sidebar Recents),
// stored per profile and encrypted at rest.
//
//   <profile>/MoltAI/Conversations/<id>.mconv
//     = "MOLTCONV1\n" + OSCrypt(JSON {id, title, updated_at (ms since epoch),
//                                     messages: [{role, content}],
//                                     attachment?: {name, text, ...}})
//
// Previously conversations were plaintext JSON in ~/.moltbrowser/
// conversations, shared by every profile and written even from incognito.
// MigrateLegacy() moves those files (encrypting them) into the first
// profile that opens the chat, then deletes the plaintext copies.
// Off-the-record profiles must not use this store at all (the chat handler
// refuses before getting here).
//
// Lives in the memory target because that is where OSCrypt is visible to
// MoltBrowser code. All methods block on disk IO / OSCrypt: call them on a
// single sequenced worker (the chat handler's conversation task runner) so
// autosaves of one conversation stay ordered.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_CONVERSATION_STORE_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_CONVERSATION_STORE_H_

#include <cstddef>
#include <string>

#include "base/files/file_path.h"
#include "base/values.h"

namespace molt_ai {
namespace memory {

class ConversationStore {
 public:
  // Same on-disk cap as chat history export.
  static constexpr size_t kMaxConversationBytes = 16 << 20;

  // |profile_dir| is Profile::GetPath() of a regular (non-OTR) profile.
  explicit ConversationStore(const base::FilePath& profile_dir);
  ~ConversationStore();

  // Path-traversal defense: ids must match [a-zA-Z0-9_-]{1,64}.
  static bool IsValidId(const std::string& id);

  // The legacy shared plaintext directory (~/.moltbrowser/conversations).
  static base::FilePath LegacyDir();

  // Encrypts every legacy plaintext conversation in |legacy_dir| into this
  // store (an id already present here wins) and deletes the plaintext
  // file. Cheap no-op once the legacy dir is gone.
  void MigrateLegacy(const base::FilePath& legacy_dir);

  // Each returns the dict that resolves the page's promise:
  //   List   -> {success, conversations: [{id, title, updated_at,
  //                                         message_count}]} newest first
  //   Save   -> {success, error?}   |payload_json| is the page's history:
  //             a bare [{role, content}] array, or the v2 envelope
  //             {v: 2, messages: [...], attachment: {...}}
  //   Load   -> {success, id, title, history_json, error?}
  //   Delete -> {success, error?}
  base::DictValue List(size_t max_entries) const;
  base::DictValue Save(const std::string& id,
                       const std::string& title,
                       const std::string& payload_json) const;
  base::DictValue Load(const std::string& id) const;
  base::DictValue Delete(const std::string& id) const;

  const base::FilePath& dir() const { return dir_; }

 private:
  base::FilePath PathFor(const std::string& id) const;
  // Decrypts |path| into |json|. False if missing, too large, not ours or
  // undecryptable.
  bool ReadDecrypted(const base::FilePath& path, std::string* json) const;
  // Encrypts |json| and atomically replaces |path| (tmp + rename). Refuses
  // (returns false) rather than writing plaintext if OSCrypt fails.
  bool WriteEncrypted(const base::FilePath& path,
                      const std::string& json) const;

  base::FilePath dir_;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_CONVERSATION_STORE_H_
