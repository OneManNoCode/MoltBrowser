// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryStorage — SQLite-backed durable store for documents + chunks.
//
// Security
//   - DB file lives at <profile>/MoltMemory/memory.db with mode 0600.
//   - Every field derived from what the user read is encrypted with the
//     os_crypt_async Encryptor through molt_os_crypt (macOS Keychain /
//     Linux keyring, or Linux's hard-coded fallback key without one /
//     Windows DPAPI-derived key; files the old OSCrypt wrote still decrypt):
//     URL, title, chunk text AND the embedding vector (a hashed
//     bag-of-words vector leaks which words a page contained). Schema v1
//     stored URLs and embeddings in plaintext; Open() migrates such a DB in
//     place, recomputing the embeddings from the decrypted chunk text with
//     the current embedder, then VACUUMs so the old plaintext doesn't
//     linger in free pages.
//   - URL lookups (revisit de-duplication) use an HMAC-SHA256 of the URL
//     keyed with a random per-database secret that is itself stored
//     encrypted in the meta table, so the DB can't be probed with a
//     dictionary of known URLs.
//   - If encryption fails, writes fail rather than persisting plaintext, and
//     a migration that can't encrypt is rolled back (the DB stays at v1 and
//     Open() fails — memory is unavailable for the session, nothing lost).
//     Open() before the Encryptor is ready fails the same way, so
//     MemoryService only opens the DB once molt_os_crypt is ready.
//   - A DB whose compatible version is newer than this code is wiped
//     rather than misread.
//
// Vector spaces (docs/design/SEMANTIC_SEARCH.md)
//   - Every stored vector belongs to the "committed" space (meta
//     embedder_space, e.g. "hashing-v3/256" or the on-device model's
//     "<model>@<sha>/256"). A chunk stored while the committed embedder is
//     the asynchronous on-device model has no vector yet (NULL, "pending")
//     until the model embeds it.
//   - Switching to an asynchronous embedder builds its vectors in
//     chunk_vectors_next (meta embedder_space_next), in the background,
//     from the decrypted chunk text; CommitMigration() moves them into
//     place in one transaction once every chunk has one. Switching to a
//     synchronous embedder re-embeds everything at once
//     (ReembedAllAndCommit()). Vectors of two spaces are never mixed.
//   - embedder_version (the pre-space key) is kHashingEmbedderVersion only
//     while the committed space is HashingEmbedder's, else 0: a downgraded
//     MoltBrowser that predates spaces then re-embeds with hashing on open
//     and sets it back, which Open() recognizes.
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
#include "base/strings/cstring_view.h"
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
  // schema if needed. |embedder| (synchronous, not retained) recomputes
  // vectors during a migration, after a HashingEmbedder-version bump, and
  // for a new DB (whose committed space becomes its space). Returns true on
  // success. Idempotent: safe to call repeatedly.
  bool Open(const base::FilePath& memory_dir, const MemoryEmbedder& embedder);

  // Returns the doc_id of any existing row whose URL matches AND whose
  // content_hash matches |content_hash|. 0 if not found. Lets the
  // recorder skip re-embedding when a user revisits an unchanged page.
  int64_t FindUnchangedDoc(const std::string& url,
                           const std::vector<uint8_t>& content_hash);

  // Insert one document + its chunks atomically. On return |doc| has
  // doc_id populated; each chunk also has chunk_id populated. A chunk with
  // an empty embedding is stored pending (no vector yet).
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

  // ---- Vector spaces (see the header comment) ----

  // A chunk's plaintext, to (re-)embed it.
  struct ChunkText {
    int64_t chunk_id = 0;
    int64_t doc_id = 0;
    std::string text;
  };

  // The space of chunks.embedding, and the one being built ("" = none).
  // Empty before Open().
  const std::string& committed_space() const { return committed_space_; }
  const std::string& next_space() const { return next_space_; }

  // Re-embeds every chunk with |embedder| (synchronous) in one transaction
  // and commits its space; drops any migration. On failure nothing changes.
  bool ReembedAllAndCommit(const MemoryEmbedder& embedder);

  // Starts building |space_key| (discarding vectors of another next space).
  bool BeginMigration(const std::string& space_key);
  // Drops the next space and its vectors.
  bool AbortMigration();
  // Up to |limit| chunks with id > |after_chunk_id|, oldest first, that
  // have no vector in the next space yet.
  bool ChunksMissingNextVector(int64_t after_chunk_id,
                               size_t limit,
                               std::vector<ChunkText>* out);
  // -1 on error (also for CountPendingChunks()).
  int CountChunksMissingNextVector();
  // Stores next-space vectors (kEmbeddingDim floats each) for the chunks
  // that still exist.
  bool StoreNextVectors(const std::vector<EmbeddingRow>& rows);
  // Once every chunk has a next-space vector: moves them into
  // chunks.embedding and commits the next space, in one transaction.
  // Returns false (nothing changed) if any is missing.
  bool CommitMigration();

  // Up to |limit| pending chunks (no vector in the committed space) with
  // id > |after_chunk_id|, oldest first.
  bool PendingChunks(int64_t after_chunk_id,
                     size_t limit,
                     std::vector<ChunkText>* out);
  int CountPendingChunks();
  // Stores committed-space vectors for pending chunks; returns the rows
  // actually stored (a chunk deleted meanwhile is skipped).
  std::vector<EmbeddingRow> StorePendingVectors(
      const std::vector<EmbeddingRow>& rows);

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
  // chunk_vectors_next and the pending-chunk index (idempotent).
  bool CreateSpaceTables();
  // Settles committed_space_ / next_space_ from the meta table, re-embedding
  // an outdated hashing space with |embedder|.
  bool LoadSpaces(sql::MetaTable* meta, const MemoryEmbedder& embedder);
  // Records |space_key| as committed (in a caller's transaction).
  bool SetCommittedSpace(sql::MetaTable* meta, const std::string& space_key);
  // `sql` is a string literal (GetUniqueStatement() takes a cstring_view).
  bool ReadChunkTexts(base::cstring_view sql,
                      int64_t after_chunk_id,
                      size_t limit,
                      std::vector<ChunkText>* out);
  bool LoadOrCreateUrlKey(sql::MetaTable* meta);
  std::vector<uint8_t> UrlHash(const std::string& url) const;
  std::vector<uint8_t> KeyedContentHash(
      const std::vector<uint8_t>& content_hash) const;

  // Helpers around molt_os_crypt. Encrypt returns false on failure (callers
  // must then refuse to persist); Decrypt returns the plaintext or "" if the
  // ciphertext can't be unwrapped (e.g. a different user copied the file).
  bool Encrypt(const std::string& plaintext, std::string* ciphertext) const;
  std::string Decrypt(const std::string& ciphertext) const;

  std::unique_ptr<sql::Database> db_;
  // Initialized by EnsureSchema(); used afterwards for the space keys.
  std::unique_ptr<sql::MetaTable> meta_;
  base::FilePath db_path_;
  std::vector<uint8_t> url_key_;  // HMAC key for UrlHash(); 32 bytes
  bool open_ = false;
  std::string committed_space_;
  std::string next_space_;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_STORAGE_H_
