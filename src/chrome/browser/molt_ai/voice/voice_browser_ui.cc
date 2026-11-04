// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_browser_ui.h"

namespace molt_ai {
namespace voice {

namespace {

VoiceBrowserUi* g_ui = nullptr;

}  // namespace

// static
VoiceBrowserUi* VoiceBrowserUi::Get() {
  return g_ui;
}

// static
void VoiceBrowserUi::Install(VoiceBrowserUi* ui) {
  g_ui = ui;
}

}  // namespace voice
}  // namespace molt_ai
