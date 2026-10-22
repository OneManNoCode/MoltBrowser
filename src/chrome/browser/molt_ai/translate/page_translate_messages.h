// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What the Translate bubble's footnote says about on-device translation:
// that it runs on this device, its progress (with Stop), and why it failed
// (with "Choose a model" when there is none). Plain text; the view shows it
// in a label. Standard library only, tested in molt_ai_unittests.
// Design: docs/design/PAGE_TRANSLATION.md §4.5.

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_MESSAGES_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_MESSAGES_H_

#include <cstddef>
#include <optional>
#include <string>

#include "chrome/browser/molt_ai/translate/page_translate_plan.h"

namespace molt_ai::page_translate {

enum class Phase {
  // Nothing running: the bubble offers to translate.
  kIdle,
  // Reading the page and loading the model.
  kPreparing,
  kTranslating,
  // The page is (partly) translated.
  kDone,
  // Nothing was changed.
  kFailed,
};

struct StatusSnapshot {
  Phase phase = Phase::kIdle;
  // kDone and kFailed.
  Report report;
  size_t failed_units = 0;
  bool truncated = false;
  // kTranslating: pieces settled, of how many.
  size_t settled = 0;
  size_t total = 0;
  // kIdle: what this device can do, once known.
  std::optional<bool> model_installed;
  bool device_supported = true;
  // A MoltNet window.
  bool moltnet = false;
};

enum class FootnoteLink {
  kNone,
  // Opens molt://ai, the model picker. Nothing is downloaded from here.
  kChooseModel,
  // Stops the translation; what is translated stays.
  kStop,
};

struct Footnote {
  std::string text;
  FootnoteLink link = FootnoteLink::kNone;
  std::string link_text;
};

Footnote FootnoteFor(const StatusSnapshot& status);

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_MESSAGES_H_
