// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryBackend with a real (encrypted) MemoryStorage and a scripted
// asynchronous "model" embedder: the store is versioned by vector space,
// switching to the model re-embeds in the background from the encrypted
// chunk text without mixing vectors, switching back re-embeds at once, and
// the privacy gates (memory on and not paused, deletes) hold throughout.

#include "chrome/browser/molt_ai/memory/memory_backend.h"

#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt_test_util.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "sql/database.h"
#include "sql/meta_table.h"
#include "sql/test/test_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace memory {
namespace {

const char kModelSpace[] = "fake-model@0123456789ab/256";

// Pages long enough for NormalizePageText, one topic each.
std::string Page(const std::string& topic) {
  std::string s;
  for (int i = 0; i < 12; ++i) {
    s += "This page is about " + topic + " and nothing else at all. ";
  }
  return s;
}

// The "model": a vector from the topic words a text contains, so a search
// for "miso" finds the miso page whatever its wording; answers only when
// the test says so.
class FakeModelEmbedder : public MemoryEmbedder {
 public:
  struct Call {
    std::vector<std::string> texts;
    EmbedPurpose purpose;
    BatchCallback done;
  };

  // |loaded| (may be null: loaded) says whether the model's process is
  // already loaded, for EmbedQueryIfReady().
  explicit FakeModelEmbedder(std::deque<Call>* calls,
                             const bool* loaded = nullptr)
      : calls_(calls), loaded_(loaded) {}

  EmbedderSpace space() const override {
    return EmbedderSpace{"fake-model@0123456789ab", kEmbeddingDim};
  }
  bool is_synchronous() const override { return false; }
  std::vector<float> Embed(const std::string&) const override { return {}; }
  void EmbedBatch(std::vector<std::string> texts,
                  EmbedPurpose purpose,
                  BatchCallback done) const override {
    calls_->push_back({std::move(texts), purpose, std::move(done)});
  }
  size_t max_batch_size() const override { return 2; }
  void EmbedQueryIfReady(std::string text, BatchCallback done) const override {
    if (loaded_ && !*loaded_) {
      EmbedBatchResult r;
      r.outcome = EmbedOutcome::kRetryLater;
      done(std::move(r));
      return;
    }
    MemoryEmbedder::EmbedQueryIfReady(std::move(text), std::move(done));
  }

  static std::vector<float> VectorFor(const std::string& text) {
    std::vector<float> v(kEmbeddingDim, 0.0f);
    const char* topics[] = {"miso", "kubernetes", "bali", "poison"};
    for (size_t i = 0; i < 4; ++i) {
      if (text.find(topics[i]) != std::string::npos) v[i] = 1.0f;
    }
    if (v[0] + v[1] + v[2] + v[3] == 0.0f) v[100] = 1.0f;
    float n = 0;
    for (float x : v) n += x * x;
    for (float& x : v) x /= std::sqrt(n);
    return v;
  }

