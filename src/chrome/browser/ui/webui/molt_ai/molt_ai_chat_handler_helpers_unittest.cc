// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"

#include <string>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace chat_handler_helpers {
namespace {

TEST(MoltWebUIArgsTest, AcceptsCallbackIdAndEnoughArgs) {
  base::ListValue args;
  args.Append("vaultList_1");
  EXPECT_TRUE(HasCallbackArgs(args, 1u, "Test"));

  args.Append("entry");
  EXPECT_TRUE(HasCallbackArgs(args, 2u, "Test"));
}

TEST(MoltWebUIArgsTest, RejectsEmptyOrShortLists) {
  base::ListValue args;
  EXPECT_FALSE(HasCallbackArgs(args, 0u, "Test"));
  EXPECT_FALSE(HasCallbackArgs(args, 1u, "Test"));

  args.Append("cb_1");
  EXPECT_FALSE(HasCallbackArgs(args, 2u, "Test"));
}

TEST(MoltWebUIArgsTest, RejectsNonStringCallbackId) {
  base::ListValue ints;
  ints.Append(7);
  EXPECT_FALSE(HasCallbackArgs(ints, 1u, "Test"));

  base::ListValue dicts;
  dicts.Append(base::DictValue());
  dicts.Append("x");
  EXPECT_FALSE(HasCallbackArgs(dicts, 2u, "Test"));

  base::ListValue nulls;
  nulls.Append(base::Value());
  EXPECT_FALSE(HasCallbackArgs(nulls, 1u, "Test"));
}

TEST(MoltWebUIArgsTest, RejectsOversizedCallbackId) {
  base::ListValue args;
  args.Append(std::string(kMaxCallbackIdLength, 'a'));
  EXPECT_TRUE(HasCallbackArgs(args, 1u, "Test"));

  base::ListValue too_long;
  too_long.Append(std::string(kMaxCallbackIdLength + 1, 'a'));
  EXPECT_FALSE(HasCallbackArgs(too_long, 1u, "Test"));
}

TEST(MoltWebUIArgsTest, StringArgNeverChecks) {
  base::ListValue args;
  args.Append("cb");
  args.Append(42);
  args.Append("value");

  EXPECT_EQ(StringArg(args, 0), "cb");
  EXPECT_EQ(StringArg(args, 1), "");            // wrong type
  EXPECT_EQ(StringArg(args, 1, "dflt"), "dflt");
  EXPECT_EQ(StringArg(args, 2), "value");
  EXPECT_EQ(StringArg(args, 3), "");            // out of range
  EXPECT_EQ(StringArg(args, 99, "x"), "x");
}

}  // namespace
}  // namespace chat_handler_helpers
}  // namespace molt_ai
