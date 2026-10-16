// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_storage.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"  // for BUILDFLAG(IS_WIN) — explicit not transitive
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "crypto/hmac.h"
#include "crypto/random.h"
#include "sql/meta_table.h"
#include "sql/statement.h"
#include "sql/transaction.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

namespace {

// v1: url + embedding plaintext. v2: url, embedding and content hash
// protected (see header). v1 code cannot read a v2 DB, hence compat 2.
constexpr int kCurrentSchemaVersion = 2;
constexpr int kCompatibleVersionNumber = 2;

constexpr char kUrlKeyMetaKey[] = "url_hmac_key";
constexpr char kEmbedderVersionMetaKey[] = "embedder_version";
constexpr size_t kUrlKeyBytes = 32;

// Re-embedding works through the chunks table in pages so a large DB
// never has to be resident at once.
constexpr int kReembedBatch = 512;

// 4 bytes per float — checked at compile time so packing logic below
// never silently breaks on an exotic platform.
static_assert(sizeof(float) == 4,
              "MemoryStorage assumes 4-byte IEEE-754 floats");

}  // namespace

MemoryStorage::MemoryStorage() = default;

MemoryStorage::~MemoryStorage() {
  if (db_)
    db_->Close();
}

// static
std::string MemoryStorage::PackEmbedding(const std::vector<float>& v) {
  // Floats are little-endian on every platform Chromium runs on, so this
  // is a straight memcpy.
  std::string out(v.size() * sizeof(float), '\0');
  if (!v.empty()) {
    std::memcpy(out.data(), v.data(), out.size());
  }
  return out;
}

// static
std::vector<float> MemoryStorage::UnpackEmbedding(const std::string& blob) {
  std::vector<float> v;
  if (blob.size() != kEmbeddingDim * sizeof(float)) return v;
  v.resize(kEmbeddingDim);
  std::memcpy(v.data(), blob.data(), blob.size());
  return v;
}

bool MemoryStorage::Open(const base::FilePath& memory_dir,
                         const MemoryEmbedder& embedder) {
  if (open_) return true;
  if (!base::CreateDirectory(memory_dir)) {
    LOG(ERROR) << "[MoltMemory] cannot create dir " << memory_dir.value();
    return false;
  }
  db_path_ = memory_dir.Append(FILE_PATH_LITERAL("memory.db"));

  // (set_preload() is gone at Chromium 152. It was only a page-cache hint,
  // and this DB opens on a BEST_EFFORT worker, off the startup path.)
  db_ = std::make_unique<sql::Database>(sql::DatabaseOptions(),
                                        sql::Database::Tag("MoltMemory"));

  if (!db_->Open(db_path_)) {
    LOG(ERROR) << "[MoltMemory] cannot open " << db_path_.value();
    db_.reset();
    return false;
  }

  // Best-effort permissions tighten — the dir is per-user already,
  // this just removes group/other access on Unixy filesystems.
#if !BUILDFLAG(IS_WIN)
  base::SetPosixFilePermissions(db_path_, 0600);
#endif

  if (!EnsureSchema(embedder, /*allow_wipe=*/true)) {
    // EnsureSchema only returns false for a DB we must not touch further
    // this session (e.g. encryption unavailable mid-migration). The data is
    // left intact for the next launch.
    db_->Close();
    db_.reset();
    return false;
  }
  open_ = true;
  return true;
}

