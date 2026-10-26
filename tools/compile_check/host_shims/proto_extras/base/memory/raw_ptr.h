// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// compile_check STAND-IN for //base in a host tool (see base/check.h).

#ifndef COMPILE_CHECK_SHIM_BASE_MEMORY_RAW_PTR_H_
#define COMPILE_CHECK_SHIM_BASE_MEMORY_RAW_PTR_H_

namespace base {

// A plain pointer (raw_ptr only adds checks at run time).
template <typename T>
using raw_ptr = T*;

}  // namespace base

using base::raw_ptr;

#endif  // COMPILE_CHECK_SHIM_BASE_MEMORY_RAW_PTR_H_
