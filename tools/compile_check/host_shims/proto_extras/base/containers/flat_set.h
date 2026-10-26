// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// compile_check STAND-IN for //base in a host tool (see base/check.h).
// base::flat_set iterates in sorted order without duplicates, as std::set.

#ifndef COMPILE_CHECK_SHIM_BASE_CONTAINERS_FLAT_SET_H_
#define COMPILE_CHECK_SHIM_BASE_CONTAINERS_FLAT_SET_H_

#include <functional>
#include <set>

namespace base {

template <class Key, class Compare = std::less<>>
using flat_set = std::set<Key, Compare>;

}  // namespace base

#endif  // COMPILE_CHECK_SHIM_BASE_CONTAINERS_FLAT_SET_H_
