// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_storage.h"

#include <cstring>
#include <utility>

#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "components/os_crypt/sync/os_crypt.h"
#include "crypto/sha2.h"
#include "sql/meta_table.h"
#include "sql/statement.h"
#include "sql/transaction.h"
#include "url/gurl.h"

namespace molt_ai {
namespace memory {

namespace {

constexpr int kCurrentSchemaVersion = 1;
constexpr int kCompatibleVersionNumber = 1;

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

bool MemoryStorage::Open(const base::FilePath& memory_dir) {
  if (open_) return true;
  if (!base::CreateDirectory(memory_dir)) {
    LOG(ERROR) << "[MoltMemory] cannot create dir " << memory_dir.value();
    return false;
  }
  db_path_ = memory_dir.Append(FILE_PATH_LITERAL("memory.db"));

  db_ = std::make_unique<sql::Database>(
      sql::DatabaseOptions().set_preload(true),
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

  if (!EnsureSchema()) {
    LOG(ERROR) << "[MoltMemory] schema setup failed; wiping";
    db_->Close();
    base::DeleteFile(db_path_);
    if (!db_->Open(db_path_) || !EnsureSchema()) return false;
  }
  open_ = true;
  return true;
}

bool MemoryStorage::EnsureSchema() {
  sql::MetaTable meta;
  if (!meta.Init(db_.get(), kCurrentSchemaVersion,
                  kCompatibleVersionNumber)) {
    return false;
  }
  if (meta.GetCompatibleVersionNumber() > kCurrentSchemaVersion) {
    LOG(WARNING) << "[MoltMemory] DB too new (compat="
                 << meta.GetCompatibleVersionNumber() << ")";
    return false;
  }

  if (!db_->Execute(
          "CREATE TABLE IF NOT EXISTS documents ("
          " doc_id INTEGER PRIMARY KEY AUTOINCREMENT,"
          " url TEXT NOT NULL,"
          " title_enc BLOB,"
          " visited_at INTEGER NOT NULL,"
          " word_count INTEGER NOT NULL DEFAULT 0,"
          " content_hash BLOB"
          ")")) {
    return false;
  }
  if (!db_->Execute(
          "CREATE INDEX IF NOT EXISTS idx_documents_url "
          "ON documents(url)")) {
    return false;
  }
  if (!db_->Execute(
          "CREATE INDEX IF NOT EXISTS idx_documents_visited "
          "ON documents(visited_at DESC)")) {
    return false;
  }
  if (!db_->Execute(
          "CREATE TABLE IF NOT EXISTS chunks ("
          " chunk_id INTEGER PRIMARY KEY AUTOINCREMENT,"
          " doc_id INTEGER NOT NULL,"
          " chunk_idx INTEGER NOT NULL,"
          " text_enc BLOB,"
          " embedding BLOB"
          ")")) {
    return false;
  }
  if (!db_->Execute(
          "CREATE INDEX IF NOT EXISTS idx_chunks_doc "
          "ON chunks(doc_id)")) {
    return false;
  }
  return true;
}

std::string MemoryStorage::Encrypt(const std::string& plaintext) const {
  std::string out;
  if (!OSCrypt::EncryptString(plaintext, &out)) return std::string();
  return out;
}

std::string MemoryStorage::Decrypt(const std::string& ciphertext) const {
  std::string out;
  if (ciphertext.empty()) return std::string();
  if (!OSCrypt::DecryptString(ciphertext, &out)) return std::string();
  return out;
}

int64_t MemoryStorage::FindUnchangedDoc(
    const std::string& url,
    const std::vector<uint8_t>& content_hash) {
  if (!open_ || content_hash.empty()) return 0;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT doc_id FROM documents WHERE url=? AND content_hash=? LIMIT 1"));
  s.BindString(0, url);
  s.BindBlob(1, content_hash);
  if (s.Step()) return s.ColumnInt64(0);
  return 0;
}

bool MemoryStorage::InsertDocumentWithChunks(Document* doc,
                                              std::vector<Chunk>* chunks) {
  if (!open_ || !doc || !chunks) return false;
  sql::Transaction tx(db_.get());
  if (!tx.Begin()) return false;

  std::string title_enc = Encrypt(doc->title);

  sql::Statement ins(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO documents(url, title_enc, visited_at, word_count, "
      " content_hash) VALUES(?, ?, ?, ?, ?)"));
  ins.BindString(0, doc->url);
  ins.BindBlob(1, base::as_byte_span(title_enc));
  ins.BindInt64(2, doc->visited_at_unix);
  ins.BindInt(3, doc->word_count);
  ins.BindBlob(4, doc->content_hash);
  if (!ins.Run()) return false;
  doc->doc_id = db_->GetLastInsertRowId();

  for (auto& c : *chunks) {
    c.doc_id = doc->doc_id;
    std::string text_enc = Encrypt(c.text);

    // Pack embedding floats into a tight little-endian blob. (Floats
    // are already little-endian on every platform Chromium runs on,
    // so this is a straight memcpy.)
    std::vector<uint8_t> emb_blob(c.embedding.size() * sizeof(float));
    if (!c.embedding.empty()) {
      std::memcpy(emb_blob.data(), c.embedding.data(), emb_blob.size());
    }

    sql::Statement cins(db_->GetCachedStatement(
        SQL_FROM_HERE,
        "INSERT INTO chunks(doc_id, chunk_idx, text_enc, embedding) "
        "VALUES(?, ?, ?, ?)"));
    cins.BindInt64(0, c.doc_id);
    cins.BindInt(1, c.chunk_idx);
    cins.BindBlob(2, base::as_byte_span(text_enc));
    cins.BindBlob(3, emb_blob);
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
      "SELECT doc_id, url, title_enc, visited_at, word_count, content_hash "
      "FROM documents ORDER BY visited_at DESC LIMIT ?"));
  s.BindInt(0, limit);
  while (s.Step()) {
    Document d;
    d.doc_id = s.ColumnInt64(0);
    d.url = s.ColumnString(1);
    d.title = Decrypt(s.ColumnBlobAsString(2));
    d.visited_at_unix = s.ColumnInt64(3);
    d.word_count = s.ColumnInt(4);
    d.content_hash = s.ColumnBlobAsVector(5);
    out.push_back(std::move(d));
  }
  return out;
}

std::optional<Document> MemoryStorage::GetDocument(int64_t doc_id) {
  if (!open_) return std::nullopt;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT url, title_enc, visited_at, word_count, content_hash "
      "FROM documents WHERE doc_id=?"));
  s.BindInt64(0, doc_id);
  if (!s.Step()) return std::nullopt;
  Document d;
  d.doc_id = doc_id;
  d.url = s.ColumnString(0);
  d.title = Decrypt(s.ColumnBlobAsString(1));
  d.visited_at_unix = s.ColumnInt64(2);
  d.word_count = s.ColumnInt(3);
  d.content_hash = s.ColumnBlobAsVector(4);
  return d;
}

std::optional<Chunk> MemoryStorage::GetChunk(int64_t chunk_id) {
  if (!open_) return std::nullopt;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT doc_id, chunk_idx, text_enc, embedding FROM chunks "
      "WHERE chunk_id=?"));
  s.BindInt64(0, chunk_id);
  if (!s.Step()) return std::nullopt;
  Chunk c;
  c.chunk_id = chunk_id;
  c.doc_id = s.ColumnInt64(0);
  c.chunk_idx = s.ColumnInt(1);
  c.text = Decrypt(s.ColumnBlobAsString(2));
  std::string emb_blob = s.ColumnBlobAsString(3);
  if (emb_blob.size() % sizeof(float) == 0 && !emb_blob.empty()) {
    size_t n = emb_blob.size() / sizeof(float);
    c.embedding.resize(n);
    std::memcpy(c.embedding.data(), emb_blob.data(), emb_blob.size());
  }
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

