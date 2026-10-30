// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_planner.h"

namespace molt_ai {
namespace voice {

PlannerStep::PlannerStep() = default;
PlannerStep::~PlannerStep() = default;
PlannerStep::PlannerStep(const PlannerStep&) = default;
PlannerStep& PlannerStep::operator=(const PlannerStep&) = default;
PlannerStep::PlannerStep(PlannerStep&&) = default;
PlannerStep& PlannerStep::operator=(PlannerStep&&) = default;

}  // namespace voice
}  // namespace molt_ai
