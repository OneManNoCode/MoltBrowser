// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "net/socket/molt_connection_budget.h"

namespace net {

// static
MoltTorCapacity*& MoltTorCapacity::SharedInstance() {
  static MoltTorCapacity* instance = nullptr;
  return instance;
}

}  // namespace net