 private:
  raw_ptr<std::deque<Call>> calls_;
  raw_ptr<const bool> loaded_;
};

class MemoryBackendTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    NewBackend();
  }

  void NewBackend() {
    backend_.reset();
    calls_.clear();
    tasks_.clear();
    backend_ = std::make_unique<MemoryBackend>(
        [this](std::function<void()> task, int64_t delay_ms) {
          tasks_.push_back(std::move(task));
        });
    backend_->Start(temp_dir_.GetPath().AppendASCII("MoltMemory"));
    ASSERT_TRUE(backend_->open());
  }

  // Runs the backend's delayed tasks (time skips ahead).
  void RunTasks() {
    for (int guard = 0; guard < 1000 && !tasks_.empty(); ++guard) {
      std::function<void()> t = std::move(tasks_.front());
      tasks_.pop_front();
      t();
    }
  }

  // Answers the oldest model call.
  void Answer(EmbedOutcome outcome = EmbedOutcome::kOk) {
    ASSERT_FALSE(calls_.empty());
    FakeModelEmbedder::Call call = std::move(calls_.front());
    calls_.pop_front();
    EmbedBatchResult r;
    r.outcome = outcome;
    if (outcome == EmbedOutcome::kOk) {
      for (const std::string& t : call.texts) {
        r.vectors.push_back(FakeModelEmbedder::VectorFor(t));
      }
    }
    call.done(std::move(r));
    RunTasks();
  }

  // Answers model calls until none is left.
  void AnswerAll() {
    for (int guard = 0; guard < 1000 && !calls_.empty(); ++guard) Answer();
  }

  void UseModel() {
    backend_->SetModelEmbedder(
        std::make_unique<FakeModelEmbedder>(&calls_, &model_loaded_));
    RunTasks();
  }

  void NoModel() {
    backend_->SetModelEmbedder(nullptr);
    RunTasks();
  }

  void Ingest(const std::string& url, const std::string& topic) {
    backend_->Ingest(url, topic, Page(topic), 1700000000);
    RunTasks();
  }

  // Runs a search; answers the model's query call if one is made.
  SearchResult Search(const std::string& text, bool answer = true) {
    SearchResult out;
    bool done = false;
    backend_->Query(text, 5, /*one_per_page=*/true, [&](SearchResult r) {
      out = std::move(r);
      done = true;
    });
    if (!done && answer && !calls_.empty() &&
        calls_.back().purpose == EmbedPurpose::kQuery) {
      FakeModelEmbedder::Call call = std::move(calls_.back());
      calls_.pop_back();
      EmbedBatchResult r;
      r.outcome = EmbedOutcome::kOk;
      r.vectors.push_back(FakeModelEmbedder::VectorFor(call.texts[0]));
      call.done(std::move(r));
    }
    EXPECT_TRUE(done);
    return out;
  }

  MemoryStorage& storage() { return backend_->storage_for_testing(); }

  // Edits the meta table directly (as another MoltBrowser version would
  // leave it), then reopens.
  void EditMeta(const std::function<void(sql::MetaTable*)>& edit) {
    backend_.reset();
    {
      sql::Database db(sql::DatabaseOptions(), sql::test::kTestTag);
      ASSERT_TRUE(db.Open(temp_dir_.GetPath()
                              .AppendASCII("MoltMemory")
                              .AppendASCII("memory.db")));
      sql::MetaTable meta;
      ASSERT_TRUE(meta.Init(&db, 2, 2));
      edit(&meta);
      db.Close();
    }
    NewBackend();
  }

  // The @memory omnibox shortcut's answers so far, in order (later ones
  // land in omnibox_answers()).
  std::vector<OmniboxSearchResult> OmniboxSearch(
      const std::string& text,
      std::function<bool()> still_wanted = {}) {
    auto answers = std::make_shared<std::vector<OmniboxSearchResult>>();
    omnibox_answers_ = answers;
    backend_->OmniboxSearch(
        text, 5, std::move(still_wanted),
        [answers](OmniboxSearchResult r) { answers->push_back(std::move(r)); });
    return *answers;
  }
  const std::vector<OmniboxSearchResult>& omnibox_answers() const {
    return *omnibox_answers_;
  }

  bool model_loaded_ = true;
  std::shared_ptr<std::vector<OmniboxSearchResult>> omnibox_answers_;
  molt_os_crypt::ScopedEncryptorForTesting encryptor_{
      test::MakeOsKeyEncryptor()};
  base::ScopedTempDir temp_dir_;
  std::deque<FakeModelEmbedder::Call> calls_;
  std::deque<std::function<void()>> tasks_;
  std::unique_ptr<MemoryBackend> backend_;
};

TEST_F(MemoryBackendTest, NewStoreSearchesWithHashing) {
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
  NoModel();
  Ingest("https://a.example/miso", "miso");
  Ingest("https://b.example/k8s", "kubernetes");
  SearchResult r = Search("miso");
  EXPECT_TRUE(r.ok);
  EXPECT_FALSE(r.model);
  ASSERT_FALSE(r.hits.empty());
  EXPECT_EQ(r.hits[0].url, "https://a.example/miso");
  EXPECT_EQ(r.hits[0].title, "miso");
  EXPECT_EQ(r.hits[0].visited_at_unix, 1700000000);
  EXPECT_FALSE(r.hits[0].snippet.empty());
  // One hit per page.
  EXPECT_LE(r.hits.size(), 2u);
}

