// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/compiler_specific.h (the
// macros MoltBrowser's code uses), for tools/fuzz/molt_fuzz.py only.

#ifndef MOLT_FUZZ_STAND_INS_BASE_COMPILER_SPECIFIC_H_
#define MOLT_FUZZ_STAND_INS_BASE_COMPILER_SPECIFIC_H_

#define UNSAFE_BUFFERS(...) __VA_ARGS__
#define UNSAFE_TODO(...) __VA_ARGS__
#define UNSAFE_BUFFER_USAGE
#define LIFETIME_BOUND [[clang::lifetimebound]]
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define HAS_BUILTIN(x) __has_builtin(x)

#endif  // MOLT_FUZZ_STAND_INS_BASE_COMPILER_SPECIFIC_H_
