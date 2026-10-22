// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_locale.h"

#include <memory>
#include <string>

#include "base/synchronization/waitable_event.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/platform/language.h"
#include "third_party/blink/renderer/platform/scheduler/public/non_main_thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread_type.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/text/layout_locale.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink::molt_fingerprinting {
namespace {

// std::string, so nothing of the other thread's string tables crosses over.
struct ThreadResult {
  std::string default_language;
  std::string preferred_language;
  std::string layout_locale;
};

void ReadLanguagesOnThread(ThreadResult* result, base::WaitableEvent* done) {
  result->default_language = DefaultLanguage().Utf8();
  const Vector<AtomicString> preferred = UserPreferredLanguages();
  if (preferred.size() == 1) {
    result->preferred_language = preferred[0].Utf8();
  }
  // This thread's LayoutLocale data is fresh: the default is computed now.
  result->layout_locale = LayoutLocale::GetDefault().LocaleString().Utf8();
  done->Signal();
}

class MoltNetLocaleTest : public testing::Test {
 protected:
  void SetUp() override { LayoutLocale::ClearForTesting(); }
  void TearDown() override {
    UnpinTextLocaleForTesting();
    LayoutLocale::ClearForTesting();
  }

  test::TaskEnvironment task_environment_;
};

TEST_F(MoltNetLocaleTest, PinsDefaultAndPreferredLanguage) {
  PinUniformTextLocale();
  EXPECT_EQ(DefaultLanguage(), kUniformLocale);
  const Vector<AtomicString> preferred = UserPreferredLanguages();
  ASSERT_EQ(preferred.size(), 1u);
  EXPECT_EQ(preferred[0], kUniformLocale);
}

// Text without a lang attribute: en-US, which has no Han script of its own,
// so CJK glyph variants no longer follow a Chinese, Japanese or Korean UI.
TEST_F(MoltNetLocaleTest, DefaultTextLocaleIsEnUs) {
  PinUniformTextLocale();
  const LayoutLocale& locale = LayoutLocale::GetDefault();
  EXPECT_EQ(locale.LocaleString(), kUniformLocale);
  EXPECT_FALSE(locale.HasScriptForHan());
}

// Workers and worklets read DefaultLanguage() on their own threads.
TEST_F(MoltNetLocaleTest, OtherThreadsSeeThePinnedLanguage) {
  PinUniformTextLocale();
  std::unique_ptr<NonMainThread> thread = NonMainThread::CreateThread(
      ThreadCreationParams(ThreadType::kTestThread));
  ThreadResult result;
  base::WaitableEvent done;
  PostCrossThreadTask(*thread->GetTaskRunner(), FROM_HERE,
                      CrossThreadBindOnce(&ReadLanguagesOnThread,
                                          CrossThreadUnretained(&result),
                                          CrossThreadUnretained(&done)));
  done.Wait();
  thread.reset();
  EXPECT_EQ(result.default_language, kUniformLocale);
  EXPECT_EQ(result.preferred_language, kUniformLocale);
  EXPECT_EQ(result.layout_locale, kUniformLocale);
}

TEST_F(MoltNetLocaleTest, UnpinRestoresThePlatformLanguage) {
  const AtomicString platform_language = DefaultLanguage();
  PinUniformTextLocale();
  UnpinTextLocaleForTesting();
  EXPECT_EQ(DefaultLanguage(), platform_language);
}

// Upstream tests that emulate a UI language keep working.
TEST_F(MoltNetLocaleTest, TestingOverrideStillWins) {
  PinUniformTextLocale();
  OverrideUserPreferredLanguagesForTesting({AtomicString("ja-JP")});
  EXPECT_EQ(DefaultLanguage(), "ja-JP");
  OverrideUserPreferredLanguagesForTesting({});
  EXPECT_EQ(DefaultLanguage(), kUniformLocale);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