bool MemoryStorage::CreateTablesV2() {
  return db_->Execute(
             "CREATE TABLE IF NOT EXISTS documents ("
             " doc_id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " url TEXT NOT NULL DEFAULT '',"  // v1 column, always '' in v2
             " title_enc BLOB,"
             " visited_at INTEGER NOT NULL,"
             " word_count INTEGER NOT NULL DEFAULT 0,"
             " content_hash BLOB,"
             " url_enc BLOB,"
             " url_hash BLOB"
             ")") &&
         db_->Execute(
             "CREATE INDEX IF NOT EXISTS idx_documents_url_hash "
             "ON documents(url_hash)") &&
         db_->Execute(
             "CREATE INDEX IF NOT EXISTS idx_documents_visited "
             "ON documents(visited_at DESC)") &&
         db_->Execute(
             "CREATE TABLE IF NOT EXISTS chunks ("
             " chunk_id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " doc_id INTEGER NOT NULL,"
             " chunk_idx INTEGER NOT NULL,"
             " text_enc BLOB,"
             " embedding BLOB"  // Encrypt(packed floats) in v2
             ")") &&
         db_->Execute(
             "CREATE INDEX IF NOT EXISTS idx_chunks_doc "
             "ON chunks(doc_id)");
}

bool MemoryStorage::EnsureSchema(const MemoryEmbedder& embedder,
                                 bool allow_wipe) {
  const bool fresh = !db_->DoesTableExist("documents");

  auto wipe_and_recreate = [&]() -> bool {
    if (!allow_wipe) return false;
    LOG(ERROR) << "[MoltMemory] schema unusable; wiping";
    db_->Close();
    base::DeleteFile(db_path_);
    return db_->Open(db_path_) &&
           EnsureSchema(embedder, /*allow_wipe=*/false);
  };

  sql::MetaTable meta;
  if (!meta.Init(db_.get(), kCurrentSchemaVersion,
                 kCompatibleVersionNumber)) {
    return wipe_and_recreate();
  }
  if (meta.GetCompatibleVersionNumber() > kCurrentSchemaVersion) {
    LOG(WARNING) << "[MoltMemory] DB too new (compat="
                 << meta.GetCompatibleVersionNumber() << ")";
    return wipe_and_recreate();
  }

  if (fresh) {
    if (!CreateTablesV2() || !LoadOrCreateUrlKey(&meta) ||
        !meta.SetValue(kEmbedderVersionMetaKey,
                       static_cast<int64_t>(kHashingEmbedderVersion))) {
      return wipe_and_recreate();
    }
    return true;
  }

  if (meta.GetVersionNumber() < 2) {
    // v1 -> v2: encrypt URLs + embeddings in place. On failure (encryption
    // unavailable) the transaction rolls back and the v1 data is kept.
    if (!MigrateV1ToV2(&meta, embedder)) {
      LOG(ERROR) << "[MoltMemory] v1->v2 migration failed; memory disabled "
                    "this session, data left untouched";
      return false;
    }
    return true;
  }

  if (!LoadOrCreateUrlKey(&meta)) return false;

  int64_t embedder_version = 0;
  meta.GetValue(kEmbedderVersionMetaKey, &embedder_version);
  if (embedder_version < kHashingEmbedderVersion) {
    sql::Transaction tx(db_.get());
    if (!tx.Begin() || !ReembedAllChunks(embedder) ||
        !meta.SetValue(kEmbedderVersionMetaKey,
                       static_cast<int64_t>(kHashingEmbedderVersion)) ||
        !tx.Commit()) {
      // Don't run with old vectors: queries and new chunks would use the
      // new embedder, so old and new vectors would silently mix. The
      // transaction rolled back; memory is off this session and the
      // re-embed is retried on the next launch.
      LOG(ERROR) << "[MoltMemory] re-embedding for embedder v"
                 << kHashingEmbedderVersion
                 << " failed; memory disabled this session";
      return false;
    }
  }
  return true;
}

