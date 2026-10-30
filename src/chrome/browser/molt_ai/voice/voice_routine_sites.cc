// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_routine_sites.h"

#include <utility>

namespace molt_ai {
namespace voice {

namespace {

// Without the microphone (the voice gesture, §7.3, can't be taken):
// confirming a routine's sites needs a real click in the Agent studio.
class StudioOnlyConfirmer : public RoutineSiteConfirmer {
 public:
  void ConfirmSites(content::WebContents* contents,
                    const std::string& routine_name,
                    const std::vector<std::string>& sites,
                    Done done) override {
    std::move(done).Run(false, "Confirm it in the Agent studio for now.");
  }
};

}  // namespace

std::unique_ptr<RoutineSiteConfirmer> CreateRoutineSiteConfirmer() {
  return std::make_unique<StudioOnlyConfirmer>();
}

}  // namespace voice
}  // namespace molt_ai
