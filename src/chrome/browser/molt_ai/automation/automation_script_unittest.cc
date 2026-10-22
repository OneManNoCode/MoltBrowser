// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_script.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace {

// Found by molt_automation_script_fuzzer: an imported .molt file's 64-bit
// fields are read back as doubles, and casting one outside int64_t's range
// to int64_t was undefined behaviour. They saturate now.
TEST(AutomationScriptTest, ImportedTimesOutsideInt64Saturate) {
  std::optional<Script> script = Script::FromJSONString(R"({
    "id": "x",
    "created_at_unix": 1e300,
    "trigger": {"type": "cron", "until_unix": 19000214748364800000.0,
                "last_fired_unix": -1e19},
    "security": {"max_network_bytes": 9.3e18},
    "stats": {"last_run_unix": 1e19, "ai_tokens_total": -1e300,
              "run_history": [{"ts": 1.8e19}]}
  })");
  ASSERT_TRUE(script);
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  EXPECT_EQ(script->created_at_unix, kMax);
  EXPECT_EQ(script->trigger.until_unix, kMax);
  EXPECT_EQ(script->trigger.last_fired_unix, kMin);
  EXPECT_EQ(script->security.max_network_bytes, kMax);
  EXPECT_EQ(script->stats.last_run_unix, kMax);
  EXPECT_EQ(script->stats.ai_tokens_total, kMin);
  ASSERT_EQ(script->stats.run_history.size(), 1u);
  EXPECT_EQ(script->stats.run_history[0].timestamp_unix, kMax);
}

TEST(AutomationScriptTest, ImportedTimesInRangeAreKept) {
  std::optional<Script> script = Script::FromJSONString(
      R"({"id": "x", "created_at_unix": 1700000000,
          "trigger": {"until_unix": 4102444800.0}})");
  ASSERT_TRUE(script);
  EXPECT_EQ(script->created_at_unix, 1700000000);
  EXPECT_EQ(script->trigger.until_unix, 4102444800);
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