int MemoryStorage::DeleteByDomain(const std::string& domain) {
  if (!open_ || domain.empty()) return 0;
  // Match host exactly, plus *.<domain> for subdomains.
  std::vector<int64_t> ids;
  {
    sql::Statement s(db_->GetCachedStatement(
        SQL_FROM_HERE, "SELECT doc_id, url FROM documents"));
    while (s.Step()) {
      GURL g(s.ColumnString(1));
      if (!g.is_valid()) continue;
      std::string host = std::string(g.host());
      if (host == domain ||
          (host.size() > domain.size() + 1 &&
           host.compare(host.size() - domain.size() - 1,
                        domain.size() + 1, "." + domain) == 0)) {
        ids.push_back(s.ColumnInt64(0));
      }
    }
  }
  int deleted = 0;
  for (int64_t id : ids)
    if (DeleteDocument(id)) ++deleted;
  return deleted;
}

bool MemoryStorage::Clear() {
  if (!open_) return false;
  sql::Transaction tx(db_.get());
  if (!tx.Begin()) return false;
  if (!db_->Execute("DELETE FROM chunks")) return false;
  if (!db_->Execute("DELETE FROM documents")) return false;
  return tx.Commit();
}

bool MemoryStorage::LoadAllEmbeddings(
    std::vector<std::tuple<int64_t, int64_t, std::vector<float>>>* out) {
  if (!open_ || !out) return false;
  sql::Statement s(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT chunk_id, doc_id, embedding FROM chunks "
      "ORDER BY chunk_id ASC"));
  while (s.Step()) {
    int64_t chunk_id = s.ColumnInt64(0);
    int64_t doc_id = s.ColumnInt64(1);
    std::string emb_blob = s.ColumnBlobAsString(2);
    if (emb_blob.size() != kEmbeddingDim * sizeof(float)) continue;
    std::vector<float> v(kEmbeddingDim);
    std::memcpy(v.data(), emb_blob.data(), emb_blob.size());
    out->emplace_back(chunk_id, doc_id, std::move(v));
  }
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
