// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/embedding_space.h"

#include <string>

#include "chrome/browser/molt_ai/memory/embedding_model_registry.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace memory {
namespace {

EmbedderSpace Model() {
  return EmbedderSpace{"qwen3-embedding-0.6b@0123456789ab", 256};
}

TEST(EmbedderSpaceTest, Keys) {
  EXPECT_EQ(HashingEmbedderSpace().Key(), "hashing-v3/256");
  EXPECT_EQ(Model().Key(), "qwen3-embedding-0.6b@0123456789ab/256");
  EXPECT_EQ(HashingEmbedder().space(), HashingEmbedderSpace());
  EXPECT_FALSE(HashingEmbedderSpace() == Model());
}

TEST(EmbedderSpaceTest, HashingVersions) {
  EXPECT_EQ(HashingVersionOfSpaceKey("hashing-v3/256"), 3);
  EXPECT_EQ(HashingVersionOfSpaceKey("hashing-v12/256"), 12);
  EXPECT_EQ(HashingVersionOfSpaceKey("hashing-v/256"), 0);
  EXPECT_EQ(HashingVersionOfSpaceKey("hashing-v3"), 0);
  EXPECT_EQ(HashingVersionOfSpaceKey("hashing-v3x/256"), 0);
  EXPECT_EQ(HashingVersionOfSpaceKey("qwen3-embedding-0.6b@abc/256"), 0);
  EXPECT_EQ(HashingVersionOfSpaceKey(""), 0);
  EXPECT_EQ(HashingVersionOfSpaceKey("hashing-v99999999999999/256"), 0);
}

TEST(PlanEmbedderSpaceTest, AlreadyThere) {
  const std::string hashing = HashingEmbedderSpace().Key();
  EXPECT_EQ(PlanEmbedderSpace(hashing, "", HashingEmbedderSpace(), true),
            SpaceAction::kNone);
  EXPECT_EQ(PlanEmbedderSpace(Model().Key(), "", Model(), false),
            SpaceAction::kNone);
}

// The model was removed while its space was being built: drop the
// half-built space and stay.
TEST(PlanEmbedderSpaceTest, AbortsAnUnwantedMigration) {
  const std::string hashing = HashingEmbedderSpace().Key();
  EXPECT_EQ(PlanEmbedderSpace(hashing, Model().Key(), HashingEmbedderSpace(),
                              true),
            SpaceAction::kAbortMigration);
}

// Model -> hashing (model removed or unusable): re-embed now, it's cheap.
TEST(PlanEmbedderSpaceTest, SwitchesToHashingAtOnce) {
  EXPECT_EQ(PlanEmbedderSpace(Model().Key(), "", HashingEmbedderSpace(), true),
            SpaceAction::kSwitchNow);
  // An older hashing version is just another space.
  EXPECT_EQ(PlanEmbedderSpace("hashing-v2/256", "", HashingEmbedderSpace(),
                              true),
            SpaceAction::kSwitchNow);
}

// Hashing -> model: in the background, resuming a migration under way.
TEST(PlanEmbedderSpaceTest, MigratesToTheModelInTheBackground) {
  const std::string hashing = HashingEmbedderSpace().Key();
  EXPECT_EQ(PlanEmbedderSpace(hashing, "", Model(), false),
            SpaceAction::kMigrate);
  EXPECT_EQ(PlanEmbedderSpace(hashing, Model().Key(), Model(), false),
            SpaceAction::kContinueMigration);
  // A migration to another (re-pinned) file starts over.
  EXPECT_EQ(PlanEmbedderSpace(hashing, "qwen3-embedding-0.6b@ffffffffffff/256",
                              Model(), false),
            SpaceAction::kMigrate);
}

TEST(EmbeddingModelRegistryTest, FailsClosedUntilPinned) {
  const EmbeddingModelSpec& spec = DefaultEmbeddingModel();
  // This build carries no pin (see the TODO in the registry): the model
  // must not be offered. When the owner pins it, this flips with it.
  const bool pinned = std::string(spec.sha256).size() == 64;
  EXPECT_EQ(IsPinned(spec), pinned);

  EmbeddingModelSpec s = spec;
  s.revision = "0123456789abcdef0123456789abcdef01234567";
  s.sha256 =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  s.file_size_bytes = 639150592;
  EXPECT_TRUE(IsPinned(s));
  EXPECT_EQ(SpaceOf(s).Key(), "qwen3-embedding-0.6b@0123456789ab/256");

  EmbeddingModelSpec no_size = s;
  no_size.file_size_bytes = 0;
  EXPECT_FALSE(IsPinned(no_size));
  EmbeddingModelSpec no_hash = s;
  no_hash.sha256 = "";
  EXPECT_FALSE(IsPinned(no_hash));
  EmbeddingModelSpec upper = s;
  upper.sha256 =
      "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef";
  EXPECT_FALSE(IsPinned(upper));
  EmbeddingModelSpec short_rev = s;
  short_rev.revision = "main";
  EXPECT_FALSE(IsPinned(short_rev));
}

TEST(EmbeddingModelRegistryTest, FitsTheIndex) {
  const EmbeddingModelSpec& spec = DefaultEmbeddingModel();
  EXPECT_EQ(spec.dimensions, kEmbeddingDim);
  EXPECT_GE(spec.native_dimensions, spec.dimensions);
  EXPECT_GE(spec.max_tokens_per_text, 8u);
  EXPECT_LE(spec.max_tokens_per_text, 2048u);
  EXPECT_LE(std::string(spec.terminator).size(), 64u);
  EXPECT_EQ(std::string(spec.license_name), "Apache License 2.0");
  EXPECT_EQ(std::string(spec.license_url).compare(0, 8, "https://"), 0);
}

}  // namespace
}  // namespace memory
}  // namespace molt_ai