// Nothing is re-embedded before the model's state is known (at startup it
// isn't yet, and the store may hold the model's vectors).
TEST_F(MemoryBackendTest, WaitsForTheModelState) {
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  EXPECT_TRUE(storage().next_space().empty());
  EXPECT_TRUE(calls_.empty());
}

TEST_F(MemoryBackendTest, MigratesToTheModelInTheBackground) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  Ingest("https://b.example/k8s", "kubernetes");
  Ingest("https://c.example/bali", "bali");
  const int chunks = storage().CountChunks();
  ASSERT_GT(chunks, 2);

  UseModel();
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
  EXPECT_EQ(storage().next_space(), kModelSpace);
  ASSERT_EQ(calls_.size(), 1u) << "one batch at a time";
  EXPECT_EQ(calls_.front().purpose, EmbedPurpose::kDocument);
  EXPECT_LE(calls_.front().texts.size(), 2u);

  // While it runs, searches use the committed (hashing) vectors only.
  SearchResult during = Search("miso");
  EXPECT_TRUE(during.ok);
  EXPECT_FALSE(during.model);
  SemanticIndexStatus status = backend_->GetSemanticStatus();
  EXPECT_TRUE(status.migrating);
  EXPECT_FALSE(status.model_active);
  EXPECT_EQ(status.migration_total, chunks);
  EXPECT_EQ(status.migration_done, 0);

  // A page saved meanwhile gets a hashing vector now and a model vector
  // later in the same pass.
  Ingest("https://d.example/miso2", "miso");
  AnswerAll();

  EXPECT_EQ(storage().committed_space(), kModelSpace);
  EXPECT_TRUE(storage().next_space().empty());
  EXPECT_EQ(static_cast<int>(backend_->index_for_testing().size()),
            storage().CountChunks());
  status = backend_->GetSemanticStatus();
  EXPECT_TRUE(status.model_active);
  EXPECT_FALSE(status.migrating);
  EXPECT_EQ(status.pending, 0);

  SearchResult after = Search("miso");
  EXPECT_TRUE(after.ok);
  EXPECT_TRUE(after.model);
  ASSERT_GE(after.hits.size(), 2u);
  for (size_t i = 0; i < 2; ++i) {
    EXPECT_NE(after.hits[i].url.find("miso"), std::string::npos)
        << after.hits[i].url;
  }
}

TEST_F(MemoryBackendTest, OnlyWhileMemoryIsOnAndNotPaused) {
  NoModel();
  Ingest("https://a.example/miso", "miso");
  UseModel();
  EXPECT_EQ(storage().next_space(), kModelSpace);
  EXPECT_TRUE(calls_.empty()) << "background not allowed yet";
  backend_->SetBackgroundAllowed(true);
  RunTasks();
  EXPECT_EQ(calls_.size(), 1u);
  backend_->SetBackgroundAllowed(false);
  Answer();
  EXPECT_TRUE(calls_.empty()) << "paused: no next batch";
}

TEST_F(MemoryBackendTest, ModelVectorsSurviveARestart) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  AnswerAll();
  ASSERT_EQ(storage().committed_space(), kModelSpace);
  const size_t indexed = backend_->index_for_testing().size();

  NewBackend();
  EXPECT_EQ(storage().committed_space(), kModelSpace);
  EXPECT_EQ(backend_->index_for_testing().size(), indexed);
  backend_->SetBackgroundAllowed(true);
  UseModel();
  EXPECT_TRUE(calls_.empty()) << "nothing to re-embed";
  EXPECT_TRUE(Search("miso").model);
}

