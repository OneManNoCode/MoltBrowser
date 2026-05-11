// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryStorage — SQLite-backed durable store for documents + chunks.
//
// Security
//   - DB file lives at ~/.moltbrowser/memory/memory.db with mode 0600.
//   - Every text field stored at rest is wrapped with OSCrypt, which
//     uses the macOS Keychain (or Linux/Windows analogue) to derive a
//     per-user key. URLs are kept plaintext because they're required
//     as primary indexing keys and are already exposed in profile
//     History anyway.
//   - On open, if OSCrypt is unavailable (rare, mostly first-launch
//     race) the storage falls back to in-memory mode and refuses to
//     persist — silent corruption is worse than data loss.
//   - The on-disk schema versions itself. A schema upgrade that can't
//     be migrated wipes the table rather than crash; the user can
//     re-index by re-visiting pages.
//
// Threading: all methods may block on disk I/O. Callers MUST invoke
// from a worker sequence; MemoryService bounces all storage calls
// onto base::ThreadPool with MayBlock().

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_STORAGE_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_STORAGE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "sql/database.h"

namespace molt_ai {
namespace memory {

class MemoryStorage {
 public:
  MemoryStorage();
  ~MemoryStorage();

  MemoryStorage(const MemoryStorage&) = delete;
  MemoryStorage& operator=(const MemoryStorage&) = delete;

  // Open the database at the user's profile-scoped memory dir. Returns
  // true on success. Idempotent: safe to call repeatedly.
  bool Open(const base::FilePath& memory_dir);

  // Returns the doc_id of any existing row whose URL matches AND whose
  // content_hash matches |content_hash|. 0 if not found. Lets the
  // recorder skip re-embedding when a user revisits an unchanged page.
  int64_t FindUnchangedDoc(const std::string& url,
                            const std::vector<uint8_t>& content_hash);

  // Insert one document + its chunks atomically. On return |doc| has
  // doc_id populated; each chunk also has chunk_id populated.
  bool InsertDocumentWithChunks(Document* doc, std::vector<Chunk>* chunks);

  // Return up to |limit| most-recently-visited documents.
  std::vector<Document> ListRecentDocuments(int limit);

  // Look up one document by id. Returns std::nullopt if missing.
  std::optional<Document> GetDocument(int64_t doc_id);

  // Return a chunk's stored plaintext + metadata. Used to build
  // snippets for QueryHit display.
  std::optional<Chunk> GetChunk(int64_t chunk_id);

  // Delete one document + its chunks.
  bool DeleteDocument(int64_t doc_id);

  // Delete every document whose URL host matches |domain|.
  int DeleteByDomain(const std::string& domain);

  // Wipe everything.
  bool Clear();

  // Load every embedding (chunk_id, doc_id, vector) into |out|. Used
  // at startup to populate the in-RAM MemoryIndex. Cheap because
  // embeddings are tiny (kEmbeddingDim floats each).
  bool LoadAllEmbeddings(
      std::vector<std::tuple<int64_t, int64_t, std::vector<float>>>* out);

  // Counters for the molt://memory/ overview panel.
  int CountDocuments();
  int CountChunks();

 private:
  bool EnsureSchema();
  // Helpers that wrap OSCrypt. Encrypt returns "" on failure (caller
  // checks); Decrypt returns the plaintext or "" if the ciphertext
  // can't be unwrapped (e.g. a different user copied the .db file).
  std::string Encrypt(const std::string& plaintext) const;
  std::string Decrypt(const std::string& ciphertext) const;

  std::unique_ptr<sql::Database> db_;
  base::FilePath db_path_;
  bool open_ = false;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_STORAGE_H_
