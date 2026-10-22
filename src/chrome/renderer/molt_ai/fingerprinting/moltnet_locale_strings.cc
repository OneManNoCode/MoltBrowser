// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/renderer/molt_ai/fingerprinting/moltnet_locale_strings.h"

#include <string>

#include "ui/base/resource/resource_bundle.h"

namespace molt_ai::fingerprinting {

size_t PinMoltNetLocaleStrings() {
  if (!ui::ResourceBundle::HasSharedInstance()) {
    return 0;
  }
  ui::ResourceBundle& bundle = ui::ResourceBundle::GetSharedInstance();
  size_t pinned = 0;
  for (const MoltNetLocaleString& string : MoltNetLocaleStrings()) {
    // Checked before the locale paks by GetLocalizedString(); kept until
    // ReloadLocaleResources(), which renderers never call.
    bundle.OverrideLocaleStringResource(string.id,
                                        std::u16string(string.en_us));
    ++pinned;
  }
  return pinned;
}

}  // namespace molt_ai::fingerprinting