TEST_F(MemoryBackendTest, RemovingTheModelSwitchesBackAtOnce) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  AnswerAll();
  ASSERT_EQ(storage().committed_space(), kModelSpace);

  NoModel();
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
  EXPECT_EQ(static_cast<int>(backend_->index_for_testing().size()),
            storage().CountChunks());
  SearchResult r = Search("miso");
  EXPECT_TRUE(r.ok);
  EXPECT_FALSE(r.model);
  EXPECT_FALSE(r.hits.empty());
}

// With the model committed, a new page is stored at once (pending) and
// searchable once its chunks are embedded.
TEST_F(MemoryBackendTest, NewPagesWithTheModelCommitted) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  UseModel();
  AnswerAll();  // empty store: commits at once
  ASSERT_EQ(storage().committed_space(), kModelSpace);

  Ingest("https://b.example/bali", "bali");
  EXPECT_GT(storage().CountPendingChunks(), 0);
  EXPECT_EQ(backend_->index_for_testing().size(), 0u);
  EXPECT_GT(backend_->GetSemanticStatus().pending, 0);
  AnswerAll();
  EXPECT_EQ(storage().CountPendingChunks(), 0);
  SearchResult r = Search("bali");
  ASSERT_FALSE(r.hits.empty());
  EXPECT_EQ(r.hits[0].url, "https://b.example/bali");
}

// A page deleted while its text is with the model: the vectors that come
// back are dropped, nothing of it is indexed or stored.
TEST_F(MemoryBackendTest, DeletedWhileEmbedding) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  UseModel();
  AnswerAll();
  Ingest("https://b.example/bali", "bali");
  ASSERT_FALSE(calls_.empty());
  std::vector<Document> docs = backend_->ListRecent(10);
  ASSERT_EQ(docs.size(), 1u);
  EXPECT_TRUE(backend_->DeleteDocument(docs[0].doc_id));
  AnswerAll();
  EXPECT_EQ(backend_->index_for_testing().size(), 0u);
  EXPECT_EQ(storage().CountChunks(), 0);
  EXPECT_TRUE(Search("bali").hits.empty());
}

// A text that makes the model fail on its own gets an all-zero vector so
// the rest of the index goes on.
TEST_F(MemoryBackendTest, AFailingTextCantStallTheIndex) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  Ingest("https://p.example/poison", "poison");
  Ingest("https://c.example/bali", "bali");
  UseModel();
  int failed = 0;
  for (int guard = 0; guard < 200 && !calls_.empty(); ++guard) {
    bool poisoned = false;
    for (const std::string& t : calls_.front().texts) {
      poisoned = poisoned || t.find("poison") != std::string::npos;
    }
    if (poisoned) ++failed;
    Answer(poisoned ? EmbedOutcome::kFailed : EmbedOutcome::kOk);
  }
  EXPECT_GE(failed, 2);
  EXPECT_EQ(storage().committed_space(), kModelSpace);
  // The poisoned page is still there, just never found by meaning.
  EXPECT_EQ(backend_->ListRecent(10).size(), 3u);
  SearchResult r = Search("bali");
  ASSERT_FALSE(r.hits.empty());
  EXPECT_EQ(r.hits[0].url, "https://c.example/bali");
}

// "Not now" (memory, back-off) never blames the texts: it just waits.
TEST_F(MemoryBackendTest, RetryLaterWaits) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  for (int i = 0; i < 5; ++i) Answer(EmbedOutcome::kRetryLater);
  AnswerAll();
  EXPECT_EQ(storage().committed_space(), kModelSpace);
  SearchResult r = Search("miso");
  ASSERT_FALSE(r.hits.empty());
}

TEST_F(MemoryBackendTest, UnavailableModelFallsBackToHashing) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  Answer(EmbedOutcome::kUnavailable);
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
  EXPECT_TRUE(storage().next_space().empty());
  EXPECT_TRUE(backend_->GetSemanticStatus().model_failed);
  EXPECT_TRUE(Search("miso").ok);
}

