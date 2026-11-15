// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_script.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "base/values.h"
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

// A workflow file changed outside the browser loads with changed_outside
// set (workflow_record.h); stored only when true, so older files and older
// builds see nothing new.
TEST(AutomationScriptTest, ChangedOutsideIsStoredOnlyWhenTrue) {
  Script script;
  script.id = "x";
  const base::DictValue plain = script.ToJSON();
  EXPECT_FALSE(plain.FindDict("security")->FindBool("changed_outside"));
  EXPECT_FALSE(Script::FromJSON(plain)->security.changed_outside);
  script.security.changed_outside = true;
  const base::DictValue changed = script.ToJSON();
  EXPECT_EQ(changed.FindDict("security")->FindBool("changed_outside"), true);
  EXPECT_TRUE(Script::FromJSON(changed)->security.changed_outside);
}

// What the browser writes reads back as the same workflow, field for field
// (the security digest of a file depends on it).
TEST(AutomationScriptTest, ARoundTripIsStable) {
  std::optional<Script> rich = Script::FromJSONString(R"({
    "id": "wf_1", "name": "Prices", "version": 2,
    "created_at_unix": 1800000000, "ai_model": "m", "enabled": false,
    "trigger": {"type": "cron", "expression": "0 9 * * *",
                "timezone": "Europe/Paris", "until_unix": 4102444800,
                "last_fired_unix": 1800000100},
    "steps": [
      {"type": "navigate", "target": "https://shop.example/"},
      {"type": "click", "target": "#buy", "selector_fallbacks": ["#b"],
       "locators": {"origin": "https://shop.example",
                    "element": {"tag": "button", "type": "",
                                "role": "button"},
                    "list": [{"by": "role", "role": "button",
                              "value": "Buy"},
                             {"by": "text", "value": "Buy"}]},
       "timeout_ms": 9000, "retries": 2, "max_iterations": 7,
       "store_as": "v", "transform": "t", "description": "Buy",
       "extra": {"host": "shop.example", "n": 0.5, "deep": [1, {"a": null}]}}
    ],
    "security": {"domain_whitelist": ["shop.example"],
                 "require_approval_for": ["payment"],
                 "max_runtime_seconds": 60, "max_navigations": 3,
                 "max_network_bytes": 1000, "trust": "approved",
                 "sites_unconfirmed": true, "changed_outside": true},
    "stats": {"runs": 3, "successes": 2, "run_history": [{"ts": 1}]},
    "default_variables": {"city": "Paris"}
  })");
  ASSERT_TRUE(rich);
  ASSERT_TRUE(rich->steps[1].locators);
  const base::DictValue once = rich->ToJSON();
  std::optional<Script> again = Script::FromJSON(once);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->ToJSON(), once);
  // And through the file's text.
  std::optional<Script> text = Script::FromJSONString(rich->ToJSONString());
  ASSERT_TRUE(text);
  EXPECT_EQ(text->ToJSON(), once);
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
