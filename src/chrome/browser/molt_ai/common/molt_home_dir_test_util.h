// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Tests: a temporary directory as MoltBrowser's own directory
// (molt_home_dir.h) for the object's lifetime. A browser test fixture holds
// one as a member, so it is in place before the browser starts (the
// fixture is made before SetUp()) and removed after it ends:
//
//   class MyBrowserTest : public InProcessBrowserTest {
//     ...
//     molt_ai::ScopedMoltHomeDirForTesting molt_home_;
//   };

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_MOLT_HOME_DIR_TEST_UTIL_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_MOLT_HOME_DIR_TEST_UTIL_H_

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"

namespace molt_ai {

class ScopedMoltHomeDirForTesting {
 public:
  ScopedMoltHomeDirForTesting();
  ScopedMoltHomeDirForTesting(const ScopedMoltHomeDirForTesting&) = delete;
  ScopedMoltHomeDirForTesting& operator=(const ScopedMoltHomeDirForTesting&) =
      delete;
  ~ScopedMoltHomeDirForTesting();

  // The directory MoltHomeDir() gives meanwhile.
  const base::FilePath& path() const { return dir_.GetPath(); }

 private:
  base::ScopedTempDir dir_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_MOLT_HOME_DIR_TEST_UTIL_H_
