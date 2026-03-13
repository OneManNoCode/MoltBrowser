// Copyright 2025 The MoltBrowser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/molt_ai/omnibox/ai_prompt_provider.h"

#include <string>

#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "third_party/metrics_proto/omnibox_focus_type.pb.h"
#include "third_party/metrics_proto/omnibox_input_type.pb.h"
#include "url/gurl.h"

AiPromptProvider::AiPromptProvider()
    : AutocompleteProvider(AutocompleteProvider::TYPE_BUILTIN) {}

AiPromptProvider::~AiPromptProvider() = default;

void AiPromptProvider::Start(const AutocompleteInput& input,
                             bool minimal_changes) {
  matches_.clear();

  // Do not provide suggestions for zero-suggest (on-focus with no input) or
  // empty input.
  if (input.IsZeroSuggest() ||
      input.type() == metrics::OmniboxInputType::EMPTY) {
    return;
  }

  std::u16string prompt;
  if (!ExtractPrompt(input.text(), &prompt)) {
    return;
  }

  // Only show the suggestion when there is actual prompt text after the prefix.
  if (prompt.empty()) {
    return;
  }

  AddAiMatch(prompt);
  done_ = true;
}

// static
bool AiPromptProvider::ExtractPrompt(const std::u16string& input_text,
                                     std::u16string* prompt) {
  // Check for the "@ai " prefix (case-insensitive).
  const std::u16string lower_input = base::ToLowerASCII(input_text);
  const std::u16string lower_at_ai_prefix = base::ToLowerASCII(
      std::u16string(kAtAiPrefix));

  if (base::StartsWith(lower_input, lower_at_ai_prefix,
                        base::CompareCase::SENSITIVE)) {
    // The prefix length is always the same regardless of case.
    *prompt = input_text.substr(lower_at_ai_prefix.size());
    return true;
  }

  // Check for the "?" prefix. Require at least one character after "?" so we
  // do not trigger on a bare question mark (which the user may intend as a
  // normal query).
  if (base::StartsWith(input_text, kQuestionPrefix,
                        base::CompareCase::SENSITIVE)) {
    *prompt = input_text.substr(1);
    return true;
  }

  return false;
}

void AiPromptProvider::AddAiMatch(const std::u16string& prompt) {
  const std::string prompt_utf8 = base::UTF16ToUTF8(prompt);
  const std::string escaped_prompt =
      base::EscapeQueryParamValue(prompt_utf8, /*use_plus=*/true);
  const std::string destination =
      std::string("chrome://molt-ai/?q=") + escaped_prompt;

  AutocompleteMatch match(this, kRelevance, /*deletable=*/false,
                          AutocompleteMatchType::NAVSUGGEST);
  match.destination_url = GURL(destination);
  match.contents =
      u"Ask MoltBrowser AI: " + prompt;
  match.contents_class = {
      {0, ACMatchClassification::NONE},
  };
  match.description = u"Route to local AI engine";
  match.description_class = {
      {0, ACMatchClassification::DIM},
  };
  match.fill_into_edit = base::UTF8ToUTF16(destination);
  match.allowed_to_be_default_match = true;
  match.suggest_type = omnibox::TYPE_NAVIGATION;

  matches_.push_back(std::move(match));
}
