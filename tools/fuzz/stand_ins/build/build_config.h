// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's build/build_config.h, for
// tools/fuzz/molt_fuzz.py only: BUILDFLAG(IS_xxx) for the Linux host the
// fuzzers run on.

#ifndef MOLT_FUZZ_STAND_INS_BUILD_BUILD_CONFIG_H_
#define MOLT_FUZZ_STAND_INS_BUILD_BUILD_CONFIG_H_

#define BUILDFLAG_CAT_INDIRECT(a, b) a##b
#define BUILDFLAG_CAT(a, b) BUILDFLAG_CAT_INDIRECT(a, b)
#define BUILDFLAG(flag) (BUILDFLAG_CAT(BUILDFLAG_INTERNAL_, flag)())

#define BUILDFLAG_INTERNAL_IS_LINUX() (1)
#define BUILDFLAG_INTERNAL_IS_POSIX() (1)
#define BUILDFLAG_INTERNAL_IS_MAC() (0)
#define BUILDFLAG_INTERNAL_IS_WIN() (0)
#define BUILDFLAG_INTERNAL_IS_ANDROID() (0)
#define BUILDFLAG_INTERNAL_IS_IOS() (0)
#define BUILDFLAG_INTERNAL_IS_CHROMEOS() (0)
#define BUILDFLAG_INTERNAL_IS_FUCHSIA() (0)
#define BUILDFLAG_INTERNAL_IS_APPLE() (0)

#endif  // MOLT_FUZZ_STAND_INS_BUILD_BUILD_CONFIG_H_