bool MemoryStorage::LoadOrCreateUrlKey(sql::MetaTable* meta) {
  std::string stored;
  if (meta->GetValue(kUrlKeyMetaKey, &stored) && !stored.empty()) {
    std::string wrapped;
    if (!base::Base64Decode(stored, &wrapped)) return false;
    std::string key = Decrypt(wrapped);
    if (key.size() != kUrlKeyBytes) {
      // Keychain entry gone or DB copied from another account: we cannot
      // match existing rows. Refuse to open rather than silently forking.
      LOG(ERROR) << "[MoltMemory] cannot unwrap URL key";
      return false;
    }
    url_key_.assign(key.begin(), key.end());
    return true;
  }
  std::vector<uint8_t> key(kUrlKeyBytes);
  crypto::RandBytes(key);
  std::string wrapped;
  if (!Encrypt(std::string(key.begin(), key.end()), &wrapped)) return false;
  if (!meta->SetValue(kUrlKeyMetaKey,
                      base::Base64Encode(base::as_byte_span(wrapped)))) {
    return false;
  }
  url_key_ = std::move(key);
  return true;
}

std::vector<uint8_t> MemoryStorage::UrlHash(const std::string& url) const {
  auto mac = crypto::hmac::SignSha256(url_key_, base::as_byte_span(url));
  return std::vector<uint8_t>(mac.begin(), mac.end());
}

std::vector<uint8_t> MemoryStorage::KeyedContentHash(
    const std::vector<uint8_t>& content_hash) const {
  // The recorder hands us SHA-256(page text). Stored as-is that is a
  // "did this user read exactly this page?" oracle, so key it too.
  if (content_hash.empty()) return {};
  auto mac = crypto::hmac::SignSha256(url_key_, content_hash);
  return std::vector<uint8_t>(mac.begin(), mac.end());
}

bool MemoryStorage::MigrateV1ToV2(sql::MetaTable* meta,
                                  const MemoryEmbedder& embedder) {
  sql::Transaction tx(db_.get());
  if (!tx.Begin()) return false;
  if (!LoadOrCreateUrlKey(meta)) return false;

  if (!db_->DoesColumnExist("documents", "url_enc") &&
      !db_->Execute("ALTER TABLE documents ADD COLUMN url_enc BLOB")) {
    return false;
  }
  if (!db_->DoesColumnExist("documents", "url_hash") &&
      !db_->Execute("ALTER TABLE documents ADD COLUMN url_hash BLOB")) {
    return false;
  }

  struct Row {
    int64_t doc_id;
    std::string url;
    std::vector<uint8_t> content_hash;
  };
  std::vector<Row> rows;
  {
    sql::Statement s(db_->GetUniqueStatement(
        "SELECT doc_id, url, content_hash FROM documents"));
    while (s.Step()) {
      rows.push_back({s.ColumnInt64(0), s.ColumnString(1),
                      s.ColumnBlobAsVector(2)});
    }
    if (!s.Succeeded()) return false;
  }
  for (const Row& r : rows) {
    std::string url_enc;
    if (!Encrypt(r.url, &url_enc)) return false;
    sql::Statement u(db_->GetCachedStatement(
        SQL_FROM_HERE,
        "UPDATE documents SET url='', url_enc=?, url_hash=?, content_hash=? "
        "WHERE doc_id=?"));
    u.BindBlob(0, base::as_byte_span(url_enc));
    u.BindBlob(1, UrlHash(r.url));
    u.BindBlob(2, KeyedContentHash(r.content_hash));
    u.BindInt64(3, r.doc_id);
    if (!u.Run()) return false;
  }

  if (!db_->Execute("DROP INDEX IF EXISTS idx_documents_url") ||
      !db_->Execute("CREATE INDEX IF NOT EXISTS idx_documents_url_hash "
                    "ON documents(url_hash)")) {
    return false;
  }

  // Recompute every vector from the decrypted chunk text: v1 vectors were
  // plaintext AND came from the buggy (case-sensitive) embedder.
  if (!ReembedAllChunks(embedder)) return false;

  if (!meta->SetVersionNumber(kCurrentSchemaVersion) ||
      !meta->SetCompatibleVersionNumber(kCompatibleVersionNumber) ||
      !meta->SetValue(kEmbedderVersionMetaKey,
                      static_cast<int64_t>(kHashingEmbedderVersion))) {
    return false;
  }
  if (!tx.Commit()) return false;

  // Rewrite the file so the old plaintext URLs/vectors don't survive in
  // SQLite free pages. Best-effort; outside the transaction by necessity.
  if (!db_->Execute("VACUUM")) {
    LOG(WARNING) << "[MoltMemory] VACUUM after migration failed";
  }
  LOG(INFO) << "[MoltMemory] migrated " << rows.size()
            << " documents to encrypted schema v2";
  return true;
}

