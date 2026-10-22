// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/embedding_space.h"

namespace molt_ai {
namespace memory {

SpaceAction PlanEmbedderSpace(const std::string& committed_key,
                              const std::string& next_key,
                              const EmbedderSpace& desired,
                              bool desired_is_synchronous) {
  const std::string want = desired.Key();
  if (committed_key == want) {
    return next_key.empty() ? SpaceAction::kNone
                            : SpaceAction::kAbortMigration;
  }
  if (desired_is_synchronous) return SpaceAction::kSwitchNow;
  return next_key == want ? SpaceAction::kContinueMigration
                          : SpaceAction::kMigrate;
}

}  // namespace memory
}  // namespace molt_ai
