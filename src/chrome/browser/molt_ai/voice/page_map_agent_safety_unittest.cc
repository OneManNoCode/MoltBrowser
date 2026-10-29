// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The page map marks credential fields as agent_safety does: for every
// input type and autocomplete value, ClassifyCredential() agrees with
// agent_safety::ClassifyField() (the map adds fields a label names).

#include <string>

#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

CredentialKind FromFieldKind(agent_safety::FieldKind kind) {
  switch (kind) {
    case agent_safety::FieldKind::kOrdinary:
      return CredentialKind::kNone;
    case agent_safety::FieldKind::kPassword:
      return CredentialKind::kPassword;
    case agent_safety::FieldKind::kPaymentCard:
      return CredentialKind::kPaymentCard;
    case agent_safety::FieldKind::kOneTimeCode:
      return CredentialKind::kOneTimeCode;
  }
  return CredentialKind::kNone;
}

TEST(PageMapAgentSafetyTest, CredentialFieldsMatchAgentSafety) {
  const char* const kTypes[] = {"",         "text",     "password", "PASSWORD",
                                " password ", "email", "tel",      "search",
                                "number",   "hidden"};
  const char* const kAutocompletes[] = {
      "",
      "username",
      "current-password",
      "new-password",
      "NEW-PASSWORD",
      "cc-number",
      "billing cc-csc",
      "section-a shipping cc-exp",
      "one-time-code",
      "one-time-code cc-number",
      "cc-name current-password",
      "email",
      "off",
      "password",
      "ccnumber",
      "\tcc-number\n"};
  for (const char* type : kTypes) {
    for (const char* autocomplete : kAutocompletes) {
      SCOPED_TRACE(std::string(type) + " / " + autocomplete);
      PageNode node;
      node.role = PageRole::kTextField;
      node.input_type = type;
      node.autocomplete = autocomplete;
      EXPECT_EQ(ClassifyCredential(node),
                FromFieldKind(agent_safety::ClassifyField(type, autocomplete)));
    }
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