bool MemoryStorage::ReembedAllChunks(const MemoryEmbedder& embedder) {
  // Caller holds a transaction.
  int64_t after = 0;
  while (true) {
    std::vector<std::pair<int64_t, std::string>> batch;
    {
      sql::Statement s(db_->GetCachedStatement(
          SQL_FROM_HERE,
          "SELECT chunk_id, text_enc FROM chunks WHERE chunk_id > ? "
          "ORDER BY chunk_id ASC LIMIT ?"));
      s.BindInt64(0, after);
      s.BindInt(1, kReembedBatch);
      while (s.Step()) {
        batch.emplace_back(s.ColumnInt64(0), s.ColumnBlobAsString(1));
      }
      if (!s.Succeeded()) return false;
    }
    if (batch.empty()) return true;
    for (auto& [chunk_id, text_enc] : batch) {
      // An undecryptable chunk embeds to the zero vector: it simply never
      // matches, and stays deletable.
      const std::string text = Decrypt(text_enc);
      std::string emb_enc;
      if (!Encrypt(PackEmbedding(embedder.Embed(text)), &emb_enc)) {
        return false;
      }
      sql::Statement u(db_->GetCachedStatement(
          SQL_FROM_HERE, "UPDATE chunks SET embedding=? WHERE chunk_id=?"));
      u.BindBlob(0, base::as_byte_span(emb_enc));
      u.BindInt64(1, chunk_id);
      if (!u.Run()) return false;
      after = chunk_id;
    }
  }
}

bool MemoryStorage::Encrypt(const std::string& plaintext,
                            std::string* ciphertext) const {
  // Any key, the hard-coded Linux one included; fails (and the caller then
  // refuses to persist) when there is none or the Encryptor isn't ready.
  // An empty field stays empty, as it did with OSCrypt.
  return molt_os_crypt::EncryptString(plaintext, ciphertext);
}

std::string MemoryStorage::Decrypt(const std::string& ciphertext) const {
  std::string out;
  if (ciphertext.empty()) return std::string();
  if (!molt_os_crypt::DecryptString(ciphertext, &out)) return std::string();
  return out;
}

int64_t MemoryStorage::FindUnchangedDoc(
    const std::string& url,
    const std::vector<uint8_t>& content_hash) {
  if (!open_ || content_hash.empty()) return 0;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT doc_id FROM documents WHERE url_hash=? AND content_hash=? "
      "LIMIT 1"));
  s.BindBlob(0, UrlHash(url));
  s.BindBlob(1, KeyedContentHash(content_hash));
  if (s.Step()) return s.ColumnInt64(0);
  return 0;
}

