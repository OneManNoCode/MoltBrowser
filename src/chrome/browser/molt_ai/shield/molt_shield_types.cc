// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_types.h"

namespace molt_ai::shield {

CheckInput::CheckInput() = default;
CheckInput::CheckInput(const CheckInput&) = default;
CheckInput::CheckInput(CheckInput&&) = default;
CheckInput& CheckInput::operator=(const CheckInput&) = default;
CheckInput& CheckInput::operator=(CheckInput&&) = default;
CheckInput::~CheckInput() = default;

CheckResult::CheckResult() = default;
CheckResult::CheckResult(const CheckResult&) = default;
CheckResult::CheckResult(CheckResult&&) = default;
CheckResult& CheckResult::operator=(const CheckResult&) = default;
CheckResult& CheckResult::operator=(CheckResult&&) = default;
CheckResult::~CheckResult() = default;

CosmeticData::CosmeticData() = default;
CosmeticData::CosmeticData(const CosmeticData&) = default;
CosmeticData::CosmeticData(CosmeticData&&) = default;
CosmeticData& CosmeticData::operator=(const CosmeticData&) = default;
CosmeticData& CosmeticData::operator=(CosmeticData&&) = default;
CosmeticData::~CosmeticData() = default;

ListStatus::ListStatus() = default;
ListStatus::ListStatus(const ListStatus&) = default;
ListStatus& ListStatus::operator=(const ListStatus&) = default;
ListStatus::~ListStatus() = default;

}  // namespace molt_ai::shield
