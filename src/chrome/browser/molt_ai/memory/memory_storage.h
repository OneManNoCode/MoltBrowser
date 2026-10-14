// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryStorage — SQLite-backed durable store for documents + chunks.
//
// Security
//   - DB file lives at <profile>/MoltMemory/memory.db with mode 0600.
//   - Every field derived from what the user read is wrapped with OSCrypt
//     (macOS Keychain / Linux keyring / Windows DPAPI-derived key): URL,
//     title, chunk text AND the embedding vector (a hashed bag-of-words
//     vector leaks which words a page contained). Schema v1 stored URLs
//     and embeddings in plaintext; Open() migrates such a DB in place,
//     recomputing the embeddings from the decrypted chunk text with the
//     current embedder, then VACUUMs so the old plaintext doesn't linger
//     in free pages.
//   - URL lookups (revisit de-duplication) use an HMAC-SHA256 of the URL
//     keyed with a random per-database secret that is itself stored
//     OSCrypt-wrapped in the meta table, so the DB can't be probed with a
//     dictionary of known URLs.
//   - If OSCrypt fails, writes fail rather than persisting plaintext, and a
//     migration that can't encrypt is rolled back (the DB stays at v1 and
//     Open() fails — memory is unavailable for the session, nothing lost).
//   - A DB whose compatible version is newer than this code is wiped
//     rather than misread.
//
// Threading: all methods may block on disk I/O. Callers MUST invoke
// from a worker sequence; MemoryService confines all storage calls to
// its worker sequence.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_STORAGE_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_STORAGE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base/files/file_path.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "sql/database.h"

namespace sql {
class MetaTable;
}  // namespace sql

namespace molt_ai {
namespace memory {

class MemoryEmbedder;

class MemoryStorage {
 public:
  using EmbeddingRow = std::tuple<int64_t /*chunk_id*/, int64_t /*doc_id*/,
                                  std::vector<float>>;

  MemoryStorage();
  ~MemoryStorage();

  MemoryStorage(const MemoryStorage&) = delete;
  MemoryStorage& operator=(const MemoryStorage&) = delete;

  // Open the database at the profile-scoped memory dir, migrating an older
  // schema if needed. |embedder| (not retained) recomputes vectors during a
  // migration or after an embedder-version bump. Returns true on success.
  // Idempotent: safe to call repeatedly.
  bool Open(const base::FilePath& memory_dir, const MemoryEmbedder& embedder);

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

  // Return a chunk's stored plaintext + metadata (the embedding is left
  // empty — callers only need the text for snippets).
  std::optional<Chunk> GetChunk(int64_t chunk_id);

  // Delete one document + its chunks.
  bool DeleteDocument(int64_t doc_id);

  // Delete every document whose URL host matches |domain| (or is a
  // subdomain of it). Returns the deleted doc ids.
  std::vector<int64_t> DeleteByDomain(const std::string& domain);

  // Retention: delete every document visited before |cutoff_unix|.
  // Returns the deleted doc ids.
  std::vector<int64_t> DeleteDocumentsVisitedBefore(int64_t cutoff_unix);

  // Wipe everything.
  bool Clear();

  // Load the |limit| newest embeddings (by chunk_id), returned oldest-first
  // so they can be appended to the FIFO hot index in order. Older rows stay
  // on disk only — see MemoryIndex.
  bool LoadNewestEmbeddings(size_t limit, std::vector<EmbeddingRow>* out);

  // Counters for the molt://memory/ overview panel.
  int CountDocuments();
  int CountChunks();

  // Pure helpers, exposed for tests.
  static std::string PackEmbedding(const std::vector<float>& v);
  static std::vector<float> UnpackEmbedding(const std::string& blob);

 private:
  // |allow_wipe| guards against looping when even a freshly recreated DB
  // can't be initialized.
  bool EnsureSchema(const MemoryEmbedder& embedder, bool allow_wipe);
  bool CreateTablesV2();
  bool MigrateV1ToV2(sql::MetaTable* meta, const MemoryEmbedder& embedder);
  bool ReembedAllChunks(const MemoryEmbedder& embedder);
  bool LoadOrCreateUrlKey(sql::MetaTable* meta);
  std::vector<uint8_t> UrlHash(const std::string& url) const;
  std::vector<uint8_t> KeyedContentHash(
      const std::vector<uint8_t>& content_hash) const;

  // Helpers that wrap OSCrypt. Encrypt returns false on failure (callers
  // must then refuse to persist); Decrypt returns the plaintext or "" if the
  // ciphertext can't be unwrapped (e.g. a different user copied the file).
  bool Encrypt(const std::string& plaintext, std::string* ciphertext) const;
  std::string Decrypt(const std::string& ciphertext) const;

  std::unique_ptr<sql::Database> db_;
  base::FilePath db_path_;
  std::vector<uint8_t> url_key_;  // HMAC key for UrlHash(); 32 bytes
  bool open_ = false;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_STORAGE_H_