// A query whose answer comes back after the space changed is not compared
// with vectors of the new space.
TEST_F(MemoryBackendTest, StaleQueryVectorsAreDropped) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  AnswerAll();
  ASSERT_EQ(storage().committed_space(), kModelSpace);
  SearchResult out;
  bool done = false;
  backend_->Query("miso", 5, true, [&](SearchResult r) {
    out = std::move(r);
    done = true;
  });
  ASSERT_FALSE(calls_.empty());
  FakeModelEmbedder::Call query = std::move(calls_.back());
  calls_.pop_back();
  NoModel();  // back to hashing before the model answers
  EmbedBatchResult r;
  r.outcome = EmbedOutcome::kOk;
  r.vectors.push_back(FakeModelEmbedder::VectorFor("miso"));
  query.done(std::move(r));
  ASSERT_TRUE(done);
  EXPECT_FALSE(out.ok);
  EXPECT_TRUE(out.hits.empty());
}

TEST_F(MemoryBackendTest, ClearAndRetentionDuringAMigration) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  Ingest("https://b.example/k8s", "kubernetes");
  UseModel();
  Answer();  // one batch stored in the next space
  backend_->EnforceRetention(2000000000);  // everything is older
  EXPECT_EQ(storage().CountChunks(), 0);
  EXPECT_EQ(storage().CountChunksMissingNextVector(), 0);
  AnswerAll();
  EXPECT_EQ(storage().committed_space(), kModelSpace);
  Ingest("https://c.example/bali", "bali");
  AnswerAll();
  EXPECT_TRUE(backend_->ClearAll());
  EXPECT_EQ(storage().CountChunks(), 0);
  EXPECT_EQ(backend_->index_for_testing().size(), 0u);
}

// The store only switches spaces when every chunk has a vector in the new
// one, and then all at once.
TEST_F(MemoryBackendTest, StorageCommitsOnlyACompleteSpace) {
  NoModel();
  Ingest("https://a.example/miso", "miso");
  Ingest("https://b.example/k8s", "kubernetes");
  MemoryStorage& s = storage();
  ASSERT_TRUE(s.BeginMigration("other/256"));
  std::vector<MemoryStorage::ChunkText> missing;
  ASSERT_TRUE(s.ChunksMissingNextVector(0, 100, &missing));
  ASSERT_GE(missing.size(), 2u);
  EXPECT_FALSE(missing[0].text.empty()) << "decrypted chunk text";
  std::vector<MemoryStorage::EmbeddingRow> rows;
  for (const auto& c : missing) {
    rows.emplace_back(c.chunk_id, c.doc_id,
                      FakeModelEmbedder::VectorFor("bali"));
  }
  const MemoryStorage::EmbeddingRow last = rows.back();
  rows.pop_back();
  ASSERT_TRUE(s.StoreNextVectors(rows));
  EXPECT_EQ(s.CountChunksMissingNextVector(), 1);
  EXPECT_FALSE(s.CommitMigration());
  EXPECT_EQ(s.committed_space(), "hashing-v3/256");
  // A vector of the wrong length is refused.
  EXPECT_FALSE(s.StoreNextVectors({MemoryStorage::EmbeddingRow(
      std::get<0>(last), std::get<1>(last), std::vector<float>(3, 0.5f))}));
  ASSERT_TRUE(s.StoreNextVectors({last}));
  ASSERT_TRUE(s.CommitMigration());
  EXPECT_EQ(s.committed_space(), "other/256");
  EXPECT_TRUE(s.next_space().empty());
  std::vector<MemoryStorage::EmbeddingRow> loaded;
  ASSERT_TRUE(s.LoadNewestEmbeddings(100, &loaded));
  ASSERT_EQ(loaded.size(), missing.size());
  for (const auto& row : loaded) {
    EXPECT_EQ(std::get<2>(row), FakeModelEmbedder::VectorFor("bali"));
  }
}

