// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Tests: a workflow record (workflow_record.h) for a directory, as the
// browser configures one at startup, for the object's lifetime. Every
// record is forgotten when it is made and when it ends, so a test starts
// as a browser start does: a storage of |directory| loads the record at
// |record_file| on its first use (seeding when |may_seed| and neither the
// record nor its marker is there).
//
//   ScopedWorkflowRecordForTesting record(dir, udd.Append(
//       kWorkflowRecordFileName), /*may_seed=*/false);

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_WORKFLOW_RECORD_TEST_UTIL_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_WORKFLOW_RECORD_TEST_UTIL_H_

#include "base/files/file_path.h"

namespace molt_ai {
namespace automation {

class ScopedWorkflowRecordForTesting {
 public:
  ScopedWorkflowRecordForTesting(const base::FilePath& directory,
                                 const base::FilePath& record_file,
                                 bool may_seed);
  ScopedWorkflowRecordForTesting(const ScopedWorkflowRecordForTesting&) =
      delete;
  ScopedWorkflowRecordForTesting& operator=(
      const ScopedWorkflowRecordForTesting&) = delete;
  ~ScopedWorkflowRecordForTesting();
};

}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_WORKFLOW_RECORD_TEST_UTIL_H_
