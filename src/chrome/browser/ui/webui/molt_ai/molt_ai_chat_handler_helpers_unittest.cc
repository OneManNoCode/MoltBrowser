// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"

#include <optional>
#include <string>
#include <utility>

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

TEST(MoltWebUIArgsTest, BoolArgOnlyTakesBools) {
  base::ListValue args;
  args.Append("cb");
  args.Append(true);
  args.Append(1);
  args.Append("true");
  args.Append(base::Value());
  args.Append(false);
  EXPECT_EQ(BoolArg(args, 1), std::optional<bool>(true));
  EXPECT_EQ(BoolArg(args, 2), std::nullopt);  // 1 is not true
  EXPECT_EQ(BoolArg(args, 3), std::nullopt);
  EXPECT_EQ(BoolArg(args, 4), std::nullopt);
  EXPECT_EQ(BoolArg(args, 5), std::optional<bool>(false));
  EXPECT_EQ(BoolArg(args, 6), std::nullopt);  // out of range
  EXPECT_EQ(BoolArg(base::ListValue(), 0), std::nullopt);
}

// molt://ai-settings "Sites using on-device AI": webAiSitesRemove(cb, site)
// takes the site as BoundedStringArg(args, 1, kMaxSiteKeyBytes).
TEST(MoltWebUIArgsTest, BoundedStringArgRefusesWhatIsntAShortString) {
  constexpr size_t kMax = 1024;
  base::ListValue args;
  args.Append("cb");
  args.Append("https://a.example");
  args.Append(std::string(kMax, 'a'));
  args.Append(std::string(kMax + 1, 'a'));
  args.Append(7);
  base::ListValue nested;
  nested.Append("https://a.example");
  args.Append(std::move(nested));
  args.Append(base::DictValue());
  args.Append(std::string("https://a.example\0evil", 22));
  args.Append("<img src=x onerror=alert(1)>");
  EXPECT_EQ(BoundedStringArg(args, 1, kMax), "https://a.example");
  EXPECT_EQ(BoundedStringArg(args, 2, kMax)->size(), kMax);
  EXPECT_EQ(BoundedStringArg(args, 3, kMax), std::nullopt);  // not cut
  EXPECT_EQ(BoundedStringArg(args, 4, kMax), std::nullopt);
  EXPECT_EQ(BoundedStringArg(args, 5, kMax), std::nullopt);
  EXPECT_EQ(BoundedStringArg(args, 6, kMax), std::nullopt);
  // Strings come back exactly as sent; the caller only matches them
  // against keys it stored (WebAISitePermissions::Remove).
  EXPECT_EQ(BoundedStringArg(args, 7, kMax)->size(), 22u);
  EXPECT_EQ(BoundedStringArg(args, 8, kMax), "<img src=x onerror=alert(1)>");
  EXPECT_EQ(BoundedStringArg(args, 9, kMax), std::nullopt);
}

}  // namespace
}  // namespace chat_handler_helpers
}  // namespace molt_ai
