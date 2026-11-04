// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_screen_mirroring.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

class CountingObserver : public ScreenMirroringObserver {
 public:
  void OnScreenMirroringChanged() override {
    ++changes;
    mirroring = IsMirroringScreen();
  }

  int changes = 0;
  bool mirroring = false;
};

// Mirroring lasts while any session does; observers hear it begin and end,
// not each session.
TEST(MoltScreenMirroringTest, WhileAnySessionMirrors) {
  CountingObserver observer;
  AddScreenMirroringObserver(&observer);
  EXPECT_FALSE(IsMirroringScreen());
  int first = 0;
  int second = 0;
  NoteScreenMirroring(&first, true);
  EXPECT_TRUE(IsMirroringScreen());
  EXPECT_EQ(observer.changes, 1);
  EXPECT_TRUE(observer.mirroring);
  // Noted twice, and a second session: no change.
  NoteScreenMirroring(&first, true);
  NoteScreenMirroring(&second, true);
  EXPECT_EQ(observer.changes, 1);
  NoteScreenMirroring(&first, false);
  EXPECT_TRUE(IsMirroringScreen());
  EXPECT_EQ(observer.changes, 1);
  NoteScreenMirroring(&second, false);
  EXPECT_FALSE(IsMirroringScreen());
  EXPECT_EQ(observer.changes, 2);
  EXPECT_FALSE(observer.mirroring);
  // Ended without having begun, or no session: no change.
  NoteScreenMirroring(&first, false);
  NoteScreenMirroring(nullptr, true);
  EXPECT_FALSE(IsMirroringScreen());
  EXPECT_EQ(observer.changes, 2);
  RemoveScreenMirroringObserver(&observer);
  NoteScreenMirroring(&first, true);
  EXPECT_EQ(observer.changes, 2);
  NoteScreenMirroring(&first, false);
}

}  // namespace
}  // namespace molt_ai
