// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_messages.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::page_translate {
namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

TEST(PageTranslateMessagesTest, BeforeTranslating) {
  StatusSnapshot status;
  Footnote f = FootnoteFor(status);
  EXPECT_TRUE(Contains(f.text, "on this device"));
  EXPECT_TRUE(Contains(f.text, "isn't sent anywhere"));
  EXPECT_EQ(f.link, FootnoteLink::kNone);

  // A MoltNet window also says what the site can see.
  status.moltnet = true;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "the language you read"));

  // No model: say so, and offer the picker; nothing is downloaded here.
  status.model_installed = false;
  f = FootnoteFor(status);
  EXPECT_TRUE(Contains(f.text, "needs an AI model"));
  EXPECT_TRUE(Contains(f.text, "Nothing is downloaded"));
  EXPECT_EQ(f.link, FootnoteLink::kChooseModel);
  EXPECT_EQ(f.link_text, "Choose a model");

  status.device_supported = false;
  f = FootnoteFor(status);
  EXPECT_TRUE(Contains(f.text, "can't run on-device AI"));
  EXPECT_EQ(f.link, FootnoteLink::kNone);
}

TEST(PageTranslateMessagesTest, WhileTranslatingItCanBeStopped) {
  StatusSnapshot status;
  status.phase = Phase::kPreparing;
  EXPECT_EQ(FootnoteFor(status).link, FootnoteLink::kStop);
  status.phase = Phase::kTranslating;
  status.settled = 12;
  status.total = 80;
  Footnote f = FootnoteFor(status);
  EXPECT_TRUE(Contains(f.text, "12 of 80"));
  EXPECT_EQ(f.link, FootnoteLink::kStop);
  EXPECT_EQ(f.link_text, "Stop");
}

TEST(PageTranslateMessagesTest, AfterTranslating) {
  StatusSnapshot status;
  status.phase = Phase::kDone;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "Machine translation"));
  status.failed_units = 3;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "left as it was"));
  status.truncated = true;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "first part"));
  status.report.problem = Problem::kStopped;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "show the original"));
  status.report.problem = Problem::kNothingToTranslate;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "no text"));
}

TEST(PageTranslateMessagesTest, FailuresSayWhy) {
  StatusSnapshot status;
  status.phase = Phase::kFailed;
  status.report.problem = Problem::kNoModel;
  EXPECT_EQ(FootnoteFor(status).link, FootnoteLink::kChooseModel);
  status.report.problem = Problem::kModelFailed;
  EXPECT_EQ(FootnoteFor(status).link, FootnoteLink::kChooseModel);
  status.report.problem = Problem::kUnknownSource;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "Options"));
  status.report.problem = Problem::kUnsupportedPair;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "covers 20 languages"));
  status.report.problem = Problem::kIdentical;
  EXPECT_TRUE(Contains(FootnoteFor(status).text, "already"));
  for (Problem problem :
       {Problem::kNone, Problem::kNothingToTranslate,
        Problem::kDeviceUnsupported, Problem::kPageUnreadable,
        Problem::kModelErrors, Problem::kStopped, Problem::kTimedOut}) {
    status.report.problem = problem;
    const Footnote f = FootnoteFor(status);
    EXPECT_FALSE(f.text.empty());
    EXPECT_NE(f.link, FootnoteLink::kStop);
  }
}

}  // namespace
}  // namespace molt_ai::page_translate