bool MemoryStorage::InsertDocumentWithChunks(Document* doc,
                                             std::vector<Chunk>* chunks) {
  if (!open_ || !doc || !chunks) return false;
  sql::Transaction tx(db_.get());
  if (!tx.Begin()) return false;

  // Never persist plaintext: any encryption failure aborts the insert.
  std::string url_enc;
  std::string title_enc;
  if (!Encrypt(doc->url, &url_enc) || !Encrypt(doc->title, &title_enc)) {
    return false;
  }

  sql::Statement ins(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO documents(url, url_enc, url_hash, title_enc, visited_at, "
      " word_count, content_hash) VALUES('', ?, ?, ?, ?, ?, ?)"));
  ins.BindBlob(0, base::as_byte_span(url_enc));
  ins.BindBlob(1, UrlHash(doc->url));
  ins.BindBlob(2, base::as_byte_span(title_enc));
  ins.BindInt64(3, doc->visited_at_unix);
  ins.BindInt(4, doc->word_count);
  ins.BindBlob(5, KeyedContentHash(doc->content_hash));
  if (!ins.Run()) return false;
  doc->doc_id = db_->GetLastInsertRowId();

  for (auto& c : *chunks) {
    c.doc_id = doc->doc_id;
    std::string text_enc;
    std::string emb_enc;
    if (!Encrypt(c.text, &text_enc) ||
        !Encrypt(PackEmbedding(c.embedding), &emb_enc)) {
      return false;
    }
    sql::Statement cins(db_->GetCachedStatement(
        SQL_FROM_HERE,
        "INSERT INTO chunks(doc_id, chunk_idx, text_enc, embedding) "
        "VALUES(?, ?, ?, ?)"));
    cins.BindInt64(0, c.doc_id);
    cins.BindInt(1, c.chunk_idx);
    cins.BindBlob(2, base::as_byte_span(text_enc));
    cins.BindBlob(3, base::as_byte_span(emb_enc));
    if (!cins.Run()) return false;
    c.chunk_id = db_->GetLastInsertRowId();
  }

  return tx.Commit();
}

std::vector<Document> MemoryStorage::ListRecentDocuments(int limit) {
  std::vector<Document> out;
  if (!open_ || limit <= 0) return out;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT doc_id, url_enc, title_enc, visited_at, word_count "
      "FROM documents ORDER BY visited_at DESC LIMIT ?"));
  s.BindInt(0, limit);
  while (s.Step()) {
    Document d;
    d.doc_id = s.ColumnInt64(0);
    d.url = Decrypt(s.ColumnBlobAsString(1));
    d.title = Decrypt(s.ColumnBlobAsString(2));
    d.visited_at_unix = s.ColumnInt64(3);
    d.word_count = s.ColumnInt(4);
    out.push_back(std::move(d));
  }
  return out;
}

std::optional<Document> MemoryStorage::GetDocument(int64_t doc_id) {
  if (!open_) return std::nullopt;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT url_enc, title_enc, visited_at, word_count "
      "FROM documents WHERE doc_id=?"));
  s.BindInt64(0, doc_id);
  if (!s.Step()) return std::nullopt;
  Document d;
  d.doc_id = doc_id;
  d.url = Decrypt(s.ColumnBlobAsString(0));
  d.title = Decrypt(s.ColumnBlobAsString(1));
  d.visited_at_unix = s.ColumnInt64(2);
  d.word_count = s.ColumnInt(3);
  return d;
}

std::optional<Chunk> MemoryStorage::GetChunk(int64_t chunk_id) {
  if (!open_) return std::nullopt;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT doc_id, chunk_idx, text_enc FROM chunks WHERE chunk_id=?"));
  s.BindInt64(0, chunk_id);
  if (!s.Step()) return std::nullopt;
  Chunk c;
  c.chunk_id = chunk_id;
  c.doc_id = s.ColumnInt64(0);
  c.chunk_idx = s.ColumnInt(1);
  c.text = Decrypt(s.ColumnBlobAsString(2));
  return c;
}

bool MemoryStorage::DeleteDocument(int64_t doc_id) {
  if (!open_) return false;
  sql::Transaction tx(db_.get());
  if (!tx.Begin()) return false;
  sql::Statement dc(db_->GetCachedStatement(
      SQL_FROM_HERE, "DELETE FROM chunks WHERE doc_id=?"));
  dc.BindInt64(0, doc_id);
  if (!dc.Run()) return false;
  sql::Statement dd(db_->GetCachedStatement(
      SQL_FROM_HERE, "DELETE FROM documents WHERE doc_id=?"));
  dd.BindInt64(0, doc_id);
  if (!dd.Run()) return false;
  return tx.Commit();
}

