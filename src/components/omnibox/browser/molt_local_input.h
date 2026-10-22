// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Omnibox input that must stay on this device: the @memory shortcut
// (molt_memory_query.h) and an "@ai" prompt for the local AI
// (ai_prompt_provider.h). AutocompleteController::ShouldRunProvider() gives
// such input to its own provider alone, so no search engine (suggest
// requests), extension or other provider sees it, and ChromeOmniboxClient
// keeps it from the action predictor and the shortcuts database
// (docs/design/SEMANTIC_SEARCH.md §7).
//
// A leading "?" is NOT local input. Typed first, Chromium's edit model turns
// it into keyword mode for the default search engine (a forced web search,
// OmniboxEditModel::EnterKeywordModeForDefaultSearchProvider()), and
// MoltBrowser keeps that meaning.
//
// Standard library only.

#ifndef COMPONENTS_OMNIBOX_BROWSER_MOLT_LOCAL_INPUT_H_
#define COMPONENTS_OMNIBOX_BROWSER_MOLT_LOCAL_INPUT_H_

#include <cstddef>
#include <optional>
#include <string_view>

namespace molt_omnibox {

// The AI prompt keyword, matched ASCII-case-insensitively.
inline constexpr char16_t kAiPromptKeyword[] = u"@ai";

// Where the prompt starts when |text|, after leading whitespace, is "@ai"
// followed by one whitespace character (Unicode spaces included): just
// after that character. std::nullopt otherwise ("@ai" alone, "@aim ...").
// AiPromptProvider claims exactly this input.
std::optional<size_t> AiPromptStart(std::u16string_view text);

// True iff AiPromptStart() has a value, even with no prompt yet ("@ai ").
bool IsAiPromptInput(std::u16string_view text);

enum class LocalInput {
  kNone,
  // "@memory" alone or with words: only MoltMemoryProvider.
  kMemoryShortcut,
  // "@ai <prompt>": only AiPromptProvider.
  kAiPrompt,
};

LocalInput ClassifyLocalInput(std::u16string_view text);

// ClassifyLocalInput() != kNone.
bool IsLocalOnlyInput(std::u16string_view text);

}  // namespace molt_omnibox

#endif  // COMPONENTS_OMNIBOX_BROWSER_MOLT_LOCAL_INPUT_H_