// A store from before vector spaces: its vectors are hashing-v<version>.
TEST_F(MemoryBackendTest, StoresFromBeforeSpaces) {
  NoModel();
  Ingest("https://a.example/miso", "miso");
  EditMeta([](sql::MetaTable* meta) {
    EXPECT_TRUE(meta->DeleteKey("embedder_space"));
  });
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
  NoModel();
  EXPECT_FALSE(Search("miso").hits.empty());

  // An older hashing version is re-embedded on open, as before spaces.
  EditMeta([](sql::MetaTable* meta) {
    EXPECT_TRUE(meta->DeleteKey("embedder_space"));
    EXPECT_TRUE(meta->SetValue("embedder_version", int64_t{2}));
  });
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
  EXPECT_EQ(static_cast<int>(backend_->index_for_testing().size()),
            storage().CountChunks());
}

// With the model committed the pre-space key says 0, so a MoltBrowser
// without spaces re-embeds with hashing when it opens the store, and sets it
// back; the store is then read as hashing again, never as the model's.
TEST_F(MemoryBackendTest, OpenedByAVersionWithoutSpaces) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  AnswerAll();
  ASSERT_EQ(storage().committed_space(), kModelSpace);
  EditMeta([](sql::MetaTable* meta) {
    int64_t version = -1;
    EXPECT_TRUE(meta->GetValue("embedder_version", &version));
    EXPECT_EQ(version, 0);
    // What that version does on open (its vectors are hashing-v3 now).
    EXPECT_TRUE(meta->SetValue("embedder_version", int64_t{3}));
  });
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
}

// The removed model's half-built space is dropped, not committed later.
TEST_F(MemoryBackendTest, RemovingTheModelMidMigration) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  Ingest("https://b.example/k8s", "kubernetes");
  UseModel();
  ASSERT_EQ(storage().next_space(), kModelSpace);
  NoModel();
  EXPECT_TRUE(storage().next_space().empty());
  Answer();  // the batch in flight comes back: dropped
  EXPECT_EQ(storage().committed_space(), "hashing-v3/256");
  EXPECT_FALSE(Search("miso").model);
}

// ---- The @memory omnibox shortcut (OmniboxSearch()) -----------------------

// With the keyword-hashing index: one final answer, at once, with the pages
// that have every word in their title, URL or text; no model is asked.
TEST_F(MemoryBackendTest, OmniboxSearchByKeywordsWithHashing) {
  NoModel();
  Ingest("https://a.example/miso", "miso");
  Ingest("https://b.example/k8s", "kubernetes");
  backend_->Ingest("https://c.example/trip", "Cheap flights to Bali",
                   Page("travel"), 1700000500);
  RunTasks();

  std::vector<OmniboxSearchResult> r = OmniboxSearch("miso");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_TRUE(r[0].final);
  EXPECT_FALSE(r[0].semantic);
  ASSERT_EQ(r[0].hits.size(), 1u);
  EXPECT_EQ(r[0].hits[0].url, "https://a.example/miso");
  EXPECT_EQ(r[0].hits[0].title, "miso");
  EXPECT_TRUE(r[0].hits[0].snippet.empty());

  // Title words, by prefix, in any case.
  r = OmniboxSearch("BALI fli");
  ASSERT_EQ(r.size(), 1u);
  ASSERT_EQ(r[0].hits.size(), 1u);
  EXPECT_EQ(r[0].hits[0].url, "https://c.example/trip");

  // Words only in the page text are found through the keyword index.
  r = OmniboxSearch("nothing else");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].hits.size(), 3u);

  // Every word must be there.
  EXPECT_TRUE(OmniboxSearch("miso kubernetes")[0].hits.empty());
  EXPECT_TRUE(OmniboxSearch("zanzibar")[0].hits.empty());
  EXPECT_TRUE(calls_.empty()) << "no model";
}

