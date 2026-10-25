// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_user_click.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

// A frame with transient user activation, as content::RenderFrameHost has
// it: a click (or a key press) gives it, consuming takes it.
struct FakeFrame {
  bool active = false;
  int consumed = 0;
  void Click() { active = true; }
  bool ConsumeTransientUserActivation() {
    const bool was = active;
    active = false;
    consumed += was ? 1 : 0;
    return was;
  }
};

TEST(MoltUserClickTest, NothingWithoutAClick) {
  FakeFrame frame;
  EXPECT_FALSE(ConsumeUserClick(true, &frame));
  EXPECT_FALSE(ConsumeUserClick(true, static_cast<FakeFrame*>(nullptr)));
  frame.Click();
  EXPECT_FALSE(ConsumeUserClick(true, static_cast<FakeFrame*>(nullptr)));
  EXPECT_TRUE(frame.active);
}

// The page's script sends two privileged messages after one click, as it
// could on molt://ai-settings (turning GPU inference on, then retrying a
// driver the browser turned off) or on the onion page (opening a MoltNet
// window twice): only the first goes ahead.
TEST(MoltUserClickTest, OneClickOneAction) {
  FakeFrame frame;
  frame.Click();
  EXPECT_TRUE(ConsumeUserClick(true, &frame));   // gpuInferenceSetMode
  EXPECT_FALSE(ConsumeUserClick(true, &frame));  // gpuInferenceRetry
  EXPECT_EQ(frame.consumed, 1);
  // The user's next click allows the next one.
  frame.Click();
  EXPECT_TRUE(ConsumeUserClick(true, &frame));
  EXPECT_EQ(frame.consumed, 2);
}

// A message that doesn't allow anything (a denial, "Not now") leaves the
// click for what the user does next.
TEST(MoltUserClickTest, NotAllowingConsumesNothing) {
  FakeFrame frame;
  frame.Click();
  EXPECT_FALSE(ConsumeUserClick(false, &frame));
  EXPECT_TRUE(frame.active);
  EXPECT_TRUE(ConsumeUserClick(true, &frame));
}

}  // namespace
}  // namespace molt_ai
