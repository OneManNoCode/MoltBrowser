// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/check.h (and check_op.h),
// used only by tools/fuzz/molt_fuzz.py. The GN fuzzer_test targets build
// against the real //base. CHECK(cond) << "..." prints the condition and
// aborts, which libFuzzer reports as a crash.

#ifndef MOLT_FUZZ_STAND_INS_BASE_CHECK_H_
#define MOLT_FUZZ_STAND_INS_BASE_CHECK_H_

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace molt_fuzz_stand_in {

class CheckFailure {
 public:
  CheckFailure(const char* file, int line, const char* condition) {
    stream_ << file << ":" << line << ": Check failed: " << condition << ". ";
  }
  CheckFailure(const CheckFailure&) = delete;
  CheckFailure& operator=(const CheckFailure&) = delete;
  [[noreturn]] ~CheckFailure() {
    std::fprintf(stderr, "%s\n", stream_.str().c_str());
    std::fflush(stderr);
    std::abort();
  }
  std::ostream& stream() { return stream_; }

 private:
  std::ostringstream stream_;
};

}  // namespace molt_fuzz_stand_in

#define CHECK(condition)                                                \
  if (condition) {                                                      \
  } else /* NOLINT */                                                   \
    ::molt_fuzz_stand_in::CheckFailure(__FILE__, __LINE__, #condition) \
        .stream()

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))

#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(a, b) CHECK_EQ(a, b)
#define DCHECK_NE(a, b) CHECK_NE(a, b)
#define DCHECK_LE(a, b) CHECK_LE(a, b)
#define DCHECK_LT(a, b) CHECK_LT(a, b)
#define DCHECK_GE(a, b) CHECK_GE(a, b)
#define DCHECK_GT(a, b) CHECK_GT(a, b)

#define NOTREACHED() CHECK(false)

#endif  // MOLT_FUZZ_STAND_INS_BASE_CHECK_H_