// With the model's index: keyword hits from titles and URLs first (not
// final), then hits by meaning once the (already loaded) model answers.
TEST_F(MemoryBackendTest, OmniboxSearchUpgradesToMeaning) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  Ingest("https://b.example/k8s", "kubernetes");
  UseModel();
  AnswerAll();
  ASSERT_EQ(storage().committed_space(), kModelSpace);

  std::vector<OmniboxSearchResult> r = OmniboxSearch("miso");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_FALSE(r[0].final);
  EXPECT_FALSE(r[0].semantic);
  ASSERT_EQ(r[0].hits.size(), 1u);
  EXPECT_EQ(r[0].hits[0].url, "https://a.example/miso");
  ASSERT_EQ(calls_.size(), 1u);
  EXPECT_EQ(calls_.front().purpose, EmbedPurpose::kQuery);
  EXPECT_EQ(calls_.front().texts, std::vector<std::string>{"miso"});

  Answer();
  ASSERT_EQ(omnibox_answers().size(), 2u);
  const OmniboxSearchResult& meaning = omnibox_answers()[1];
  EXPECT_TRUE(meaning.final);
  EXPECT_TRUE(meaning.semantic);
  ASSERT_FALSE(meaning.hits.empty());
  EXPECT_EQ(meaning.hits[0].url, "https://a.example/miso");
  EXPECT_TRUE(meaning.hits[0].snippet.empty());

  // Page-text words aren't in the keyword pass with the model's index; the
  // model still ranks pages by meaning.
  r = OmniboxSearch("this page");
  ASSERT_EQ(r.size(), 1u);
  EXPECT_TRUE(r[0].hits.empty());
  Answer();
  ASSERT_EQ(omnibox_answers().size(), 2u);
  EXPECT_TRUE(omnibox_answers()[1].final);
}

// Typing never loads the model: when its process isn't loaded, the keyword
// hits are the final answer and nothing is sent to it.
TEST_F(MemoryBackendTest, OmniboxSearchNeverLoadsTheModel) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  AnswerAll();
  ASSERT_EQ(storage().committed_space(), kModelSpace);

  model_loaded_ = false;
  OmniboxSearch("miso");
  RunTasks();
  ASSERT_EQ(omnibox_answers().size(), 2u);
  EXPECT_FALSE(omnibox_answers()[0].final);
  EXPECT_TRUE(omnibox_answers()[1].final);
  EXPECT_FALSE(omnibox_answers()[1].semantic);
  ASSERT_EQ(omnibox_answers()[1].hits.size(), 1u);
  EXPECT_EQ(omnibox_answers()[1].hits[0].url, "https://a.example/miso");
  EXPECT_TRUE(calls_.empty());
}

// A search nobody wants any more does nothing, at either step.
TEST_F(MemoryBackendTest, OmniboxSearchStopsWhenUnwanted) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  EXPECT_TRUE(OmniboxSearch("miso", [] { return false; }).empty());

  UseModel();
  AnswerAll();
  bool wanted = true;
  std::vector<OmniboxSearchResult> r =
      OmniboxSearch("miso", [&wanted] { return wanted; });
  ASSERT_EQ(r.size(), 1u);
  wanted = false;  // e.g. the user typed on
  Answer();
  EXPECT_EQ(omnibox_answers().size(), 1u);
}

// A query vector for a space no longer committed is never compared with the
// index: the keyword hits stay the answer.
TEST_F(MemoryBackendTest, OmniboxSearchDropsAStaleSpace) {
  NoModel();
  backend_->SetBackgroundAllowed(true);
  Ingest("https://a.example/miso", "miso");
  UseModel();
  AnswerAll();
  OmniboxSearch("miso");
  ASSERT_EQ(calls_.size(), 1u);
  FakeModelEmbedder::Call call = std::move(calls_.front());
  calls_.clear();
  NoModel();  // back to hashing at once
  ASSERT_EQ(storage().committed_space(), "hashing-v3/256");
  EmbedBatchResult result;
  result.outcome = EmbedOutcome::kOk;
  result.vectors.push_back(FakeModelEmbedder::VectorFor("miso"));
  call.done(std::move(result));
  ASSERT_EQ(omnibox_answers().size(), 2u);
  EXPECT_TRUE(omnibox_answers()[1].final);
  EXPECT_FALSE(omnibox_answers()[1].semantic);
}

}  // namespace
}  // namespace memory
}  // namespace molt_ai
