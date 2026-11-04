// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_home_dir_test_util.h"

#include "base/check.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/molt_ai/common/molt_home_dir.h"

namespace molt_ai {

ScopedMoltHomeDirForTesting::ScopedMoltHomeDirForTesting() {
  // Made with the fixture, before the browser's thread restrictions; ended
  // after them, on the main thread.
  base::ScopedAllowBlockingForTesting allow_blocking;
  CHECK(dir_.CreateUniqueTempDir());
  SetMoltHomeDirForTesting(dir_.GetPath());
}

ScopedMoltHomeDirForTesting::~ScopedMoltHomeDirForTesting() {
  SetMoltHomeDirForTesting(base::FilePath());
  base::ScopedAllowBlockingForTesting allow_blocking;
  // A task still writing there may keep it a moment; the temporary
  // directory is the test's either way.
  if (!dir_.Delete()) {
    dir_.Take();
  }
}

}  // namespace molt_ai
