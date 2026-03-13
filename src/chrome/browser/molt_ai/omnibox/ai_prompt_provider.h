// Copyright 2025 The MoltBrowser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_OMNIBOX_AI_PROMPT_PROVIDER_H_
#define CHROME_BROWSER_MOLT_AI_OMNIBOX_AI_PROMPT_PROVIDER_H_

#include <string>

#include "components/omnibox/browser/autocomplete_provider.h"

class AutocompleteInput;

// An AutocompleteProvider that detects AI prompts in the omnibox and offers
// to route them to the local MoltBrowser AI engine. The provider recognizes
// two trigger prefixes:
//   - "@ai " followed by a prompt (e.g. "@ai summarize this page")
//   - "?"   followed by a prompt (e.g. "?what is the speed of light")
//
// When a trigger prefix is detected, the provider produces a single
// AutocompleteMatch with high relevance (1500) that navigates to the
// chrome://molt-ai/ WebUI with the prompt text as a query parameter.
class AiPromptProvider : public AutocompleteProvider {
 public:
  AiPromptProvider();

  AiPromptProvider(const AiPromptProvider&) = delete;
  AiPromptProvider& operator=(const AiPromptProvider&) = delete;

  // AutocompleteProvider:
  void Start(const AutocompleteInput& input, bool minimal_changes) override;

 private:
  ~AiPromptProvider() override;

  // The relevance score assigned to AI prompt matches. Set high so the
  // suggestion appears at the top of the omnibox dropdown.
  static constexpr int kRelevance = 1500;

  // The "@ai " prefix that triggers the provider (case-insensitive).
  static constexpr char16_t kAtAiPrefix[] = u"@ai ";

  // The "?" prefix that triggers the provider.
  static constexpr char16_t kQuestionPrefix[] = u"?";

  // Extracts the prompt text from |input_text| if it starts with a recognized
  // trigger prefix. Returns true and sets |prompt| to the extracted text if a
  // prefix was found; returns false otherwise.
  static bool ExtractPrompt(const std::u16string& input_text,
                            std::u16string* prompt);

  // Creates an AutocompleteMatch for the given |prompt| and appends it to
  // |matches_|.
  void AddAiMatch(const std::u16string& prompt);
};

#endif  // CHROME_BROWSER_MOLT_AI_OMNIBOX_AI_PROMPT_PROVIDER_H_
