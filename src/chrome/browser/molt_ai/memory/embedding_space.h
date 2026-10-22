// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which vector space AI memory should be in, and how to get there
// (docs/design/SEMANTIC_SEARCH.md). Std-only and unit-tested.
//
// MemoryStorage keeps every chunk's vector in one "committed" space
// (chunks.embedding) and, while it switches to an asynchronous embedder,
// builds the vectors of the "next" space beside them. Search always uses
// the committed space; vectors of two spaces are never mixed.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_SPACE_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_SPACE_H_

#include <string>

#include "chrome/browser/molt_ai/memory/memory_embedder.h"

namespace molt_ai {
namespace memory {

enum class SpaceAction {
  // Already there (and nothing half-built).
  kNone,
  // Already there; drop the half-built next space.
  kAbortMigration,
  // Re-embed every chunk with the (synchronous) desired embedder now, in
  // one transaction, and commit its space. Cheap for HashingEmbedder.
  kSwitchNow,
  // Build the desired (asynchronous) embedder's space in the background,
  // from scratch; commit it once every chunk has a vector there.
  kMigrate,
  // The background migration to the desired space is under way.
  kContinueMigration,
};

// |committed_key| and |next_key| are MemoryStorage's spaces ("" = no
// migration); |desired| is the embedder that should be in use.
SpaceAction PlanEmbedderSpace(const std::string& committed_key,
                              const std::string& next_key,
                              const EmbedderSpace& desired,
                              bool desired_is_synchronous);

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_EMBEDDING_SPACE_H_
