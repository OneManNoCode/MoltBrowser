// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_cpu.h"

#include <atomic>

#include "gin/public/isolate_holder.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_memory_values.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "third_party/skia/modules/skcms/skcms.h"
#include "ui/gfx/geometry/molt_math.h"
#include "v8/include/v8-initialization.h"

namespace blink::molt_fingerprinting {

void PinUniformMachineBeforeV8() {
  static constinit std::atomic<bool> pinned{false};
  if (pinned.exchange(true)) {
    return;
  }
  // Before v8::V8::Initialize(), which freezes the flags. gin applies the
  // --js-flags switch after these, so a developer's explicit flags win.
  v8::V8::SetFlagsFromString(kMoltNetV8Flags.data(), kMoltNetV8Flags.size());
  // Before the first isolate (blink::Initialize creates the main thread's).
  gin::IsolateHolder::SetPhysicalMemoryForIsolates(
      moltnet::kV8HeapPhysicalMemory);
  // Before the first transform: skcms picks its code once, at the first one.
  skcms_MoltBrowserEnablePortableMode();
  // Before anything is drawn (no raster thread exists yet).
  SkGraphics::MoltBrowserUsePortableRasterPipeline();
  // Before Blink computes anything a page can read (no thread exists yet).
  gfx::molt_math::SetPortable(true);
}

}  // namespace blink::molt_fingerprinting