std::vector<int64_t> MemoryStorage::DeleteByDomain(const std::string& domain) {
  std::vector<int64_t> deleted;
  if (!open_ || domain.empty()) return deleted;
  const std::string want = base::ToLowerASCII(domain);
  // URLs are encrypted, so match host-by-host after decrypting. Exact
  // host, plus *.<domain> for subdomains.
  std::vector<int64_t> ids;
  {
    sql::Statement s(db_->GetCachedStatement(
        SQL_FROM_HERE, "SELECT doc_id, url_enc FROM documents"));
    while (s.Step()) {
      GURL g(Decrypt(s.ColumnBlobAsString(1)));
      if (!g.is_valid()) continue;
      std::string host = base::ToLowerASCII(std::string(g.host()));
      if (host == want ||
          (host.size() > want.size() + 1 &&
           host.compare(host.size() - want.size() - 1, want.size() + 1,
                        "." + want) == 0)) {
        ids.push_back(s.ColumnInt64(0));
      }
    }
  }
  for (int64_t id : ids) {
    if (DeleteDocument(id)) deleted.push_back(id);
  }
  return deleted;
}

std::vector<int64_t> MemoryStorage::DeleteDocumentsVisitedBefore(
    int64_t cutoff_unix) {
  std::vector<int64_t> ids;
  if (!open_) return ids;
  {
    sql::Statement s(db_->GetCachedStatement(
        SQL_FROM_HERE, "SELECT doc_id FROM documents WHERE visited_at < ?"));
    s.BindInt64(0, cutoff_unix);
    while (s.Step()) ids.push_back(s.ColumnInt64(0));
  }
  if (ids.empty()) return ids;
  sql::Transaction tx(db_.get());
  if (!tx.Begin()) return {};
  sql::Statement dc(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "DELETE FROM chunks WHERE doc_id IN "
      "(SELECT doc_id FROM documents WHERE visited_at < ?)"));
  dc.BindInt64(0, cutoff_unix);
  sql::Statement dd(db_->GetCachedStatement(
      SQL_FROM_HERE, "DELETE FROM documents WHERE visited_at < ?"));
  dd.BindInt64(0, cutoff_unix);
  if (!dc.Run() || !dd.Run() || !tx.Commit()) return {};
  return ids;
}

bool MemoryStorage::Clear() {
  if (!open_) return false;
  sql::Transaction tx(db_.get());
  if (!tx.Begin()) return false;
  if (!db_->Execute("DELETE FROM chunks")) return false;
  if (!db_->Execute("DELETE FROM documents")) return false;
  if (!tx.Commit()) return false;
  // Shrink the file; everything sensitive was encrypted anyway.
  if (!db_->Execute("VACUUM")) {
    LOG(WARNING) << "[MoltMemory] VACUUM after clear failed";
  }
  return true;
}

bool MemoryStorage::LoadNewestEmbeddings(size_t limit,
                                         std::vector<EmbeddingRow>* out) {
  if (!open_ || !out) return false;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT chunk_id, doc_id, embedding FROM chunks "
      "ORDER BY chunk_id DESC LIMIT ?"));
  s.BindInt64(0, static_cast<int64_t>(limit));
  while (s.Step()) {
    std::vector<float> v = UnpackEmbedding(Decrypt(s.ColumnBlobAsString(2)));
    if (v.empty()) continue;
    out->emplace_back(s.ColumnInt64(0), s.ColumnInt64(1), std::move(v));
  }
  std::reverse(out->begin(), out->end());
  return true;
}

int MemoryStorage::CountDocuments() {
  if (!open_) return 0;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE, "SELECT COUNT(*) FROM documents"));
  return s.Step() ? s.ColumnInt(0) : 0;
}

int MemoryStorage::CountChunks() {
  if (!open_) return 0;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE, "SELECT COUNT(*) FROM chunks"));
  return s.Step() ? s.ColumnInt(0) : 0;
}

}  // namespace memory
}  // namespace molt_ai
