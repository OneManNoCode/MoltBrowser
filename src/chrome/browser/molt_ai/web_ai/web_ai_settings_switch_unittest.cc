// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_settings_switch.h"

#include <optional>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::web_ai {
namespace {

TEST(WebAISettingsSwitchTest, OnUnlessTheSettingsSayOff) {
  // No settings.json: the default.
  EXPECT_TRUE(WebAIApisSwitchFrom(base::DictValue()));
  base::DictValue on;
  on.Set(kWebAIApisSettingsKey, true);
  EXPECT_TRUE(WebAIApisSwitchFrom(std::move(on)));
  base::DictValue off;
  off.Set(kWebAIApisSettingsKey, false);
  EXPECT_FALSE(WebAIApisSwitchFrom(std::move(off)));
  // Not a boolean: the default.
  base::DictValue odd;
  odd.Set(kWebAIApisSettingsKey, "false");
  EXPECT_TRUE(WebAIApisSwitchFrom(std::move(odd)));
}

TEST(WebAISettingsSwitchTest, AFileThatCantBeReadLeavesThemOff) {
  // settings.json exists but isn't a JSON object, or is over the size cap
  // (ReadMoltSettings() returns nullopt): never on by default.
  EXPECT_FALSE(WebAIApisSwitchFrom(std::nullopt));
}

}  // namespace
}  // namespace molt_ai::web_ai
