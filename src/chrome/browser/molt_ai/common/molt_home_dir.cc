// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_home_dir.h"

#include "base/base_paths.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"

namespace molt_ai {

namespace {

// The test's directory (read from any sequence).
struct ForTesting {
  base::Lock lock;
  base::FilePath dir;
};

ForTesting& TestDir() {
  static base::NoDestructor<ForTesting> for_testing;
  return *for_testing;
}

}  // namespace

std::optional<base::FilePath> MoltHomeDir() {
  {
    ForTesting& test = TestDir();
    base::AutoLock hold(test.lock);
    if (!test.dir.empty()) {
      return test.dir;
    }
  }
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return std::nullopt;
  }
  return home.Append(FILE_PATH_LITERAL(".moltbrowser"));
}

void SetMoltHomeDirForTesting(const base::FilePath& dir) {
  ForTesting& test = TestDir();
  base::AutoLock hold(test.lock);
  test.dir = dir;
}

}  // namespace molt_ai
