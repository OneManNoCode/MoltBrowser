// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// compile_check STAND-IN for //base in a host tool (see base/check.h).

#ifndef COMPILE_CHECK_SHIM_BASE_NOTREACHED_H_
#define COMPILE_CHECK_SHIM_BASE_NOTREACHED_H_

#include "base/check.h"

#define NOTREACHED() \
  ::compile_check_shim::FatalMessage(__FILE__, __LINE__, "NOTREACHED").stream()

#endif  // COMPILE_CHECK_SHIM_BASE_NOTREACHED_H_
