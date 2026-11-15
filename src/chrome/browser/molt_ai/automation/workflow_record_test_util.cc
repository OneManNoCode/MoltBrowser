// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/workflow_record_test_util.h"

#include "chrome/browser/molt_ai/automation/workflow_record.h"

namespace molt_ai {
namespace automation {

ScopedWorkflowRecordForTesting::ScopedWorkflowRecordForTesting(
    const base::FilePath& directory,
    const base::FilePath& record_file,
    bool may_seed) {
  WorkflowRecord::ResetForTesting();
  WorkflowRecord::Configure({directory, record_file, may_seed});
}

ScopedWorkflowRecordForTesting::~ScopedWorkflowRecordForTesting() {
  WorkflowRecord::ResetForTesting();
}

}  // namespace automation
}  // namespace molt_ai
